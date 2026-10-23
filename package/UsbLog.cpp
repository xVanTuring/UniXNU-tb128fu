// SPDX-License-Identifier: MIT
// USB log channel for the TB128FU (SM6225 "khaje"): the primary USB controller (Synopsys DWC3 at 0x4e00000 with Qualcomm's QSCRATCH
// glue at +0xf8800, high speed only) and its Synopsys femto HS PHY (0x1613000), brought up by the kernel so a host can read the board
// log while the system runs (the UFS log is only read after a restart into fastboot). Built in stages, each reported in the boot log's
// [usb] rows:
//   1  power: the USB30_PRIM GDSC on, the controller and PHY block resets pulsed, the master RCG at 133.33 MHz (GPLL0 / 4.5) and the
//      mock UTMI RCG at 19.2 MHz (XO), the seven branch clocks on; then, only if every branch reports running and the GDSC reports
//      power, a read of the controller's identity, QSCRATCH and PHY registers. Nothing is written to the controller or the PHY.
//      (Round 063: GDSC on, every branch running, DWC_usb3 3.30a; the SMMU faults unmatched streams and none matched 0x120.)
//   2  the femto PHY's power-up, the glue (UTMI clock, VBUS and session valid forced), the controller's core soft reset
//   3  the controller's stream mapped through the SMMU, device mode, enumeration as a CDC ACM serial port
//   4  the board log (each snapshot and the kernel message buffer's new text, from BootLog.c) on its bulk IN endpoint
// The register offsets are Linux's drivers/clk/qcom/gcc-sm6115.c (the SM6225 GCC has the same USB blocks), the clock and supply
// names the stock device tree's usb0 and usb2_phy0 nodes, the glue and PHY sequences U-Boot's dwc3-generic.c and
// phy-qcom-snps-femto-v2.c.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <libkern/c++/OSSharedPtr.h>
#include <string.h>

extern "C" {
// The boot log's [usb] rows (BootLog.c): power and clocks, the branch clocks, the controller, the PHY, the SMMU's routing of the
// controller's stream.
char tb128fu_usb_rows[5][120];
volatile uint32_t tb128fu_usb_stage_done;
void tb128fu_usb_probe(void);
}

