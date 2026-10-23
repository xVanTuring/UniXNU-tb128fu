// SPDX-License-Identifier: BSD-2-Clause-Patent
// Default-off staged bring-up of the TB128FU touch controller (Himax HX83102E on QUP SE0 SPI), after UniXNU-sheng's
// TouchProbe.cpp (NT36532E on SM8550). `tb128fu-touch=<n>`:
//   1  read-only snapshot of the GCC branches, the SE0 clock and the touch pins
//   2  vote the QUP clocks, wait for them, configure the pins, read the GENI engine identity
//   3  GENI SPI init (FIFO mode) and clock, an engine loopback test, then reset the controller and read its chip id
//   4  load the firmware into the controller (zero flash) and start it (needs the generated firmware object)
//   5  read touch reports on the attention line and publish the finger set to NativeHidInput (TouchState.cpp)
// Bits 8 and up of the value are a count of scripted strokes (autoTouchThread) for runs nobody watches: 10245 is stage 5 and 40.
// Every stage reports on the diagnostic page and in the UFS log ([touch probe]); the first failure stops the sequence. Board
// constants and their sources are in TouchBoard.h, the controller protocol in HimaxProtocol.c.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <string.h>
#include <libkern/c++/OSSharedPtr.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
extern "C" {
#include "GeniSpi.h"
#include "HimaxProtocol.h"
#include "TouchBoard.h"
// Generated from the tablet's Himax_firmware.bin by scripts/build-native-legacy-display.py.
extern const uint8_t native_touch_firmware[];
extern const uint32_t native_touch_firmware_size;
// The finger set (TouchState.cpp) and its consumer's notification.
extern volatile uint32_t native_touch_seq, native_touch_count;
extern volatile int native_touch_xs[12], native_touch_ys[12], native_touch_ids[12];
void native_touch_published(void);
}

extern "C" {
char native_touch_rows[4][120];
char native_touch_trace[8][120];
volatile uint32_t native_touch_loop_step;
volatile unsigned native_touch_stage_req;
volatile uint32_t native_touch_stage_done, native_touch_step, native_touch_frames, native_touch_valid, native_touch_contacts;
volatile int native_touch_error;
volatile int native_touch_last_x = -1, native_touch_last_y = -1;
// Finished strokes for the UFS log (BootLog.c [touch]): {start ms, duration ms, first x, first y, last x, last y, frames} in
// tenths of a panel pixel, and the extent every contact reached {min x, max x, min y, max y}.
volatile int32_t native_touch_strokes[16][7];
volatile uint32_t native_touch_stroke_count;
volatile int32_t native_touch_extent[4] = {0x7fffffff, -1, 0x7fffffff, -1};
}

