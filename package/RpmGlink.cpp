// SPDX-License-Identifier: MIT
// RPM requests over GLINK for the TB128FU (SM6225 "khaje"): the APPS end of the "rpm_requests" channel, polled, so the GPU probe can
// vote the CX rail level and the DDR (BIMC) clock before it raises the GPU clock (GpuProbe.cpp, tb128fu-gpu=9).
//
// As Linux does it (drivers/rpmsg/qcom_glink_rpm.c, qcom_glink_native.c, drivers/soc/qcom/smd-rpm.c; V0 requests, since the stock
// khaje device tree's rpm-smd node has no "rev1" reg): the last 256 bytes of the RPM message RAM (0x45f0000, 0x7000 bytes) hold a
// table ("grt0") naming the APPS->RPM ("ap2r") and RPM->APPS ("r2ap") FIFOs, each a tail, a head and the data. Indices are byte
// offsets into the data, every command is padded to 8 bytes, and the writer advances its head and rings the APCS IPC doorbell
// (0x0F111008, bit 0). The transport is intentless. The RPM's interrupt (SPI 194) is not used: the FIFO is polled while a call waits.
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSSharedPtr.h>
#include <string.h>

extern "C" {
// The boot log's [rpm] rows (BootLog.c): the FIFO table and the indices found before the reset; the handshake and the newest
// commands received; the newest request and its answer.
char tb128fu_rpm_rows[3][120];
int tb128fu_rpm_open(void);
int tb128fu_rpm_request(uint32_t set, uint32_t type, uint32_t id, uint32_t key, uint32_t value);
}