namespace {
constexpr unsigned kStage = 4;   // the stage to reach; the board's boot arguments have no room left for a switch
constexpr uint64_t kGcc = 0x1400000, kGccSize = 0xa0000, kDwc = 0x4e00000, kDwcSize = 0x100000, kPhy = 0x1613000, kPhySize = 0x1000;
// GCC (gcc-sm6115.c): block resets, the GDSC, the two RCGs and the branches the usb0 and usb2_phy0 nodes name.
constexpr uint32_t kUsbBcr = 0x1a000, kPhyBcr = 0x1c000, kGdsc = 0x1a004, kMasterRcg = 0x1a01c, kUtmiRcg = 0x1a034;
constexpr uint32_t kBranches[] = {
    0x1a080,   // gcc_sys_noc_usb3_prim_axi_clk (bus_aggr_clk)
    0x1a084,   // gcc_cfg_noc_usb3_prim_axi_clk (iface_clk)
    0x1a010,   // gcc_usb30_prim_master_clk (core_clk)
    0x1a014,   // gcc_usb30_prim_sleep_clk (sleep_clk)
    0x1a018,   // gcc_usb30_prim_mock_utmi_clk (utmi_clk)
    0x9f000,   // gcc_usb3_prim_clkref_clk (xo)
    0x1d008,   // gcc_ahb2phy_usb_clk (the PHY's cfg_ahb_clk)
};
constexpr unsigned kBranchCount = sizeof kBranches / sizeof kBranches[0];
// DWC3 global registers and QSCRATCH.
constexpr uint32_t kGctl = 0xc110, kGsnpsid = 0xc120, kGhwparams0 = 0xc140, kDctl = 0xc704, kDsts = 0xc70c;
constexpr uint32_t kQscratch = 0xf8800, kQsGeneralCfg = 0x08, kQsHsPhyCtrl = 0x10;
// The femto PHY. (The EUD enable register the PHY node also names, 0x1612000, belongs to another block and is not read.)
constexpr uint32_t kUtmiCtrl0 = 0x3c, kUtmiCtrl5 = 0x50, kCommon0 = 0x54, kCommon1 = 0x58, kCommon2 = 0x5c, kPhyCtrl1 = 0x60,
                   kPhyCtrl2 = 0x64, kPhyCfg0 = 0x94, kRefclkCtrl = 0xa0;
// The stock device tree's qcom,param-override-seq for usb2_phy0: value, offset pairs (the HS PHY's tuning overrides).
constexpr uint32_t kPhyOverrides[][2] = {{0xa6, 0x6c}, {0xcf, 0x70}, {0x16, 0x74}};
constexpr uint32_t kQsSsPhyCtrl = 0x30;
constexpr uint32_t kDctlCoreSoftReset = 1u << 30;

// The apps SMMU (as NativeUfs's GccUfs.h): the stream the usb0 node names (iommus = <&apps_smmu 0x120 0>), its stream match
// registers (SMR, 0x800 + 4n: VALID bit 31, MASK [30:16], ID [15:0]) and stream-to-context registers (S2CR, 0xc00 + 4n).
constexpr uint64_t kSmmu = 0xc600000, kSmmuSize = 0x80000;
constexpr uint32_t kUsbStream = 0x120;

struct Region {OSSharedPtr<IOMemoryMap> map; uintptr_t va = 0;};
Region gcc, dwc, phy, smmu;

bool mapRegion(uint64_t physical, uint64_t length, Region &region) {
    IODeviceMemory *memory = IODeviceMemory::withRange(physical, length);
    if (!memory) return false;
    region.map = OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
    memory->release();
    region.va = region.map ? static_cast<uintptr_t>(region.map->getVirtualAddress()) : 0;
    return region.va != 0;
}
uint32_t rd(const Region &r, uint32_t offset) {return *reinterpret_cast<volatile uint32_t *>(r.va + offset);}
void wr(const Region &r, uint32_t offset, uint32_t value) {
    *reinterpret_cast<volatile uint32_t *>(r.va + offset) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}
// Polls (up to timeoutUs) until (register & mask) == want.
bool waitFor(const Region &r, uint32_t offset, uint32_t mask, uint32_t want, unsigned timeoutUs) {
    for (unsigned t = 0; t < timeoutUs; t += 10) {
        if ((rd(r, offset) & mask) == want) return true;
        IODelay(10);
    }
    return (rd(r, offset) & mask) == want;
}
// An RCG's source and half-integer divider (CFG_RCGR: SRC_DIV [4:0] = 2 * divider - 1, SRC_SEL [10:8], MODE [13:12] = 0 without
// M/N), then the update (CMD_RCGR bit 0), which the RCG clears once it switched.
bool setRcg(uint32_t cmd, uint32_t source, uint32_t halfDivider) {
    const uint32_t cfg = rd(gcc, cmd + 4);
    wr(gcc, cmd + 4, (cfg & ~0x371fu) | (source << 8) | halfDivider);
    wr(gcc, cmd, rd(gcc, cmd) | 1u);
    return waitFor(gcc, cmd, 1u, 0u, 2000);
}

void setBits(const Region &r, uint32_t offset, uint32_t mask, uint32_t value) {wr(r, offset, (rd(r, offset) & ~mask) | (value & mask));}

// Stage 2. The femto PHY's power-up (phy-qcom-snps-femto-v2.c qcom_snps_hsphy_usb_init, with the stock tree's tuning overrides
// applied while the PHY is held in power-on reset), then the glue (dwc3-generic.c qcom glue: UTMI as the pipe clock of a USB2-only
// port, VBUS and session valid forced on, as no PMIC driver reports them), then the controller's core soft reset (DCTL.CSFTRST).
bool bringUpPhy() {
    setBits(phy, kPhyCfg0, 1u << 1, 1u << 1);       // UTMI_PHY_CMN_CTRL_OVERRIDE_EN
    setBits(phy, kUtmiCtrl5, 1u << 1, 1u << 1);     // POR
    setBits(phy, kCommon0, 7u << 4, 0);             // FSEL
    setBits(phy, kCommon1, 1u << 5, 1u << 5);       // PLLBTUNE
    setBits(phy, kRefclkCtrl, 3u, 2u);              // REFCLK_SEL default
    setBits(phy, kCommon1, 1u << 4, 1u << 4);       // VBUSVLDEXTSEL0
    setBits(phy, kPhyCtrl1, 1u, 1u);                // VBUSVLDEXT0
    for (const auto &o : kPhyOverrides) wr(phy, o[1], o[0]);
    setBits(phy, kCommon2, 1u, 1u);                 // VREGBYPASS
    setBits(phy, kPhyCtrl2, 3u << 2, 3u << 2);      // USB2_SUSPEND_N_SEL | USB2_SUSPEND_N
    setBits(phy, kUtmiCtrl0, 1u, 1u);               // SLEEPM
    setBits(phy, kCommon0, 1u << 2, 0);             // SIDDQ off
    setBits(phy, kUtmiCtrl5, 1u << 1, 0);           // POR released
    setBits(phy, kPhyCtrl2, 1u << 3, 0);            // USB2_SUSPEND_N_SEL
    setBits(phy, kPhyCfg0, 1u << 1, 0);
    IODelay(100);
    // Glue: PIPE_UTMI_CLK_DIS, then PIPE_UTMI_CLK_SEL | PIPE3_PHYSTATUS_SW, then the clock again; UTMI_OTG_VBUS_VALID |
    // SW_SESSVLD_SEL; LANE0_PWR_PRESENT.
    setBits(dwc, kQscratch + kQsGeneralCfg, 1u << 8, 1u << 8);
    IODelay(100);
    setBits(dwc, kQscratch + kQsGeneralCfg, (1u << 0) | (1u << 3), (1u << 0) | (1u << 3));
    IODelay(100);
    setBits(dwc, kQscratch + kQsGeneralCfg, 1u << 8, 0);
    setBits(dwc, kQscratch + kQsHsPhyCtrl, (1u << 20) | (1u << 28), (1u << 20) | (1u << 28));
    setBits(dwc, kQscratch + kQsSsPhyCtrl, 1u << 24, 1u << 24);
    // Core soft reset: the bit clears itself once the reset finished (dwc3_core_soft_reset waits up to 1 s).
    wr(dwc, kDctl, kDctlCoreSoftReset);
    return waitFor(dwc, kDctl, kDctlCoreSoftReset, 0, 1000000);
}

// Stages 3 and 4: the controller as a USB 2.0 high-speed device, a CDC ACM serial port (macOS binds its own driver and shows
// /dev/cu.usbmodem*), the board log streamed on its bulk IN endpoint. Polled every millisecond from this thread, no interrupt; the
// control endpoint follows Linux's drivers/usb/dwc3/ep0.c (setup, data and status phases, the status phase started on the
// controller's XferNotReady for it). One 64 KiB physically contiguous buffer below 4 GiB holds the event ring, the TRBs and the
// transmit buffer; the CPU's copy is cleaned before the controller reads and invalidated before the CPU reads what it wrote.
constexpr uint32_t kGusb2PhyCfg = 0xc200, kGevntAdrLo = 0xc400, kGevntAdrHi = 0xc404, kGevntSiz = 0xc408, kGevntCount = 0xc40c;
constexpr uint32_t kDcfg = 0xc700, kDevten = 0xc708, kDalepena = 0xc720;
constexpr uint32_t depPar2(unsigned ep) {return 0xc800 + ep * 0x10;}
constexpr uint32_t depPar1(unsigned ep) {return 0xc804 + ep * 0x10;}
constexpr uint32_t depPar0(unsigned ep) {return 0xc808 + ep * 0x10;}
constexpr uint32_t depCmd(unsigned ep) {return 0xc80c + ep * 0x10;}
enum : uint32_t {kCmdDepCfg = 1, kCmdXferCfg = 2, kCmdSetStall = 4, kCmdStartXfer = 6, kCmdEndXfer = 8, kCmdStartCfg = 9};
constexpr uint32_t kCmdActive = 1u << 10;
enum : uint32_t {kTrbNormal = 1, kTrbSetup = 2, kTrbStatus2 = 3, kTrbStatus3 = 4, kTrbData = 5};
constexpr uint32_t kTrbHwo = 1u << 0, kTrbLst = 1u << 1, kTrbIspImi = 1u << 10, kTrbIoc = 1u << 11;
// Buffer layout.
constexpr uint32_t kDmaBytes = 0x10000, kEvents = 0x0, kEventBytes = 0x1000, kEp0Trb = 0x1000, kSetup = 0x1040, kEp0Buf = 0x1100,
                   kTxTrb = 0x1400, kTxBuf = 0x2000, kTxMax = kDmaBytes - kTxBuf;
// Physical endpoints: 0 ep0 OUT, 1 ep0 IN, 2 ep1 OUT (data from the host, never armed), 3 ep1 IN (the log), 5 ep2 IN (notifications).
enum : unsigned {kEp0Out = 0, kEp0In = 1, kBulkOut = 2, kBulkIn = 3, kNotify = 5};

// Descriptors: one configuration, the CDC communication interface (ACM, with its header, call management, ACM and union functional
// descriptors and an interrupt endpoint) and the CDC data interface (bulk OUT and IN). The test VID/PID of pid.codes.
const uint8_t kDeviceDescriptor[18] = {18, 1, 0x00, 0x02, 0x02, 0x00, 0x00, 64, 0x09, 0x12, 0x01, 0x00, 0x00, 0x01, 1, 2, 3, 1};
const uint8_t kQualifierDescriptor[10] = {10, 6, 0x00, 0x02, 0x02, 0x00, 0x00, 64, 1, 0};
uint8_t gConfigDescriptor[67] = {
    9, 2, 67, 0, 2, 1, 0, 0xc0, 50,
    9, 4, 0, 0, 1, 0x02, 0x02, 0x01, 0,
    5, 0x24, 0x00, 0x10, 0x01,
    5, 0x24, 0x01, 0x00, 0x01,
    4, 0x24, 0x02, 0x02,
    5, 0x24, 0x06, 0x00, 0x01,
    7, 5, 0x82, 0x03, 16, 0, 9,
    9, 4, 1, 0, 2, 0x0a, 0x00, 0x00, 0,
    7, 5, 0x01, 0x02, 0x00, 0x02, 0,
    7, 5, 0x81, 0x02, 0x00, 0x02, 0};
const char *const kStrings[] = {nullptr, "UniXNU", "TB128FU board log", "tb128fu"};

// The log queue: BootLog's writer appends (tb128fu_usb_log_put), this thread sends. When full the oldest bytes go.
constexpr uint32_t kQueueBytes = 512 * 1024;
uint8_t *gQueue;
uint32_t gQueueHead, gQueueLength;   // the oldest byte and how many are queued
IOLock *gQueueLock;

struct Gadget {
    OSSharedPtr<IOBufferMemoryDescriptor> dma;
    uint8_t *cpu = nullptr;
    uint64_t phys = 0;
    uint32_t eventAt = 0;
    enum {Setup, DataIn, DataOut, StatusWait, Status} ep0 = Setup;
    unsigned statusEp = kEp0In;          // the endpoint of the pending status phase
    bool threeStage = false, notReady[2] = {false, false};
    uint8_t request[8] = {};
    uint32_t outLength = 0;              // the OUT data phase's requested length
    bool configured = false, opened = false, txBusy = false;
    uint32_t txLength = 0, txResource = 0, speed = 0, address = 0;
    // Counters for the [usb] rows, and the newest raw events.
    uint32_t events = 0, resets = 0, connects = 0, setups = 0, stalls = 0, sent = 0, cmdFailures = 0, lastCmd = 0;
    uint64_t sentBytes = 0;
    uint32_t recent[16] = {};
} gGadget;

void cleanRange(uint32_t offset, uint32_t length) {
    uint64_t ctr; __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    const uintptr_t line = 4u << ((ctr >> 16) & 15), start = reinterpret_cast<uintptr_t>(gGadget.cpu + offset);
    for (uintptr_t a = start & ~(line - 1); a < start + length; a += line) __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

// An endpoint command: its three parameters, the command with CMDACT, then CMDACT polled clear (status in [15:12]).
bool endpointCommand(unsigned ep, uint32_t command, uint32_t p0 = 0, uint32_t p1 = 0, uint32_t p2 = 0) {
    wr(dwc, depPar0(ep), p0); wr(dwc, depPar1(ep), p1); wr(dwc, depPar2(ep), p2);
    wr(dwc, depCmd(ep), command | kCmdActive);
    const bool done = waitFor(dwc, depCmd(ep), kCmdActive, 0, 50000);
    const uint32_t result = rd(dwc, depCmd(ep));
    gGadget.lastCmd = (ep << 24) | (command & 0xff) << 16 | (result >> 12 & 15);
    if (!done || (result >> 12 & 15)) {++gGadget.cmdFailures; return false;}
    return true;
}
// DEPCFG: type (0 control, 2 bulk, 3 interrupt), max packet, the TX FIFO of an IN endpoint, XferComplete events, and XferNotReady
// on the control endpoint only (it starts the status phase there; on the others it would report every poll of an idle host).
bool configureEndpoint(unsigned ep, uint32_t type, uint32_t maxPacket, uint32_t action, uint32_t interval = 0) {
    const uint32_t fifo = (ep & 1) ? (ep >> 1) : 0;
    const uint32_t p0 = (type << 1) | (maxPacket << 3) | (fifo << 17) | (action << 30);
    const uint32_t p1 = (1u << 8) | (ep <= kEp0In ? 1u << 10 : 0) | (interval ? (interval - 1) << 16 : 0) | (ep << 25);
    return endpointCommand(ep, kCmdDepCfg, p0, p1);
}
bool startTransfer(unsigned ep, uint32_t trbOffset, uint32_t bufferOffset, uint32_t length, uint32_t type) {
    uint32_t *trb = reinterpret_cast<uint32_t *>(gGadget.cpu + trbOffset);
    const uint64_t buffer = gGadget.phys + bufferOffset;
    trb[0] = uint32_t(buffer); trb[1] = uint32_t(buffer >> 32); trb[2] = length;
    trb[3] = (type << 4) | kTrbHwo | kTrbLst | kTrbIoc | kTrbIspImi;
    cleanRange(trbOffset, 16);
    const uint64_t address = gGadget.phys + trbOffset;
    if (!endpointCommand(ep, kCmdStartXfer, uint32_t(address >> 32), uint32_t(address))) return false;
    if (ep == kBulkIn) gGadget.txResource = (rd(dwc, depCmd(ep)) >> 16) & 0x7f;
    return true;
}
void startSetup() {
    gGadget.ep0 = Gadget::Setup;
    gGadget.notReady[0] = gGadget.notReady[1] = false;
    startTransfer(kEp0Out, kEp0Trb, kSetup, 8, kTrbSetup);
}
void stallEp0() {
    ++gGadget.stalls;
    endpointCommand(kEp0Out, kCmdSetStall);
    startSetup();
}
// The status phase on its endpoint, once the controller asked for it (XferNotReady with the control status stage).
void startStatus() {
    gGadget.ep0 = Gadget::Status;
    startTransfer(gGadget.statusEp, kEp0Trb, kEp0Buf, 0, gGadget.threeStage ? kTrbStatus3 : kTrbStatus2);
}
void awaitStatus(unsigned ep, bool threeStage) {
    gGadget.ep0 = Gadget::StatusWait;
    gGadget.statusEp = ep;
    gGadget.threeStage = threeStage;
    if (gGadget.notReady[ep]) startStatus();
}
void sendEp0(const void *data, uint32_t length) {
    const uint32_t wanted = gGadget.request[6] | gGadget.request[7] << 8;
    if (length > wanted) length = wanted;
    if (length > 256) length = 256;
    memcpy(gGadget.cpu + kEp0Buf, data, length);
    cleanRange(kEp0Buf, 256);
    gGadget.ep0 = Gadget::DataIn;
    startTransfer(kEp0In, kEp0Trb, kEp0Buf, length, kTrbData);
}
// SET_CONFIGURATION 1: the bulk and interrupt endpoints (Linux's dwc3_gadget_start_config issues DEPSTARTCFG with transfer
// resource 2 once, then each endpoint's DEPCFG and DEPXFERCFG, then DALEPENA).
bool configureInterfaces() {
    const uint32_t bulk = gGadget.speed == 0 ? 512 : 64;
    gConfigDescriptor[57] = uint8_t(bulk); gConfigDescriptor[58] = uint8_t(bulk >> 8);   // wMaxPacketSize of 0x01 and 0x81
    gConfigDescriptor[64] = uint8_t(bulk); gConfigDescriptor[65] = uint8_t(bulk >> 8);
    bool ok = endpointCommand(kEp0Out, kCmdStartCfg | (2u << 16));
    ok = ok && configureEndpoint(kBulkOut, 2, bulk, 0) && endpointCommand(kBulkOut, kCmdXferCfg, 1);
    ok = ok && configureEndpoint(kBulkIn, 2, bulk, 0) && endpointCommand(kBulkIn, kCmdXferCfg, 1);
    ok = ok && configureEndpoint(kNotify, 3, 16, 0, 9) && endpointCommand(kNotify, kCmdXferCfg, 1);
    if (ok) wr(dwc, kDalepena, rd(dwc, kDalepena) | (1u << kBulkOut) | (1u << kBulkIn) | (1u << kNotify));
    return ok;
}
void endBulkTransfer() {
    if (gGadget.txBusy) endpointCommand(kBulkIn, kCmdEndXfer | (1u << 11) | (gGadget.txResource << 16));
    gGadget.txBusy = false;
}

void handleSetup() {
    cleanRange(kSetup, 8);
    memcpy(gGadget.request, gGadget.cpu + kSetup, 8);
    ++gGadget.setups;
    const uint8_t type = gGadget.request[0], request = gGadget.request[1];
    const uint16_t value = gGadget.request[2] | gGadget.request[3] << 8, length = gGadget.request[6] | gGadget.request[7] << 8;
    if ((type & 0x60) == 0x00) {   // standard
        switch (request) {
        case 6: {                  // GET_DESCRIPTOR
            const uint8_t kind = value >> 8, index = value & 0xff;
            if (kind == 1) {sendEp0(kDeviceDescriptor, sizeof kDeviceDescriptor); return;}
            if (kind == 2) {sendEp0(gConfigDescriptor, sizeof gConfigDescriptor); return;}
            if (kind == 6) {sendEp0(kQualifierDescriptor, sizeof kQualifierDescriptor); return;}
            if (kind == 3 && index == 0) {const uint8_t languages[4] = {4, 3, 0x09, 0x04}; sendEp0(languages, 4); return;}
            if (kind == 3 && index < sizeof kStrings / sizeof kStrings[0]) {
                uint8_t text[64] = {0, 3};
                unsigned n = 2;
                for (const char *c = kStrings[index]; *c && n + 2 <= sizeof text; ++c) {text[n++] = uint8_t(*c); text[n++] = 0;}
                text[0] = uint8_t(n);
                sendEp0(text, n);
                return;
            }
            break;
        }
        case 5:                    // SET_ADDRESS
            gGadget.address = value & 0x7f;
            setBits(dwc, kDcfg, 0x7fu << 3, gGadget.address << 3);
            awaitStatus(kEp0In, false);
            return;
        case 9:                    // SET_CONFIGURATION
            if (value == 1 && !gGadget.configured) gGadget.configured = configureInterfaces();
            if (value == 0) {endBulkTransfer(); gGadget.configured = false;}
            awaitStatus(kEp0In, false);
            return;
        case 8: {const uint8_t current = gGadget.configured ? 1 : 0; sendEp0(&current, 1); return;}   // GET_CONFIGURATION
        case 0: {const uint8_t status[2] = {0, 0}; sendEp0(status, 2); return;}                          // GET_STATUS
        case 10: {const uint8_t alternate = 0; sendEp0(&alternate, 1); return;}                         // GET_INTERFACE
        case 1: case 3: case 11:   // CLEAR_FEATURE, SET_FEATURE, SET_INTERFACE: nothing to change
            if (!length) {awaitStatus(kEp0In, false); return;}
            break;
        }
    } else if ((type & 0x60) == 0x20) {   // CDC class requests
        if (request == 0x21) {const uint8_t coding[7] = {0x00, 0xc2, 0x01, 0x00, 0, 0, 8}; sendEp0(coding, 7); return;}   // GET_LINE_CODING
        if (request == 0x20 && length && length <= 64) {   // SET_LINE_CODING: take the data, then the status phase
            gGadget.ep0 = Gadget::DataOut;
            gGadget.outLength = length;
            startTransfer(kEp0Out, kEp0Trb, kEp0Buf, 64, kTrbData);
            return;
        }
        if (request == 0x22) {gGadget.opened = (value & 1) != 0; awaitStatus(kEp0In, false); return;}   // SET_CONTROL_LINE_STATE
        if (request == 0x23 && !length) {awaitStatus(kEp0In, false); return;}                            // SEND_BREAK
    }
    stallEp0();
}

// Device events (DEVT, bits [11:8]) and endpoint events (physical endpoint [5:1], type [9:6], status [15:12]).
void handleEvent(uint32_t event) {
    gGadget.recent[gGadget.events & 15] = event;
    ++gGadget.events;
    if (event & 1) {
        if ((event >> 1 & 0x7f) != 0) return;
        switch (event >> 8 & 15) {
        case 0:   // disconnect
            endBulkTransfer();
            gGadget.configured = gGadget.opened = false;
            break;
        case 1:   // USB reset
            ++gGadget.resets;
            endBulkTransfer();
            gGadget.configured = gGadget.opened = false;
            setBits(dwc, kDcfg, 0x7fu << 3, 0);
            break;
        case 2:   // connection done: the speed, and ep0's packet size modified to it
            ++gGadget.connects;
            gGadget.speed = rd(dwc, kDsts) & 7;
            configureEndpoint(kEp0Out, 0, 64, 2);
            configureEndpoint(kEp0In, 0, 64, 2);
            break;
        }
        return;
    }
    const unsigned ep = event >> 1 & 0x1f, kind = event >> 6 & 15, status = event >> 12 & 15;
    if (ep <= kEp0In) {
        if (kind == 3) {   // XferNotReady: status 2 is the control status stage
            if (status == 2) {
                gGadget.notReady[ep] = true;
                if (gGadget.ep0 == Gadget::StatusWait && gGadget.statusEp == ep) startStatus();
            }
            return;
        }
        if (kind != 1) return;   // XferComplete
        switch (gGadget.ep0) {
        case Gadget::Setup: handleSetup(); break;
        case Gadget::DataIn: awaitStatus(kEp0Out, true); break;
        case Gadget::DataOut: awaitStatus(kEp0In, true); break;
        case Gadget::Status: startSetup(); break;
        case Gadget::StatusWait: break;
        }
        return;
    }
    if (ep == kBulkIn && kind == 1) {   // the log transfer finished: drop what went
        IOLockLock(gQueueLock);
        const uint32_t done = gGadget.txLength < gQueueLength ? gGadget.txLength : gQueueLength;
        gQueueHead = (gQueueHead + done) % kQueueBytes; gQueueLength -= done;
        IOLockUnlock(gQueueLock);
        gGadget.sentBytes += done; ++gGadget.sent;
        gGadget.txBusy = false;
    }
}

// The next contiguous run of the queue into the transmit buffer and onto the bulk IN endpoint.
void sendQueued() {
    if (!gGadget.configured || gGadget.txBusy) return;
    IOLockLock(gQueueLock);
    uint32_t n = gQueueLength < kTxMax ? gQueueLength : kTxMax;
    const uint32_t first = kQueueBytes - gQueueHead < n ? kQueueBytes - gQueueHead : n;
    memcpy(gGadget.cpu + kTxBuf, gQueue + gQueueHead, first);
    memcpy(gGadget.cpu + kTxBuf + first, gQueue, n - first);
    IOLockUnlock(gQueueLock);
    if (!n) return;
    cleanRange(kTxBuf, n);
    gGadget.txLength = n;
    gGadget.txBusy = startTransfer(kBulkIn, kTxTrb, kTxBuf, n, kTrbNormal);
}

void gadgetRows() {
    snprintf(tb128fu_usb_rows[2], 120, "gadget dsts %08x speed %u addr %u cfg %u open %u | ev %u rst %u conn %u setup %u stall %u cmdfail %u last %08x",
             rd(dwc, kDsts), gGadget.speed, gGadget.address, gGadget.configured, gGadget.opened, gGadget.events, gGadget.resets,
             gGadget.connects, gGadget.setups, gGadget.stalls, gGadget.cmdFailures, gGadget.lastCmd);
    const uint32_t *trb = reinterpret_cast<const uint32_t *>(gGadget.cpu + kEp0Trb);
    cleanRange(kEp0Trb, 16);
    snprintf(tb128fu_usb_rows[1], 120, "log sent %u/%llu queued %u | ep0 %u/%u nr %u%u req %02x%02x%02x%02x%02x%02x%02x%02x trb %x %x",
             gGadget.sent, (unsigned long long)gGadget.sentBytes, gQueueLength, unsigned(gGadget.ep0), gGadget.statusEp, gGadget.notReady[0],
             gGadget.notReady[1], gGadget.request[0], gGadget.request[1], gGadget.request[2], gGadget.request[3], gGadget.request[4],
             gGadget.request[5], gGadget.request[6], gGadget.request[7], trb[2], trb[3]);
    // The newest twelve events, oldest first, in the row the PHY used before the gadget started.
    size_t used = (size_t)snprintf(tb128fu_usb_rows[3], 120, "events");
    const uint32_t count = gGadget.events < 12 ? gGadget.events : 12;
    for (uint32_t i = gGadget.events - count; i < gGadget.events && used < 112; ++i)
        used += (size_t)snprintf(tb128fu_usb_rows[3] + used, 120 - used, " %x", gGadget.recent[i & 15]);
}

// The controller's DMA through the apps SMMU (round 063: sCR0.USFCFG set, so an unmatched stream faults, and no group matched 0x120).
// As NativeUfs does it for its own stream (SmmuTakeover.h): a context bank no valid stream match points at and whose translation is
// off, set to stage-1 bypass in the AArch64 format (CBA2R VA64, CBAR type 1, SCTLR 0), then a free stream match group sending 0x120
// there. The banks are tried from the top down, so the ones UFS and the GPU took are passed over. Each write is read back; on any
// mismatch nothing further is written and the gadget does not start.
const char *mapStream() {
    const uint32_t idr0 = rd(smmu, 0x20), idr1 = rd(smmu, 0x24);
    unsigned groups = idr0 & 255, banks = idr1 & 255;
    const unsigned pages = 1u << (((idr1 >> 28) & 7) + 1);
    if (groups > 128) groups = 128;
    if ((idr1 >> 31) || banks < 2 || (pages + banks) * 4096 > kSmmuSize) return "unsupported SMMU geometry";
    int freeGroup = -1;
    uint64_t usedBanks = 0, usedBanksHigh = 0;
    for (unsigned i = 0; i < groups; ++i) {
        const uint32_t smr = rd(smmu, 0x800 + 4 * i), s2cr = rd(smmu, 0xc00 + 4 * i);
        if (!(smr & 0x80000000u)) {if (freeGroup < 0) freeGroup = int(i); continue;}
        if ((smr & 0xffff) == kUsbStream && !((smr >> 16) & 0x7fff)) return "the USB stream is already mapped";
        if (((s2cr >> 16) & 3) == 0) {
            const unsigned bank = s2cr & 255;
            if (bank < 64) usedBanks |= 1ull << bank; else usedBanksHigh |= 1ull << (bank - 64);
        }
    }
    if (freeGroup < 0) return "no free stream match group";
    int bank = -1;
    for (int b = int(banks) - 1; b >= 1 && bank < 0; --b) {
        const bool used = b < 64 ? (usedBanks >> b & 1) : (usedBanksHigh >> (b - 64) & 1);
        if (!used && !(rd(smmu, (pages + unsigned(b)) * 4096) & 1)) bank = b;
    }
    if (bank < 0) return "no free context bank";
    const uint32_t cb = (pages + unsigned(bank)) * 4096;
    wr(smmu, cb, 0);                                       // SCTLR: translation off
    wr(smmu, 0x1800 + 4 * unsigned(bank), 1);              // CBA2R: VA64
    wr(smmu, 0x1000 + 4 * unsigned(bank), 0x10000);        // CBAR: stage 1 with stage 2 bypass
    if (rd(smmu, cb) != 0 || rd(smmu, 0x1800 + 4 * unsigned(bank)) != 1 || rd(smmu, 0x1000 + 4 * unsigned(bank)) != 0x10000)
        return "context bank readback";
    wr(smmu, 0xc00 + 4 * unsigned(freeGroup), unsigned(bank));
    if (rd(smmu, 0xc00 + 4 * unsigned(freeGroup)) != unsigned(bank)) return "stream to context readback";
    wr(smmu, 0x800 + 4 * unsigned(freeGroup), 0x80000000u | kUsbStream);
    if (rd(smmu, 0x800 + 4 * unsigned(freeGroup)) != (0x80000000u | kUsbStream)) return "stream match readback";
    snprintf(tb128fu_usb_rows[4], 120, "smmu stream 0x%x -> group %d, bank %d (bypass, VA64)", kUsbStream, freeGroup, bank);
    return nullptr;
}

// Device mode (dwc3_core_init and dwc3_gadget_start in their order): GCTL to device, the USB2 PHY kept out of suspend, the event
// buffer, high speed, the reset/connect/disconnect events, ep0 configured and armed for a SETUP, then RUN_STOP.
bool startGadget() {
    gGadget.dma = OSSharedPtr<IOBufferMemoryDescriptor>(IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, static_cast<IOOptionBits>(kIODirectionInOut) | static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous), kDmaBytes,
        0xFFFFF000ULL), OSNoRetain);
    if (!gGadget.dma || gGadget.dma->prepare() != kIOReturnSuccess) return false;
    gGadget.cpu = static_cast<uint8_t *>(gGadget.dma->getBytesNoCopy());
    gGadget.phys = gGadget.dma->getPhysicalSegment(0, nullptr);
    if (!gGadget.cpu || !gGadget.phys) return false;
    bzero(gGadget.cpu, kDmaBytes);
    cleanRange(0, kDmaBytes);
    setBits(dwc, kGctl, (3u << 12) | (3u << 4) | 1u, (2u << 12) | 1u);    // PRTCAPDIR device, no scale-down, clock gating off
    setBits(dwc, kGusb2PhyCfg, (1u << 6) | (1u << 8), 0);                  // SUSPHY and ENBLSLPM off
    wr(dwc, kGevntAdrLo, uint32_t(gGadget.phys + kEvents)); wr(dwc, kGevntAdrHi, uint32_t((gGadget.phys + kEvents) >> 32));
    wr(dwc, kGevntSiz, kEventBytes | (1u << 31));                          // the ring's size; its interrupt masked (polled)
    wr(dwc, kGevntCount, rd(dwc, kGevntCount) & 0xffff);                   // nothing pending
    setBits(dwc, kDcfg, 7u | (0x7fu << 3), 0);                             // high speed, address 0
    wr(dwc, kDevten, 7u);                                                  // disconnect, USB reset, connection done
    bool ok = endpointCommand(kEp0Out, kCmdStartCfg);
    ok = ok && configureEndpoint(kEp0Out, 0, 64, 0) && endpointCommand(kEp0Out, kCmdXferCfg, 1);
    ok = ok && configureEndpoint(kEp0In, 0, 64, 0) && endpointCommand(kEp0In, kCmdXferCfg, 1);
    if (!ok) return false;
    wr(dwc, kDalepena, 3u);
    startSetup();
    setBits(dwc, kDctl, 1u << 31, 1u << 31);                               // RUN_STOP
    return waitFor(dwc, kDsts, 1u << 22, 0, 500000);                       // DEVCTRLHLT clear
}