namespace {
struct Region {OSSharedPtr<IOMemoryMap> keep; uintptr_t va = 0;};

bool mapRegion(uint64_t phys, uint64_t length, Region &region) {
    IODeviceMemory *memory = IODeviceMemory::withRange(phys, length);
    if (!memory) return false;
    region.keep = OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
    memory->release();
    region.va = region.keep ? static_cast<uintptr_t>(region.keep->getVirtualAddress()) : 0;
    return region.va != 0;
}
uint32_t rd32(uintptr_t base, uint32_t reg) {return *reinterpret_cast<volatile uint32_t *>(base + reg);}
void wr32(uintptr_t base, uint32_t reg, uint32_t value) {
    *reinterpret_cast<volatile uint32_t *>(base + reg) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}
uint32_t ioRd(void *ctx, uint32_t reg) {return rd32(reinterpret_cast<uintptr_t>(ctx), reg);}
void ioWr(void *ctx, uint32_t reg, uint32_t value) {wr32(reinterpret_cast<uintptr_t>(ctx), reg, value);}
void ioDelayUs(void *, unsigned us) {IODelay(us);}

// TLMM per-pin registers: ctl +0, io +4, intr_cfg +8, intr_status +0xc (pinctrl-khaje.c PINGROUP).
constexpr uint32_t kIntrCfg = 0x8, kIntrStatus = 0xc;
// intr_cfg: raw status latch (bit 4), falling edge (detection field 3:2 = 2, polarity bit 1), interrupt itself left disabled
// (bit 0) so nothing reaches the MPM or the GIC; the probe polls and clears the latched status (pinctrl-msm.c
// msm_gpio_irq_set_type for detection width 2).
constexpr uint32_t kIntrFallingLatch = (1u << 4) | (2u << 2) | (1u << 1);

struct Session {
    Region gccLow, gccQup, gccVote, tlmmSpi, tlmmIrq, tlmmRst, wrapper, se;
    uint32_t vals[24];
    uint64_t levelHz[TB_DFS_LEVELS];
    unsigned levels = 0, level = 0, div = 0;
    int dfs = 0;
    uint64_t actualHz = 0;
    struct geni_spi spi;
    int loopRc = 99;
    uint8_t loopRx[8];
    // First transfers as seen on the wire and the last four (ring).
    unsigned traceN, ringN;
    uint32_t traceLen[4], ringLen[4];
    uint8_t traceTx[4][8], traceRx[4][8], ringTx[4][8], ringRx[4][8];
    int lastRc = 0;
    uint32_t lastLen = 0, xIrq = 0, xGeni = 0, xTx = 0, xRx = 0;
    uint8_t *tx = nullptr, *rx = nullptr, *cfg = nullptr;
    struct hx_bus bus;
    struct hx_chip chip;
    struct hx_fw fw;
    int fwRc = 0;
    // Event loop evidence.
    uint32_t edges = 0, lowSeen = 0, reads = 0, bad = 0, idleReads = 0, nonzero = 0;
    uint8_t frame[HX_EVENT_LEN], lastNonzero[HX_EVENT_LEN];   // the latest read, and the latest read that was not all zero
};

int busXfer(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len) {
    Session *s = static_cast<Session *>(ctx);
    int rc = geni_spi_transfer(&s->spi, tx, rx, len, 0);
    unsigned i = s->ringN++ % 4;
    s->ringLen[i] = static_cast<uint32_t>(len);
    for (unsigned k = 0; k < 8; ++k) {
        s->ringTx[i][k] = (tx && k < len) ? tx[k] : 0;
        s->ringRx[i][k] = (rx && k < len && !rc) ? rx[k] : 0;
    }
    if (!rc && s->traceN < 4) {
        unsigned t = s->traceN++;
        s->traceLen[t] = static_cast<uint32_t>(len);
        for (unsigned k = 0; k < 8; ++k) {
            s->traceTx[t][k] = (tx && k < len) ? tx[k] : 0;
            s->traceRx[t][k] = (rx && k < len) ? rx[k] : 0;
        }
    }
    if (rc && !s->lastRc) {
        s->lastRc = rc; s->lastLen = static_cast<uint32_t>(len);
        s->xIrq = s->spi.snap_irq; s->xGeni = s->spi.snap_status; s->xTx = s->spi.snap_txfs; s->xRx = s->spi.snap_rxfs;
    }
    return rc;
}
void busReset(void *ctx, int high) {
    Session *s = static_cast<Session *>(ctx);
    wr32(s->tlmmRst.va, 4, high ? TB_GPIO_OUT : 0);
}
void busSleepMs(void *, unsigned ms) {IOSleep(ms);}

void fail(unsigned step, int code) {
    if (!native_touch_error) {native_touch_step = step; native_touch_error = code ? code : -1;}
}

// snprintf returns the length it would have written, so a running offset must be clamped before the next append.
struct Out {
    char *p; size_t cap, n;
    void add(const char *fmt, unsigned a = 0, unsigned b = 0) {
        if (n + 1 >= cap) return;
        int w = snprintf(p + n, cap - n, fmt, a, b);
        if (w < 0) return;
        n = (n + static_cast<size_t>(w) >= cap) ? cap - 1 : n + static_cast<size_t>(w);
    }
    void hex(uint8_t v) {add("%02x", v);}
};

void rows(const Session &s) {
    const uint32_t *v = s.vals;
    const struct hx_chip &c = s.chip;
    snprintf(native_touch_rows[0], sizeof native_touch_rows[0],
        "TCH s%u/%u st%u e%d vote%08x h%08x %08x %08x %08x %08x gp%08x rcg%08x/%08x dfs%x",
        native_touch_stage_done, native_touch_stage_req, native_touch_step, native_touch_error,
        v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9]);
    snprintf(native_touch_rows[1], sizeof native_touch_rows[1],
        "TCH pin %03x %03x %03x %03x int%03x/%x/%02x rst%03x/%x fw%08x if%x hw%08x qup%08x L%u/%u %uHz",
        v[10], v[11], v[12], v[13], v[14], v[15], v[16], v[17], v[18], v[19], v[20], v[21], v[22], s.level, s.div,
        (unsigned)s.actualHz);
    snprintf(native_touch_rows[2], sizeof native_touch_rows[2],
        "TCH id%02x%02x%02x%02x cs%02x safe%u stage%u rc%d fw%d crc%08x/%08x/%08x hs%08x rl%04x dn%04x ver%08x pi%08x",
        c.id[3], c.id[2], c.id[1], c.id[0], c.state & 0xff, c.safe_tries, c.stage, c.rc, s.fwRc, c.crc_main, c.crc_cfg, c.crc_ovl,
        c.handshake, c.reload & 0xffff, c.done & 0xffff, c.fw_ver, c.panel_info);
    snprintf(native_touch_rows[3], sizeof native_touch_rows[3],
        "TCH ev lp%u edges%u low%u reads%u idle%u bad%u nz%u frames%u valid%u c%u %d,%d loop%d xf rc%d n%u ev%08x gs%08x",
        native_touch_loop_step, s.edges, s.lowSeen, s.reads, s.idleReads, s.bad, s.nonzero, native_touch_frames, native_touch_valid,
        native_touch_contacts, native_touch_last_x, native_touch_last_y, s.loopRc, s.lastRc, s.lastLen, s.xIrq, s.xGeni);
    for (unsigned r = 0; r < 2; ++r) {
        Out o = {native_touch_trace[r], sizeof native_touch_trace[r], 0};
        o.p[0] = 0;
        o.add(r ? "TCH last" : "TCH first");
        for (unsigned j = 0; j < 4; ++j) {
            unsigned len, idx = 0;
            const uint8_t *t, *x;
            if (!r) {
                if (j >= s.traceN) break;
                len = s.traceLen[j]; t = s.traceTx[j]; x = s.traceRx[j];
            } else {
                if (j >= s.ringN || j >= 4) break;
                unsigned seq = s.ringN >= 4 ? s.ringN - 4 + j : j;
                idx = seq % 4;
                len = s.ringLen[idx]; t = s.ringTx[idx]; x = s.ringRx[idx];
            }
            o.add(" n%u tx", len);
            for (unsigned k = 0; k < 8 && k < len; ++k) o.hex(t[k]);
            o.add(" rx");
            for (unsigned k = 0; k < 8 && k < len; ++k) o.hex(x[k]);
        }
    }
    Out f = {native_touch_trace[2], sizeof native_touch_trace[2], 0};
    f.p[0] = 0;
    f.add("TCH nonzero");
    for (unsigned k = 0; k < 54; ++k) f.hex(s.lastNonzero[k]);
    Out l = {native_touch_trace[3], sizeof native_touch_trace[3], 0};
    l.p[0] = 0;
    l.add("TCH loopback rc%d rx", static_cast<unsigned>(s.loopRc));
    for (unsigned k = 0; k < 8; ++k) l.hex(s.loopRx[k]);
    l.add(" fifo depth%u width%u", s.spi.info.tx_fifo_depth, s.spi.info.fifo_width_bits);
    l.add(" os%u dfs%u", s.spi.oversampling, static_cast<unsigned>(s.dfs));
    l.add(" levels%u", s.levels);
}