namespace {
constexpr uint64_t kMsgRam = 0x45f0000, kMsgRamSize = 0x7000, kApcs = 0xf111000;
constexpr uint32_t kApcsIpc = 0x8, kTocSize = 256;
constexpr uint32_t kTocMagic = 0x67727430, kTxFifoId = 0x61703272, kRxFifoId = 0x72326170;   // "grt0", "ap2r", "r2ap"
enum : uint32_t {kVersion = 0, kVersionAck = 1, kOpen = 2, kClose = 3, kOpenAck = 4, kTxData = 9, kCloseAck = 11, kTxDataCont = 12,
                 kReadNotif = 13};
constexpr uint32_t kReq = 0x00716572, kMsgId = 0x2367736d, kErr = 0x00727265;            // "req", "msg#", "err"
constexpr char kChannel[] = "rpm_requests";
constexpr uint16_t kLcid = 1;

struct Rpm {
    OSSharedPtr<IOMemoryMap> ramMap, apcsMap;
    uintptr_t ram = 0, apcs = 0;
    uint32_t tx = 0, txLength = 0, rx = 0, rxLength = 0;   // FIFO offsets in the RAM: +0 tail, +4 head, +8 data
    uint32_t tocCount = 0, before[4] = {};                  // tx head/tail, rx head/tail as found
    bool versioned = false, remoteOpen = false, opened = false, ready = false;
    uint16_t rcid = 0;
    uint32_t versionSeen = 0, featuresSeen = 0;
    uint32_t received = 0, recent[8] = {};                  // commands received; cmd | param1 << 16 of the newest
    uint32_t unknown = 0, notifies = 0;
    uint32_t msgId = 0, answeredId = 0;
    bool answered = false;
    char error[40] = {};
    int status = -1;
    IOLock *lock = nullptr;
} g;

uint32_t rd(uint32_t offset) {return *reinterpret_cast<volatile uint32_t *>(g.ram + offset);}
void wr(uint32_t offset, uint32_t value) {*reinterpret_cast<volatile uint32_t *>(g.ram + offset) = value;}
void doorbell() {
    __asm__ volatile("dsb sy" ::: "memory");
    *reinterpret_cast<volatile uint32_t *>(g.apcs + kApcsIpc) = 1u;
    __asm__ volatile("dsb sy" ::: "memory");
}
uint32_t align8(uint32_t n) {return (n + 7) & ~7u;}

bool mapDevice(uint64_t phys, uint64_t length, OSSharedPtr<IOMemoryMap> &map, uintptr_t &va) {
    IODeviceMemory *memory = IODeviceMemory::withRange(phys, length);
    if (!memory) return false;
    map = OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
    memory->release();
    va = map ? static_cast<uintptr_t>(map->getVirtualAddress()) : 0;
    return va != 0;
}

// The APPS->RPM FIFO: whole words, split at the end; at least 8 bytes stay free so a full FIFO never reads as empty.
bool txWrite(const uint32_t *words, uint32_t bytes) {
    uint32_t head = rd(g.tx + 4);
    const uint32_t tail = rd(g.tx);
    if (head >= g.txLength || tail >= g.txLength) return false;
    const uint32_t free = tail <= head ? g.txLength - head + tail : tail - head;
    if (free < bytes + 8) return false;
    for (uint32_t i = 0; i < bytes / 4; ++i) {
        wr(g.tx + 8 + head, words[i]);
        head += 4;
        if (head >= g.txLength) head -= g.txLength;
    }
    __asm__ volatile("dsb sy" ::: "memory");
    wr(g.tx + 4, head);
    doorbell();
    return true;
}
bool txCommand(uint32_t cmd, uint32_t param1, uint32_t param2) {
    const uint32_t words[2] = {cmd | (param1 << 16), param2};
    return txWrite(words, 8);
}

uint32_t rxAvailable() {
    const uint32_t head = rd(g.rx + 4), tail = rd(g.rx);
    if (head >= g.rxLength || tail >= g.rxLength) return 0;
    return head < tail ? g.rxLength - tail + head : head - tail;
}
// Words from the tail plus an offset (a multiple of 4), across the end of the FIFO.
void rxPeek(uint32_t offset, uint32_t *out, uint32_t words) {
    uint32_t at = rd(g.rx) + offset;
    for (uint32_t i = 0; i < words; ++i) {
        while (at >= g.rxLength) at -= g.rxLength;
        out[i] = rd(g.rx + 8 + at);
        at += 4;
    }
}
void rxAdvance(uint32_t bytes) {
    uint32_t tail = rd(g.rx) + bytes;
    while (tail >= g.rxLength) tail -= g.rxLength;
    __asm__ volatile("dsb sy" ::: "memory");
    wr(g.rx, tail);
}

// An SMD RPM answer (V0): the "req" header, then items {type, length, data} stepped by 8 + length rounded up to 4: "msg#" with the
// request's id, "err" with a text when the request failed.
void parseAnswer(const uint32_t *payload, uint32_t bytes) {
    if (bytes < 8 || payload[0] != kReq) {strlcpy(g.error, "bad answer header", sizeof g.error); g.answered = true; return;}
    const uint32_t length = payload[1] + 8 < bytes ? payload[1] + 8 : bytes;
    uint32_t id = 0;
    bool failed = false;
    for (uint32_t at = 8; at + 8 <= length;) {
        const uint32_t type = payload[at / 4], itemBytes = payload[at / 4 + 1];
        if (type == kMsgId && itemBytes >= 4 && at + 12 <= length) id = payload[at / 4 + 2];
        if (type == kErr) {
            failed = true;
            const uint32_t n = itemBytes < sizeof g.error - 1 ? itemBytes : sizeof g.error - 1;
            if (at + 8 + n <= length) {memcpy(g.error, reinterpret_cast<const uint8_t *>(payload) + at + 8, n); g.error[n] = 0;}
        }
        at += 8 + ((itemBytes + 3) & ~3u);
    }
    if (!failed) g.error[0] = 0;
    g.answeredId = id;
    g.answered = true;
    g.status = failed ? -5 : 0;
}

// Handles every complete command in the RPM->APPS FIFO. An unknown command stays in place and stops the pass, as in Linux.
void rxPoll() {
    for (uint32_t guard = 0; guard < 64 && rxAvailable() >= 8; ++guard) {
        uint32_t header[2];
        rxPeek(0, header, 2);
        const uint32_t cmd = header[0] & 0xffff, param1 = header[0] >> 16, param2 = header[1];
        g.recent[g.received % 8] = header[0];
        ++g.received;
        switch (cmd) {
        case kVersion:
            // The RPM's own version: acknowledge version 1 without features. Both ends speaking version 1 is enough to go on.
            rxAdvance(8);
            g.versionSeen = param1; g.featuresSeen = param2;
            txCommand(kVersionAck, 1, 0);
            if (param1 == 1) g.versioned = true;
            break;
        case kVersionAck:
            rxAdvance(8);
            g.versionSeen = param1; g.featuresSeen = param2;
            if (param1 == 1) g.versioned = true;
            else if (param1 > 1) txCommand(kVersion, 1, 0);
            break;
        case kOpen: {
            const uint32_t nameBytes = param2 & 0xffff, total = 8 + align8(nameBytes);
            if (rxAvailable() < total) return;
            uint32_t name[8] = {};
            if (nameBytes <= sizeof name) rxPeek(8, name, (nameBytes + 3) / 4);
            rxAdvance(total);
            if (nameBytes == sizeof kChannel && !memcmp(name, kChannel, sizeof kChannel)) {
                g.rcid = uint16_t(param1);
                g.remoteOpen = true;
                txCommand(kOpenAck, param1, 0);
            }
            break;
        }
        case kOpenAck:
            rxAdvance(8);
            if (param1 == kLcid) g.opened = true;
            break;
        case kClose:
            rxAdvance(8);
            if (param1 == g.rcid) {g.remoteOpen = false; txCommand(kCloseAck, param1, 0);}
            break;
        case kTxData:
        case kTxDataCont: {
            if (rxAvailable() < 16) return;
            uint32_t sizes[4];
            rxPeek(0, sizes, 4);
            const uint32_t chunk = sizes[2], left = sizes[3], total = align8(16 + chunk);
            if (rxAvailable() < total) return;
            uint32_t payload[64] = {};
            const uint32_t copy = chunk < sizeof payload ? chunk : sizeof payload;
            rxPeek(16, payload, (copy + 3) / 4);
            rxAdvance(total);
            if (cmd == kTxData && !left) parseAnswer(payload, copy);
            break;
        }
        case kReadNotif:
            rxAdvance(8);
            ++g.notifies;
            doorbell();
            break;
        default:
            ++g.unknown;
            return;
        }
    }
}

// Polls until the condition holds or the time (ms) runs out.
template <typename Condition> bool waitFor(Condition condition, unsigned ms) {
    for (unsigned i = 0; i <= ms; ++i) {
        rxPoll();
        if (condition()) return true;
        IOSleep(1);
    }
    return false;
}

void rows() {
    snprintf(tb128fu_rpm_rows[0], sizeof tb128fu_rpm_rows[0],
        "RPM toc n%u tx@%x+%x rx@%x+%x | found tx h%x t%x rx h%x t%x | st%d", g.tocCount, g.tx, g.txLength, g.rx, g.rxLength,
        g.before[0], g.before[1], g.before[2], g.before[3], g.status);
    snprintf(tb128fu_rpm_rows[1], sizeof tb128fu_rpm_rows[1],
        "RPM ver%u/%x v%d ropen%d rcid%u open%d | rx n%u unk%u ntf%u | %x %x %x %x", g.versionSeen, g.featuresSeen, g.versioned,
        g.remoteOpen, g.rcid, g.opened, g.received, g.unknown, g.notifies, g.recent[(g.received + 4) % 8],
        g.recent[(g.received + 5) % 8], g.recent[(g.received + 6) % 8], g.recent[(g.received + 7) % 8]);
}
}  // namespace