void serveGadget() {
    unsigned tick = 0;
    for (;;) {
        uint32_t pending = rd(dwc, kGevntCount) & 0xffff;
        while (pending >= 4) {
            cleanRange(kEvents + gGadget.eventAt, 4);
            const uint32_t event = *reinterpret_cast<volatile uint32_t *>(gGadget.cpu + kEvents + gGadget.eventAt);
            gGadget.eventAt = (gGadget.eventAt + 4) % kEventBytes;
            handleEvent(event);
            wr(dwc, kGevntCount, 4);
            pending -= 4;
        }
        if (kStage >= 4) sendQueued();
        if ((++tick & 255) == 0) gadgetRows();
        IOSleep(1);
    }
}

// Which stream match registers the controller's stream hits, and where they send it (read only).
void smmuRow() {
    const uint32_t idr0 = rd(smmu, 0x20);
    unsigned count = idr0 & 255;
    if (count > 128) count = 128;
    size_t used = (size_t)snprintf(tb128fu_usb_rows[4], 120, "smmu cr0 %08x idr0 %08x groups %u", rd(smmu, 0), idr0, count);
    unsigned hits = 0;
    for (unsigned i = 0; i < count && used < 100; ++i) {
        const uint32_t smr = rd(smmu, 0x800 + 4 * i);
        if (!(smr & 0x80000000u)) continue;
        const uint32_t mask = (smr >> 16) & 0x7fff;
        if (((smr ^ kUsbStream) & 0xffff & ~mask) != 0) continue;
        ++hits;
        used += (size_t)snprintf(tb128fu_usb_rows[4] + used, 120 - used, " [%u] smr %08x s2cr %08x", i, smr, rd(smmu, 0xc00 + 4 * i));
    }
    if (!hits) snprintf(tb128fu_usb_rows[4] + used, 120 - used, " no group matches 0x%x", kUsbStream);
}