void mark(const Session &s, unsigned step) {native_touch_step = step; rows(s);}

bool snapshot(Session &s) {
    mark(s, 1);
    if (!mapRegion(TB_GCC_BASE + 0x0000, 0x1000, s.gccLow) || !mapRegion(TB_GCC_BASE + 0x1f000, 0x1000, s.gccQup) ||
        !mapRegion(TB_GCC_BASE + 0x79000, 0x1000, s.gccVote) ||
        !mapRegion(TB_TLMM_BASE + tb_tlmm_ctl(TB_GPIO_SPI_FIRST), 4 * TB_GPIO_STRIDE, s.tlmmSpi) ||
        !mapRegion(TB_TLMM_BASE + tb_tlmm_ctl(TB_GPIO_IRQ), 0x1000, s.tlmmIrq) ||
        !mapRegion(TB_TLMM_BASE + tb_tlmm_ctl(TB_GPIO_RESET), 0x1000, s.tlmmRst)) {
        fail(1, -12);
        return false;
    }
    uint32_t *v = s.vals;
    v[0] = rd32(s.gccVote.va, TB_GCC_VOTE_ENABLE & 0xfff);
    v[1] = rd32(s.gccQup.va, TB_HALT_M_AHB & 0xfff);
    v[2] = rd32(s.gccQup.va, TB_HALT_S_AHB & 0xfff);
    v[3] = rd32(s.gccQup.va, TB_HALT_CORE & 0xfff);
    v[4] = rd32(s.gccQup.va, TB_HALT_CORE_2X & 0xfff);
    v[5] = rd32(s.gccQup.va, TB_HALT_S0 & 0xfff);
    v[6] = rd32(s.gccLow.va, TB_GCC_GPLL0_MODE);
    v[7] = rd32(s.gccQup.va, (TB_S0_RCG + TB_RCG_CMD) & 0xfff);
    v[8] = rd32(s.gccQup.va, (TB_S0_RCG + TB_RCG_CFG) & 0xfff);
    v[9] = rd32(s.gccQup.va, (TB_S0_RCG + TB_RCG_DFSR_CMD) & 0xfff);
    for (unsigned i = 0; i < 4; ++i) v[10 + i] = rd32(s.tlmmSpi.va, i * TB_GPIO_STRIDE);
    v[14] = rd32(s.tlmmIrq.va, 0); v[15] = rd32(s.tlmmIrq.va, 4); v[16] = rd32(s.tlmmIrq.va, kIntrCfg);
    v[17] = rd32(s.tlmmRst.va, 0); v[18] = rd32(s.tlmmRst.va, 4);
    // The serial clock source: the DFS levels if the RCG runs under DFS, else the configuration it has now.
    s.dfs = (v[9] & 1) != 0;
    if (s.dfs) {
        s.levels = TB_DFS_LEVELS;
        for (unsigned l = 0; l < TB_DFS_LEVELS; ++l)
            s.levelHz[l] = tb_rcg_hz(rd32(s.gccQup.va, (TB_S0_RCG + TB_RCG_PERF_CFG(l)) & 0xfff),
                                     rd32(s.gccQup.va, (TB_S0_RCG + TB_RCG_PERF_M(l)) & 0xfff),
                                     rd32(s.gccQup.va, (TB_S0_RCG + TB_RCG_PERF_N(l)) & 0xfff));
    } else {
        s.levels = 1;   // the plain RCG's rate is read in openBus, once the branch vote has its root running
    }
    return true;
}