// Finds the FIFOs, resets the APPS indices (TX head, RX tail) as Linux does, negotiates version 1 and opens "rpm_requests":
// the RPM opens it from its side and is acknowledged, then ours is acknowledged; without the RPM's open within 1 s ours goes first.
// 0 when the channel is open; negative where it stopped: -1 map, -2 table, -3 FIFOs, -4 version, -5 open.
int tb128fu_rpm_open(void) {
    if (g.ready) return 0;
    if (!g.lock) g.lock = IOLockAlloc();
    if (!g.lock) return g.status = -12;
    IOLockLock(g.lock);
    int status = 0;
    do {
        if (!mapDevice(kMsgRam, kMsgRamSize, g.ramMap, g.ram) || !mapDevice(kApcs, 0x1000, g.apcsMap, g.apcs)) {status = -1; break;}
        const uint32_t toc = kMsgRamSize - kTocSize;
        if (rd(toc) != kTocMagic) {status = -2; break;}
        g.tocCount = rd(toc + 4);
        for (uint32_t i = 0; i < g.tocCount && i < (kTocSize - 8) / 12; ++i) {
            const uint32_t id = rd(toc + 8 + 12 * i), offset = rd(toc + 12 + 12 * i), size = rd(toc + 16 + 12 * i);
            // The size is the data's alone, after the tail and head words (qcom_glink_rpm.c: native.length = size, fifo at
            // offset + 8). tb128fu-089..092 took 8 bytes off it, wrapped 8 bytes early, and the first request that crossed the
            // end (the 31st) left the RPM a misplaced message: rpm_err_fatal, whichever request it was.
            if (offset >= kMsgRamSize || size < 8 || size > kMsgRamSize - offset - 8) continue;
            if (id == kTxFifoId) {g.tx = offset; g.txLength = size;}
            if (id == kRxFifoId) {g.rx = offset; g.rxLength = size;}
        }
        if (!g.txLength || !g.rxLength || (g.txLength & 7) || (g.rxLength & 7)) {status = -3; break;}
        g.before[0] = rd(g.tx + 4); g.before[1] = rd(g.tx); g.before[2] = rd(g.rx + 4); g.before[3] = rd(g.rx);
        wr(g.tx + 4, 0);
        wr(g.rx, 0);
        __asm__ volatile("dsb sy" ::: "memory");
        txCommand(kVersion, 1, 0);
        if (!waitFor([] {return g.versioned;}, 1000)) {status = -4; break;}
        waitFor([] {return g.remoteOpen;}, 1000);
        uint32_t open[6] = {kOpen | (uint32_t(kLcid) << 16), sizeof kChannel, 0, 0, 0, 0};
        memcpy(&open[2], kChannel, sizeof kChannel);
        txWrite(open, 8 + align8(sizeof kChannel));
        if (!waitFor([] {return g.opened;}, 2000)) {status = -5; break;}
        waitFor([] {return g.remoteOpen;}, 500);
        g.ready = true;
    } while (false);
    g.status = status;
    rows();
    IOLockUnlock(g.lock);
    return status;
}