void usbThread(void *, wait_result_t) {
    char *row = tb128fu_usb_rows[0];
    if (!mapRegion(kGcc, kGccSize, gcc) || !mapRegion(kDwc, kDwcSize, dwc) || !mapRegion(kPhy, kPhySize, phy) ||
        !mapRegion(kSmmu, kSmmuSize, smmu)) {
        snprintf(row, 120, "map failed");
        return;
    }
    smmuRow();
    // Power: the GDSC (SW_COLLAPSE bit 0 clear, PWR_ON bit 31 reported), the block resets (asserted while the clocks are off, as
    // dwc3-qcom pulses them before enabling its clocks), the two RCGs, then the branches (CLK_ENABLE bit 0, CLK_OFF bit 31 clear).
    const uint32_t gdscBefore = rd(gcc, kGdsc);
    wr(gcc, kGdsc, gdscBefore & ~1u);
    const bool powered = waitFor(gcc, kGdsc, 1u << 31, 1u << 31, 100000);
    wr(gcc, kUsbBcr, 1); wr(gcc, kPhyBcr, 1);
    IODelay(100);
    wr(gcc, kUsbBcr, 0); wr(gcc, kPhyBcr, 0);
    IODelay(100);
    const bool master = setRcg(kMasterRcg, 1, 8);   // GPLL0 main (600 MHz) / 4.5 = 133.33 MHz
    const bool utmi = setRcg(kUtmiRcg, 0, 0);       // XO, undivided = 19.2 MHz
    snprintf(row, 120, "power gdsc %08x->%08x%s rcg master %08x/%08x%s utmi %08x/%08x%s", gdscBefore, rd(gcc, kGdsc),
             powered ? "" : "!", rd(gcc, kMasterRcg), rd(gcc, kMasterRcg + 4), master ? "" : "!", rd(gcc, kUtmiRcg),
             rd(gcc, kUtmiRcg + 4), utmi ? "" : "!");
    bool running = powered && master && utmi;
    size_t used = (size_t)snprintf(tb128fu_usb_rows[1], 120, "branches");
    for (unsigned i = 0; i < kBranchCount; ++i) {
        wr(gcc, kBranches[i], rd(gcc, kBranches[i]) | 1u);
        const bool on = waitFor(gcc, kBranches[i], 1u << 31, 0u, 2000);
        running = running && on;
        used += (size_t)snprintf(tb128fu_usb_rows[1] + used, 120 - used, " %x%s", rd(gcc, kBranches[i]), on ? "" : "!");
    }
    tb128fu_usb_stage_done = 1;
    if (!running) {
        snprintf(tb128fu_usb_rows[2], 120, "controller not read: a clock or the GDSC did not come up");
        return;
    }
    snprintf(tb128fu_usb_rows[2], 120, "dwc3 id %08x hw0 %08x gctl %08x dctl %08x dsts %08x qs gen %08x hs %08x", rd(dwc, kGsnpsid),
             rd(dwc, kGhwparams0), rd(dwc, kGctl), rd(dwc, kDctl), rd(dwc, kDsts), rd(dwc, kQscratch + kQsGeneralCfg),
             rd(dwc, kQscratch + kQsHsPhyCtrl));
    snprintf(tb128fu_usb_rows[3], 120, "phy ctrl0 %x ctrl5 %x common %x/%x/%x cfg0 %x refclk %x", rd(phy, kUtmiCtrl0), rd(phy, kUtmiCtrl5),
             rd(phy, kCommon0), rd(phy, kCommon1), rd(phy, kCommon2), rd(phy, kPhyCfg0), rd(phy, kRefclkCtrl));
    if (kStage < 2) {tb128fu_usb_stage_done = kStage; return;}
    const bool reset = bringUpPhy();
    snprintf(tb128fu_usb_rows[3], 120, "phy ctrl0 %x ctrl5 %x common %x/%x/%x | reset %s dctl %08x dsts %08x gctl %08x qs %08x/%08x",
             rd(phy, kUtmiCtrl0), rd(phy, kUtmiCtrl5), rd(phy, kCommon0), rd(phy, kCommon1), rd(phy, kCommon2), reset ? "done" : "STUCK",
             rd(dwc, kDctl), rd(dwc, kDsts), rd(dwc, kGctl), rd(dwc, kQscratch + kQsGeneralCfg), rd(dwc, kQscratch + kQsHsPhyCtrl));
    if (!reset) return;
    tb128fu_usb_stage_done = 2;
    if (kStage < 3) return;
    if (const char *error = mapStream()) {
        snprintf(tb128fu_usb_rows[2], 120, "gadget not started: SMMU %s", error);
        return;
    }
    if (!startGadget()) {
        snprintf(tb128fu_usb_rows[2], 120, "gadget start failed: dsts %08x cmdfail %u last %08x", rd(dwc, kDsts), gGadget.cmdFailures,
                 gGadget.lastCmd);
        return;
    }
    tb128fu_usb_stage_done = 3;
    gadgetRows();
    serveGadget();
}
}  // namespace