bool enable(Session &s) {
    mark(s, 2);
    // GPLL0 feeds the SE clock; vote for it (a shared vote, not an exclusive enable) and wait for lock.
    wr32(s.gccVote.va, TB_GCC_GPLL0_ENABLE & 0xfff, rd32(s.gccVote.va, TB_GCC_GPLL0_ENABLE & 0xfff) | 1u);
    for (unsigned i = 0; i < 10000 && !(rd32(s.gccLow.va, TB_GCC_GPLL0_MODE) & (1u << 31)); ++i) IODelay(10);
    s.vals[6] = rd32(s.gccLow.va, TB_GCC_GPLL0_MODE);
    wr32(s.gccVote.va, TB_GCC_VOTE_ENABLE & 0xfff, rd32(s.gccVote.va, TB_GCC_VOTE_ENABLE & 0xfff) | TB_VOTE_ALL);
    // Branches with hardware clock gating (bit 1) show CLK_OFF while idle and wake on access (clk-branch.c).
    auto running = [](uint32_t cbcr) {return !(cbcr & TB_CBCR_CLK_OFF) || (cbcr & 2u);};
    bool ok = false;
    for (unsigned i = 0; i < 2000 && !ok; ++i) {
        ok = running(rd32(s.gccQup.va, TB_HALT_M_AHB & 0xfff)) && running(rd32(s.gccQup.va, TB_HALT_S_AHB & 0xfff)) &&
             !(rd32(s.gccQup.va, TB_HALT_CORE & 0xfff) & TB_CBCR_CLK_OFF) &&
             !(rd32(s.gccQup.va, TB_HALT_CORE_2X & 0xfff) & TB_CBCR_CLK_OFF) &&
             !(rd32(s.gccQup.va, TB_HALT_S0 & 0xfff) & TB_CBCR_CLK_OFF);
        if (!ok) IODelay(10);
    }
    s.vals[0] = rd32(s.gccVote.va, TB_GCC_VOTE_ENABLE & 0xfff);
    s.vals[1] = rd32(s.gccQup.va, TB_HALT_M_AHB & 0xfff);
    s.vals[2] = rd32(s.gccQup.va, TB_HALT_S_AHB & 0xfff);
    s.vals[3] = rd32(s.gccQup.va, TB_HALT_CORE & 0xfff);
    s.vals[4] = rd32(s.gccQup.va, TB_HALT_CORE_2X & 0xfff);
    s.vals[5] = rd32(s.gccQup.va, TB_HALT_S0 & 0xfff);
    if (!ok) {fail(2, -110); return false;}   // never touch the engine without running clocks
    // SPI pins: qup0 function, 6 mA, no pull (DTS qupv3_se0_spi_active); CS0 also output-high (qupv3_se0_spi_cs_active).
    const uint32_t spi = tb_pin_cfg(TB_FUNC_QUP0, TB_DRIVE_6MA, TB_PULL_NONE);
    for (unsigned i = 0; i < 4; ++i) {
        if (TB_GPIO_SPI_FIRST + i == TB_GPIO_SPI_CS) {wr32(s.tlmmSpi.va, i * TB_GPIO_STRIDE + 4, TB_GPIO_OUT); wr32(s.tlmmSpi.va, i * TB_GPIO_STRIDE, spi | TB_GPIO_OE);}
        else wr32(s.tlmmSpi.va, i * TB_GPIO_STRIDE, spi);
    }
    // Attention: GPIO input with pull-up (DTS ts_int_active) and the falling-edge latch; reset: GPIO output, released (high).
    wr32(s.tlmmIrq.va, 0, tb_pin_cfg(TB_FUNC_GPIO, TB_DRIVE_8MA, TB_PULL_UP));
    wr32(s.tlmmIrq.va, kIntrCfg, (rd32(s.tlmmIrq.va, kIntrCfg) & ~0x1fu) | kIntrFallingLatch);
    wr32(s.tlmmIrq.va, kIntrStatus, 0);
    wr32(s.tlmmRst.va, 4, TB_GPIO_OUT);
    wr32(s.tlmmRst.va, 0, tb_pin_cfg(TB_FUNC_GPIO, TB_DRIVE_8MA, TB_PULL_UP) | TB_GPIO_OE);
    for (unsigned i = 0; i < 4; ++i) s.vals[10 + i] = rd32(s.tlmmSpi.va, i * TB_GPIO_STRIDE);
    s.vals[14] = rd32(s.tlmmIrq.va, 0); s.vals[15] = rd32(s.tlmmIrq.va, 4); s.vals[16] = rd32(s.tlmmIrq.va, kIntrCfg);
    s.vals[17] = rd32(s.tlmmRst.va, 0); s.vals[18] = rd32(s.tlmmRst.va, 4);
    mark(s, 3);   // the next accesses touch the engine: if the page freezes here, the engine access hung
    if (!mapRegion(TB_QUP_WRAPPER_BASE, 0x2000, s.wrapper) || !mapRegion(TB_SE_BASE, TB_SE_SIZE, s.se)) {fail(3, -12); return false;}
    s.spi.se = (struct geni_io){reinterpret_cast<void *>(s.se.va), ioRd, ioWr, ioDelayUs};
    s.spi.wrapper = (struct geni_io){reinterpret_cast<void *>(s.wrapper.va), ioRd, ioWr, ioDelayUs};
    mark(s, 31);   // first engine register read
    int r = geni_spi_read_info(&s.spi);
    s.vals[19] = s.spi.info.fw_revision;
    s.vals[20] = s.spi.info.if_disable;
    s.vals[21] = s.spi.info.hw_param0;
    s.vals[22] = s.spi.info.qup_hw_version;
    if (r) {fail(3, r); return false;}
    return true;
}