// One request with one key/value (set 0 active, 1 sleep); 0 when the RPM answered without an error. -6 not open, -7 FIFO full,
// -60 no answer within 500 ms, -5 the RPM's "err" (its text in the row).
int tb128fu_rpm_request(uint32_t set, uint32_t type, uint32_t id, uint32_t key, uint32_t value) {
    if (!g.ready || !g.lock) return -6;
    IOLockLock(g.lock);
    const uint32_t msgId = ++g.msgId;
    const uint32_t message[14] = {kTxData | (uint32_t(kLcid) << 16), 0, 40, 0,       // TX_DATA, intent 0, chunk 40 bytes, none left
                                  kReq, 32, msgId, set, type, id, 12, key, 4, value};
    g.answered = false;
    int status;
    if (!txWrite(message, sizeof message)) status = -7;
    else if (!waitFor([msgId] {return g.answered && g.answeredId == msgId;}, 500)) status = -60;
    else status = g.status;
    snprintf(tb128fu_rpm_rows[2], sizeof tb128fu_rpm_rows[2], "RPM req #%u set%u %.4s/%u %.4s=%x -> %d %.32s", msgId, set,
             reinterpret_cast<const char *>(&type), id, reinterpret_cast<const char *>(&key), value, status, status == -5 ? g.error : "");
    rows();
    IOLockUnlock(g.lock);
    return status;
}