// BootLog's writer hands over each snapshot and the message buffer's new text; kept until a host reads them, the oldest dropped
// when the queue is full.
extern "C" void tb128fu_usb_log_put(const void *data, size_t length) {
    if (!gQueueLock || !gQueue || !data) return;
    const uint8_t *bytes = static_cast<const uint8_t *>(data);
    if (length > kQueueBytes) {bytes += length - kQueueBytes; length = kQueueBytes;}
    IOLockLock(gQueueLock);
    const uint32_t overflow = gQueueLength + uint32_t(length) > kQueueBytes ? gQueueLength + uint32_t(length) - kQueueBytes : 0;
    gQueueHead = (gQueueHead + overflow) % kQueueBytes; gQueueLength -= overflow;
    for (size_t i = 0; i < length; ++i) gQueue[(gQueueHead + gQueueLength + i) % kQueueBytes] = bytes[i];
    gQueueLength += uint32_t(length);
    IOLockUnlock(gQueueLock);
}

extern "C" void tb128fu_usb_probe(void) {
    static bool started;
    if (started || !kStage) return;
    started = true;
    if (kStage >= 4) {
        gQueueLock = IOLockAlloc();
        gQueue = static_cast<uint8_t *>(IOMalloc(kQueueBytes));
    }
    thread_t thread = nullptr;
    if (kernel_thread_start(usbThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
}