bool openBus(Session &s, unsigned hz) {
    mark(s, 4);
    // FIFO (PIO) mode only: an engine whose FIFO interface the firmware disabled needs the GPI or SE DMA path.
    if (s.spi.info.fifo_disabled) {fail(4, GENI_ERR_NOTSUP); return false;}
    s.spi.poll_limit = 400000;
    s.spi.poll_only = 1;             // completion is polled; no engine interrupt reaches the GIC
    int r = geni_spi_init(&s.spi);
    if (r) {fail(4, r); return false;}
    mark(s, 5);
    // Without DFS the SE clock is whatever the RCG is configured for now; if its source is not one of the known ones, run it
    // from the 19.2 MHz TCXO (CFG: source 0, hid 1 = divide by 1).
    if (!s.dfs) {
        s.vals[7] = rd32(s.gccQup.va, (TB_S0_RCG + TB_RCG_CMD) & 0xfff);
        s.vals[8] = rd32(s.gccQup.va, (TB_S0_RCG + TB_RCG_CFG) & 0xfff);
        s.levelHz[0] = tb_rcg_hz(s.vals[8], rd32(s.gccQup.va, (TB_S0_RCG + TB_RCG_M) & 0xfff),
                                 rd32(s.gccQup.va, (TB_S0_RCG + TB_RCG_N) & 0xfff));
        if (!s.levelHz[0]) {
            wr32(s.gccQup.va, (TB_S0_RCG + TB_RCG_CFG) & 0xfff, 1);
            wr32(s.gccQup.va, (TB_S0_RCG + TB_RCG_CMD) & 0xfff, s.vals[7] | 1u);
            for (unsigned i = 0; i < 1000 && (rd32(s.gccQup.va, (TB_S0_RCG + TB_RCG_CMD) & 0xfff) & 1u); ++i) IODelay(10);
            s.vals[7] = rd32(s.gccQup.va, (TB_S0_RCG + TB_RCG_CMD) & 0xfff);
            s.vals[8] = rd32(s.gccQup.va, (TB_S0_RCG + TB_RCG_CFG) & 0xfff);
            s.levelHz[0] = 19200000ULL;
        }
    }
    struct tb_clock_choice choice = {};
    if (tb_choose_clock(s.levelHz, s.levels, s.spi.oversampling, hz, &choice)) {fail(5, -22); return false;}
    s.level = choice.level; s.div = choice.div; s.actualHz = choice.actual_hz;
    r = geni_spi_set_clock(&s.spi, hz, static_cast<unsigned>(choice.sclk_hz), s.dfs ? choice.level : 0);
    if (r) {fail(5, r); return false;}
    return true;
}

// Engine loopback: MOSI is fed back to MISO inside the controller, which checks the FIFO, packing and clock path without the
// touch chip.
void loopback(Session &s) {
    static const uint8_t pattern[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    geni_spi_setup(&s.spi, GENI_SPI_LOOP, 0);
    s.loopRc = geni_spi_transfer(&s.spi, pattern, s.loopRx, sizeof pattern, 0);
    if (!s.loopRc && memcmp(pattern, s.loopRx, sizeof pattern)) s.loopRc = 1;
    geni_spi_setup(&s.spi, 0, 0);
}

struct Stroke {int id, x0, y0, x1, y1, frames; uint32_t t0;};
Stroke strokes[HX_MAX_PT];
unsigned strokeCount;
uint32_t touchMs() {
    uint64_t f = 0; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return (uint32_t)(mach_absolute_time() / ((f ? f : 19200000) / 1000));
}
void trackStrokes(unsigned count, const hx_point *p) {
    const uint32_t now = touchMs();
    for (unsigned i = 0; i < count; ++i) {
        int x = p[i].x * 10, y = p[i].y * 10;
        if (x < native_touch_extent[0]) native_touch_extent[0] = x;
        if (x > native_touch_extent[1]) native_touch_extent[1] = x;
        if (y < native_touch_extent[2]) native_touch_extent[2] = y;
        if (y > native_touch_extent[3]) native_touch_extent[3] = y;
        unsigned k = 0;
        while (k < strokeCount && strokes[k].id != p[i].slot) ++k;
        if (k == strokeCount) {
            if (strokeCount == HX_MAX_PT) continue;
            strokes[strokeCount++] = {p[i].slot, x, y, x, y, 0, now};
        }
        strokes[k].x1 = x; strokes[k].y1 = y; ++strokes[k].frames;
    }
    for (unsigned k = 0; k < strokeCount;) {
        bool live = false;
        for (unsigned i = 0; i < count && !live; ++i) live = p[i].slot == strokes[k].id;
        if (live) {++k; continue;}
        volatile int32_t *r = native_touch_strokes[native_touch_stroke_count & 15];
        r[0] = (int32_t)strokes[k].t0; r[1] = (int32_t)(now - strokes[k].t0);
        r[2] = strokes[k].x0; r[3] = strokes[k].y0; r[4] = strokes[k].x1; r[5] = strokes[k].y1; r[6] = strokes[k].frames;
        native_touch_stroke_count = native_touch_stroke_count + 1;
        strokes[k] = strokes[--strokeCount];
    }
}

// The finger set in the digitizer's units (x 0..30480 across the panel's 1200 pixels, y 0..20320 across its 2000), under the
// sequence lock TouchState.cpp describes, then the HID input's notification.
void publish(unsigned count, const hx_point *p) {
    trackStrokes(count, p);
    native_touch_seq = native_touch_seq + 1;
    __asm__ volatile("dmb ish" ::: "memory");
    native_touch_count = count;
    for (unsigned i = 0; i < count && i < 12; ++i) {
        native_touch_xs[i] = p[i].x * 30480 / HX_X_MAX;
        native_touch_ys[i] = p[i].y * 20320 / HX_Y_MAX;
        native_touch_ids[i] = p[i].slot;
    }
    __asm__ volatile("dmb ish" ::: "memory");
    native_touch_seq = native_touch_seq + 1;
    native_touch_published();
}
}  // namespace

static void touchThread(void *, wait_result_t) {
    Session *s = static_cast<Session *>(IOMallocType(Session));
    if (!s) {fail(0, -12); return;}
    bzero(s, sizeof *s);
    s->loopRc = 99;
    unsigned stage = native_touch_stage_req, hz = TB_SPI_DEFAULT_HZ;
    PE_parse_boot_argn("tb128fu-touch-hz", &hz, sizeof hz);
    if (hz < 100000 || hz > TB_SPI_MAX_HZ) hz = TB_SPI_DEFAULT_HZ;
    do {
        if (!snapshot(*s)) break;
        native_touch_stage_done = 1;
        rows(*s);
        if (stage < 2) break;
        if (!enable(*s)) break;
        native_touch_stage_done = 2;
        rows(*s);
        if (stage < 3) break;
        if (!openBus(*s, hz)) break;
        loopback(*s);
        rows(*s);
        geni_spi_setup(&s->spi, (TB_SPI_MODE & 1 ? GENI_SPI_CPHA : 0) | (TB_SPI_MODE & 2 ? GENI_SPI_CPOL : 0), 0);
        s->tx = static_cast<uint8_t *>(IOMalloc(HX_XFER_MAX));
        s->rx = static_cast<uint8_t *>(IOMalloc(HX_XFER_MAX));
        s->cfg = static_cast<uint8_t *>(IOMalloc(HX_CFG_MAX));
        if (!s->tx || !s->rx || !s->cfg) {fail(6, -12); break;}
        s->bus = (struct hx_bus){s, busXfer, busReset, ioDelayUs, busSleepMs};
        s->chip.bus = &s->bus; s->chip.tx = s->tx; s->chip.rx = s->rx;
        mark(*s, 7);
        int r = hx_identify(&s->chip);
        if (r) {fail(7, r); break;}
        native_touch_stage_done = 3;
        rows(*s);
        if (stage < 4) break;
        if (!native_touch_firmware_size) {fail(8, -2); break;}
        mark(*s, 8);
        s->fwRc = hx_fw_parse(&s->fw, native_touch_firmware, native_touch_firmware_size, s->cfg, HX_CFG_MAX);
        if (s->fwRc) {fail(8, s->fwRc); break;}
        mark(*s, 9);
        r = hx_download(&s->chip, &s->fw, s->cfg);
        if (r) {fail(9, r); break;}
        native_touch_stage_done = 4;
        rows(*s);
        if (stage < 5) break;
        mark(*s, 10);
        // Attention is a falling edge per frame (the firmware's INT_IS_EDGE); the TLMM latch keeps it for this poll. A read
        // every 100 ms without an edge shows whether the line is quiet or the latch does not work.
        unsigned tick = 0, rowTick = 0, lastCount = 0;
        for (;;) {
            native_touch_loop_step = 1;
            bool edge = (rd32(s->tlmmIrq.va, kIntrStatus) & 1) != 0;
            bool low = (rd32(s->tlmmIrq.va, 4) & TB_GPIO_IN) == 0;
            if (edge) {wr32(s->tlmmIrq.va, kIntrStatus, 0); ++s->edges;}
            if (low) ++s->lowSeen;
            bool idle = !edge && !low && (++tick % 50) == 0;
            if (!edge && !low && !idle) {IOSleep(2); continue;}
            native_touch_loop_step = 2;
            if (hx_read_event(&s->chip, s->frame)) {fail(11, HX_ERR_BUS); IOSleep(8); continue;}
            native_touch_loop_step = 3;
            if (idle) ++s->idleReads; else ++s->reads;
            {
                bool any = false;
                for (unsigned k = 0; k < HX_EVENT_LEN && !any; ++k) any = s->frame[k] != 0;
                if (any) {++s->nonzero; memcpy(s->lastNonzero, s->frame, HX_EVENT_LEN);}
            }
            struct hx_point pts[HX_MAX_PT];
            int n = hx_parse_event(s->frame, pts, HX_MAX_PT);
            if (n < 0) {++s->bad; if ((++rowTick & 15) == 0) rows(*s); continue;}
            native_touch_valid = native_touch_valid + 1;
            // Publish frames from the attention line, and an idle read only when it changes the finger count.
            if (!idle || static_cast<unsigned>(n) != lastCount) {
                native_touch_frames = native_touch_frames + 1;
                native_touch_contacts = static_cast<uint32_t>(n);
                if (n > 0) {native_touch_last_x = pts[0].x; native_touch_last_y = pts[0].y;}
                publish(static_cast<unsigned>(n), pts);
                lastCount = static_cast<unsigned>(n);
            }
            if ((++rowTick & 15) == 0) rows(*s);
        }
    } while (0);
    rows(*s);
}

// Scripted strokes for runs nobody watches (tb128fu-touch bits 8 and up: their count). Once the GPU hand-off has completed frames
// (the lock screen is up) and 30 s more have passed, a swipe up from the bottom edge unlocks to the home screen, and 5 s later strokes
// alternately right to left and back across the middle of the panel turn its pages, each 300 ms long with 1.5 s between them: one
// finger in a slot the controller does not use, published as its frames are (publish). Without the swipe the strokes ran on whichever
// screen was up, the lock screen unless someone unlocked, and rounds did not compare (tb128fu-077, 078).
extern "C" volatile uint64_t native_gop_gpu_completions __attribute__((weak_import));
extern "C" volatile uint32_t native_touch_auto_done;
volatile uint32_t native_touch_auto_done;
static void autoTouchThread(void *arg, wait_result_t) {
    const unsigned strokes = static_cast<unsigned>(reinterpret_cast<uintptr_t>(arg));
    while (!&native_gop_gpu_completions || !native_gop_gpu_completions) IOSleep(1000);
    IOSleep(30000);
    // From the panel's last rows, where NativeHidInput marks the contact as an edge one (SpringBoard's home gesture needs it).
    for (int step = 0; step <= 25; ++step) {
        hx_point p = {9, 600, 1995 - 1095 * step / 25, 8};
        publish(1, &p);
        IOSleep(10);
    }
    publish(0, nullptr);
    IOSleep(5000);
    for (unsigned i = 0; i < strokes; ++i) {
        const int from = (i & 1) ? 240 : 960, to = (i & 1) ? 960 : 240;
        for (int step = 0; step <= 30; ++step) {
            hx_point p = {9, from + (to - from) * step / 30, 1000, 8};
            publish(1, &p);
            IOSleep(10);
        }
        publish(0, nullptr);
        native_touch_auto_done = i + 1;
        IOSleep(1500);
    }
}

extern "C" void native_touch_probe(void) {
    unsigned value = 0;
    if (!PE_parse_boot_argn("tb128fu-touch", &value, sizeof value) || (value & 0xff) < 1 || native_touch_stage_req) return;
    unsigned stage = value & 0xff;
    if (stage > 5) stage = 5;
    native_touch_stage_req = stage;
    thread_t thread = nullptr;
    if (kernel_thread_start(touchThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    else native_touch_error = -12;
    if (stage == 5 && (value >> 8)) {
        thread_t automatic = nullptr;
        if (kernel_thread_start(autoTouchThread, reinterpret_cast<void *>(static_cast<uintptr_t>(value >> 8)), &automatic) == KERN_SUCCESS)
            thread_deallocate(automatic);
    }
}
