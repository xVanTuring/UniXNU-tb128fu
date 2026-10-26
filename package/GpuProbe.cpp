// SPDX-License-Identifier: BSD-2-Clause-Patent
// Default-off staged bring-up of the TB128FU's Adreno 610 (SM6225, a6xx without a GMU: the CPU owns the GPU's power domains,
// PLL and clocks). `tb128fu-gpu=<n>`:
//   1  read-only snapshot: the GCC branches the GPU needs and the speed-bin fuse; the GPU clock controller only if its
//      configuration bus clock is already on
//   2  power the GPU at 320 MHz in the stock kernel's order (GCC clocks, GPU_CC clocks, CX GDSC, GX GDSC with its reset pulse
//      and clamp release, PLL0 at 640 MHz with OUT_MAIN = VCO / 2, the gfx3d RCG, the core clock) and read the GPU's status
//      registers and its always-on counter
//   3  read-only view of the GPU's own SMMU (kgsl_smmu): identification, stream mapping and context banks as firmware left them
//   4  TrustZone queries only: IS_CALL_AVAIL for PAS init_image and PAS_IS_SUPPORTED for the GPU (PAS id 13)
//   5  the first GPU command: one context bank of the GPU's SMMU with a three-level table over a 64 KiB buffer, the a6xx
//      hardware init that the CP needs, the SQE microcode, the ring, then CP_ME_INIT and a CP_MEM_WRITE of a marker that the
//      CPU reads back (mainline a6xx_gpu.c hw_init / a6xx_cp_init, kgsl adreno_a6xx.c a6xx_start)
//   6  load the zap shader (a610_zap.elf) through PAS into the DT's reserved gpu_region@55615000, as UniXNU-sheng's
//      GpuPowerProbe.cpp zapAndSecure (qcom_mdt_load / qcom_scm_pas_*): init_image, mem_setup, segment copy, auth_and_reset,
//      only if that region lies outside the memory XNU manages; then CP_SET_SECURE_MODE(0) and a second marker
//   7  instead of 5 and 6, NativeAdreno (the driver the Metal stack talks to): its GPU context on the GPU's SMMU, the A610 hw_init,
//      ring, CP_ME_INIT, a CP_MEM_WRITE check, the zap shader and CP_SET_SECURE_MODE(0) through the ring, the GPU interrupt, then
//      the device published for user clients (UniXNU-sheng GpuPowerProbe.cpp gpuStart / irqAndTimer / publishAndWrapperTest)
//   8  7, then the staged accelerator for the original Metal discovery (sheng publishMetal); the plugin comes from the addon
// Before each access group the step is published, so a bus hang shows where it stopped. Sources (tablet DT, its msm-4.19
// kernel: gpucc-khaje.c, gcc-khaje.c, gdsc-regulator.c, clk-alpha-pll.c, kgsl adreno_a6xx.c) are cited per constant.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <libkern/c++/OSSharedPtr.h>
#include <IOKit/IOPlatformExpert.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSSymbol.h>
#include <mach/kmod.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include "NativeAdrenoGpuContext.h"
#include "NativeAdrenoRing.h"
#include "NativeAdrenoCpInit.h"
#include "NativeAdrenoIrq.h"
#include "NativeAdrenoSubmit.h"
#include "NativeAdrenoTimer.h"
#include "NativeAdrenoABI.h"
#include "NativeAdrenoUserClient.h"
#include <IOKit/IOUserClient.h>

extern "C" {
char tb128fu_gpu_rows[16][120];
// Stage 9: the RPM votes, the memory copy rate before and after the DDR vote and the boot clock (dvfsPrepare); the governor's
// level, switches, load and time at each level (dvfsThread).
char tb128fu_gpu_dvfs[2][120];
// The memory controller's clock and the CPU's read and copy rates before and after the DDR vote (dvfsPrepare); the clock as the
// governor samples it, lowest and highest of each second (dvfsThread).
char tb128fu_ddr_rows[2][120];
int cpu_number(void);
int tb128fu_rpm_open(void);
int tb128fu_rpm_request(uint32_t set, uint32_t type, uint32_t id, uint32_t key, uint32_t value);
volatile unsigned tb128fu_gpu_stage_req, tb128fu_gpu_stage_done, tb128fu_gpu_step;
volatile int tb128fu_gpu_error;
// Fence waits that slept until a GPU interrupt or spun (the boot log's GPU rows read these names).
volatile uint64_t native_gpu_idle_sleeps, native_gpu_idle_spins;
// The physical range XNU manages (osfmk/arm/pmap/pmap.c).
extern uint64_t vm_first_phys, vm_last_phys;
boolean_t ml_set_interrupts_enabled(boolean_t);
boolean_t ml_get_interrupts_enabled(void);
boolean_t ml_at_interrupt_context(void);
int get_preemption_level(void);
// Generated from the tablet's a610_zap.elf and a630_sqe.fw by scripts/build-native-legacy-display.py.
extern const uint8_t tb128fu_gpu_zap[], tb128fu_gpu_sqe[];
extern const uint32_t tb128fu_gpu_zap_size, tb128fu_gpu_sqe_size;
#include "AdrenoPM4.h"
// NativeAdreno (scripts/build-adreno610.py) and the GPU interrupt line of ArmCpu.
IOReturn NativeAdrenoGpuContextBringup(volatile uint8_t *, size_t, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned,
                                       void (*)(uint32_t), NativeAdrenoGmuContextReport *);
IOReturn NativeAdrenoGpuContextBuffers(NativeAdrenoGpuBuffer out[5]);
NativeAdrenoVm *NativeAdrenoGpuContextVm();
IOReturn native_adreno_publish(NativeAdrenoVm *, NativeAdrenoSubmit *, NativeAdrenoRing *, const NativeAdrenoRingClock *,
                               const NativeAdrenoTimer *, const NativeAdrenoAbiDeviceInfo *, uint32_t, uint32_t);
uint32_t native_adreno_publish_stats(uint32_t *);
IOService *native_adreno_published_service();
extern volatile uint32_t native_adreno_smmu_coherent_walk, native_adreno_pool_mib, native_adreno_wc_enabled, native_adreno_pool_status,
    native_adreno_pool_wc, native_adreno_pool_stats[8], native_adreno_written_imports_owned;
IOReturn armcpu_spi_attach(uint32_t intid, bool edge, void (*handler)(void *), void *context, uint32_t *readback, uint64_t *router);
// The staged AGXAccelerator (drivers/Adreno610/NativeAdrenoAccelerator.cpp, build-native-adreno-accelerator.py): its static
// initializer runs only after the original IOGPUFamily kext has loaded, because the parent metaclass does not exist before that.
void native_adreno_accelerator_static_init(void) __asm__("__GLOBAL__sub_I_NativeAdrenoAccelerator.cpp");
IOService *native_adreno_accelerator_allocate(void);
unsigned native_adreno_accelerator_instance_size(void);
kern_return_t OSKextLoadKextWithIdentifier(const char *);
extern kmod_info_t g_kernel_kmod_info;
}

namespace {
// Physical blocks (DTS: clock-controller@1400000, gpucc@5990000 size 0x9000, kgsl_3d0_reg_memory 0x5900000 size 0x90000,
// kgsl_smmu 0x59a0000 size 0x10000, qfprom 0x1b40000).
constexpr uint64_t kGcc = 0x01400000, kGpucc = 0x05990000, kGpu = 0x05900000, kSmmu = 0x059A0000, kQfprom = 0x01B40000;
// GCC (gcc-khaje.c): gpu_cfg_ahb forced on by the GCC probe; memnoc_gfx (voted, hwcg), snoc_dvm (the SMMU's), bimc_gpu_axi
// (iface), the GPLL0 vote (bit 0) and the GPU's GPLL0 / GPLL0-div sources (0x79004 bits 15, 16).
constexpr uint32_t kGccCfgAhb = 0x36004, kGccMemnoc = 0x3600c, kGccSnocDvm = 0x36018, kGccAxi = 0x71154, kGccVote = 0x79000,
                   kGccVote4 = 0x79004;
// GPU_CC (gpucc-khaje.c, gdsc DT nodes): PLL0 (Zonda) registers, GDSCs, RCGs and branches.
constexpr uint32_t kPllMode = 0x0, kPllL = 0x4, kPllAlpha = 0x8, kPllUser = 0xc, kPllConfig = 0x10, kPllConfigU = 0x14,
                   kPllConfigU1 = 0x18, kPllTest = 0x1c, kPllOpmode = 0x28, kPllStatus = 0x38;
constexpr uint32_t kGxBcr = 0x1008, kGxGdscr = 0x100c, kGfxCmd = 0x101c, kGfxCfg = 0x1020, kGfxCbcr = 0x1054, kGxCxo = 0x1060,
                   kCxGdscr = 0x106c, kAhbCbcr = 0x1078, kCxGmuCbcr = 0x1098, kCxoCbcr = 0x109c, kGmuCmd = 0x1120, kGmuCfg = 0x1124,
                   kGxDomainMisc = 0x1508, kCxGdsHwCtrl = 0x1540, kSmmuVote = 0x5000;
// GPU registers (dword offsets of a6xx_reg.h, times 4).
constexpr uint32_t kRbbmStatus = 0x210 * 4, kRbbmInt0Status = 0x201 * 4, kCpSqeCntl = 0x808 * 4, kAlwaysOnLo = 0x980 * 4,
                   kAlwaysOnHi = 0x981 * 4;

// PLL0's L and alpha: 640 MHz (gfx3d 320 MHz) unless stage 9's votes allow more.
uint32_t gPllL = 0x21, gPllAlpha = 0x5555;

struct Region {OSSharedPtr<IOMemoryMap> keep; uintptr_t va = 0;};
bool mapRegion(uint64_t phys, uint64_t length, Region &region) {
    IODeviceMemory *memory = IODeviceMemory::withRange(phys, length);
    if (!memory) return false;
    region.keep = OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
    memory->release();
    region.va = region.keep ? static_cast<uintptr_t>(region.keep->getVirtualAddress()) : 0;
    return region.va != 0;
}
uint32_t rd(const Region &r, uint32_t reg) {return *reinterpret_cast<volatile uint32_t *>(r.va + reg);}
void wr(const Region &r, uint32_t reg, uint32_t value) {
    *reinterpret_cast<volatile uint32_t *>(r.va + reg) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}
void set(const Region &r, uint32_t reg, uint32_t bits) {wr(r, reg, rd(r, reg) | bits);}
void clear(const Region &r, uint32_t reg, uint32_t bits) {wr(r, reg, rd(r, reg) & ~bits);}
// Polls until (reg & mask) == want; true on success.
bool poll(const Region &r, uint32_t reg, uint32_t mask, uint32_t want, unsigned us) {
    for (unsigned i = 0; i <= us; ++i) {
        if ((rd(r, reg) & mask) == want) return true;
        IODelay(1);
    }
    return false;
}

struct Session {
    Region gcc, gpucc, gpu, smmu, qfprom;
    uint32_t v[128];
    bool ccReadable = false;
    IOBufferMemoryDescriptor *zapMeta = nullptr;   // kept: TrustZone may refer to it after init_image
    IOBufferMemoryDescriptor *buf = nullptr;       // tables, ring, scratch and SQE (stage 5)
    uint8_t *cpu = nullptr;
    uint64_t phys = 0;
    uint32_t wptr = 0;                             // ring write pointer in dwords
    // Stage 7: NativeAdreno's report, its five kernel buffers (SQE, ring, memptrs, power-up list, scratch) with their
    // write-combined CPU views, and the values of rows 10..14.
    NativeAdrenoGmuContextReport report;
    NativeAdrenoGpuBuffer nb[5];
    OSSharedPtr<IOMemoryMap> nmap[5];
    volatile uint8_t *ncpu[5];
    uint32_t n[64];
};

void fail(unsigned step, int code) {
    if (!tb128fu_gpu_error) {tb128fu_gpu_step = step; tb128fu_gpu_error = code ? code : -1;}
}

void rows(const Session &s) {
    const uint32_t *v = s.v;
    snprintf(tb128fu_gpu_rows[0], sizeof tb128fu_gpu_rows[0],
        "GPU s%u/%u st%u e%d | gcc cfg%08x mem%08x dvm%08x axi%08x v0%08x v4%08x | bin%02x",
        tb128fu_gpu_stage_done, tb128fu_gpu_stage_req, tb128fu_gpu_step, tb128fu_gpu_error, v[0], v[1], v[2], v[3], v[4], v[5], v[6]);
    snprintf(tb128fu_gpu_rows[1], sizeof tb128fu_gpu_rows[1],
        "GPU pll %08x L%x A%x U%08x C%08x op%x st%08x | gfx rcg %08x/%08x gmu rcg %08x/%08x",
        v[7], v[8], v[9], v[10], v[11], v[12], v[13], v[14], v[15], v[16], v[17]);
    snprintf(tb128fu_gpu_rows[2], sizeof tb128fu_gpu_rows[2],
        "GPU cx %08x/%08x gx %08x bcr%x misc%08x | cbcr gfx%08x gxcxo%08x ahb%08x gmu%08x cxo%08x smmu%08x",
        v[18], v[19], v[20], v[21], v[22], v[23], v[24], v[25], v[26], v[27], v[28]);
    snprintf(tb128fu_gpu_rows[3], sizeof tb128fu_gpu_rows[3],
        "GPU rbbm st%08x int%08x sqe%08x aon %08x:%08x -> %08x:%08x (1 ms)",
        v[29], v[30], v[31], v[33], v[34], v[35], v[36]);
    snprintf(tb128fu_gpu_rows[4], sizeof tb128fu_gpu_rows[4],
        "GPU smmu cr0%08x id0%08x id1%08x id2%08x smr %08x %08x %08x %08x",
        v[37], v[38], v[39], v[40], v[41], v[42], v[43], v[44]);
    snprintf(tb128fu_gpu_rows[5], sizeof tb128fu_gpu_rows[5],
        "GPU s2cr %08x %08x %08x %08x cbar %08x %08x %08x %08x cb0 sctlr%08x",
        v[45], v[46], v[47], v[48], v[49], v[50], v[51], v[52], v[53]);
    // XNU's physical range; TrustZone queries: IS_CALL_AVAIL(init_image) a0/a1, PAS_IS_SUPPORTED(13) a0/a1.
    snprintf(tb128fu_gpu_rows[6], sizeof tb128fu_gpu_rows[6],
        "GPU phys %llx-%llx | tz avail %d/%u supported %d/%u ms%u",
        (unsigned long long)vm_first_phys, (unsigned long long)vm_last_phys, int(v[54]), v[55], int(v[56]), v[57], v[58]);
    // Zap: step, init a0/a1, mem_setup a0/a1, auth a0/a1, ELF facts (headers, hash index, relocatable, span), times.
    snprintf(tb128fu_gpu_rows[7], sizeof tb128fu_gpu_rows[7],
        "GPU zap z%d i%d/%u m%d/%u a%d/%u | ph%u hash%u rel%u span%x meta%x ms%u/%u/%u res%u",
        int(v[59]), int(v[60]), v[61], int(v[62]), v[63], int(v[64]), v[65], v[66], v[67], v[68], v[69], v[70], v[71], v[72], v[73],
        v[74]);
    // Stage 5: buffer, SMMU readback (CB0 SCTLR/TCR/TTBR0, SMR0, S2CR0, CBAR0), SQE version.
    snprintf(tb128fu_gpu_rows[8], sizeof tb128fu_gpu_rows[8],
        "GPU mmu buf%x sctlr%x tcr%x ttbr%x smr%08x s2cr%08x cbar%08x | sqe v%x",
        v[80], v[81], v[82], v[83], v[84], v[85], v[86], v[87]);
    // The CP after each submission: marker 1 and 2, RBBM_STATUS, RPTR/WPTR, CP_HW_FAULT, CP_INTERRUPT_STATUS, RBBM_INT_0,
    // CB0 FSR / FAR, wait in ms.
    snprintf(tb128fu_gpu_rows[9], sizeof tb128fu_gpu_rows[9],
        "GPU cp m%08x/%08x st%08x r%u/%u hw%x cpi%x int%x fsr%x far%x ms%u/%u",
        v[88], v[89], v[90], v[91], v[92], v[93], v[94], v[95], v[96], v[97], v[98], v[99]);
    if (tb128fu_gpu_stage_req < 7) return;
    // Stage 7. Context: bring-up status, the report's step, kit status of program/attach, program phase and enable, route
    // phase and verified streams, mapped spans, output bits; CB0 ACTLR before/after; BO pool MiB, its status, write-combine.
    const NativeAdrenoGmuContextReport &q = s.report;
    const uint32_t *n = s.n;
    snprintf(tb128fu_gpu_rows[10], sizeof tb128fu_gpu_rows[10],
        "GPU na ctx%x s%u k%u/%u p%ue%u r%uv%u n%u oas%u | actlr %x>%x | pool %u st%x wc%x | sctlr%x s2cr%x smr%x",
        n[0], q.step, q.program_status, q.route_status, q.program_phase, q.program_enabled, q.route_phase, q.routes_verified,
        q.spans, q.output_bits, n[1], n[2], native_adreno_pool_stats[0], native_adreno_pool_status, native_adreno_pool_wc,
        q.sctlr, q.s2cr, q.smr);
    // CP: stage reached, SQE readback mismatch, ring attach, CP init status, its fence wait and observed value, CP_MEM_WRITE
    // fence wait and the word read back, RBBM_STATUS, CP_RB_RPTR/WPTR, CP_HW_FAULT, CP_INTERRUPT_STATUS, CB0 FSR; RBBM_CLOCK_CNTL
    // (0xaaa8aa82 with clock gating) and CP_PROTECT_CNTL (0xb with protection).
    snprintf(tb128fu_gpu_rows[11], sizeof tb128fu_gpu_rows[11],
        "GPU na cp c%u sqe%x ring%x init%x w%u/%u mw%u %08x | st%08x r%u/%u hw%x cpi%x fsr%x | cg%08x prot%x",
        n[4], n[5], n[6], n[7], n[8], n[9], n[10], n[11], n[12], n[13], n[14], n[15], n[16], n[17], n[54], n[55]);
    // Secure mode (zap row above) through the ring: wait result; the GPU interrupt: attach status, GICD readback (group,
    // enabled, edge, priority), count after attach / after a fence, statuses seen, status now.
    snprintf(tb128fu_gpu_rows[12], sizeof tb128fu_gpu_rows[12],
        "GPU na sec w%d | irq a%x b%x n%u/%u%c seen%x now%x",
        int(n[18]), n[19], n[20], n[21], n[22], n[23] ? '+' : '-', n[24], n[25]);
    // Publication: submit attach, timer attach, publish; the user-client self test: steps passed of steps, first error and
    // its step, matching dwords (of 4) of rounds 1 and 2, the first clientMemoryForType status; Metal accelerator stage, kext
    // load, status, registered.
    snprintf(tb128fu_gpu_rows[13], sizeof tb128fu_gpu_rows[13],
        "GPU na pub sub%x tim%x pub%x | uc w%u/%u e%x@%d v%d/%d m%x | metal st%u k%d s%x reg%u",
        n[26], n[27], n[28], n[41], n[42], n[43], int(n[44]), int(n[45]), int(n[46]), n[47], n[29], int(n[30]), n[31], n[32]);
    // The stream-map slots before NativeAdreno claims 0 and 1 (S2CR 0..3, SMR 0/1), and after its attach (report).
    snprintf(tb128fu_gpu_rows[15], sizeof tb128fu_gpu_rows[15],
        "GPU na slots s2cr %08x %08x %08x %08x smr %08x %08x | attached s2cr %08x smr %08x cbar %08x",
        n[33], n[34], n[35], n[36], n[37], n[38], q.s2cr, q.smr, q.cbar);
}

void mark(const Session &s, unsigned step) {tb128fu_gpu_step = step; rows(s);}

void readGcc(Session &s) {
    s.v[0] = rd(s.gcc, kGccCfgAhb); s.v[1] = rd(s.gcc, kGccMemnoc); s.v[2] = rd(s.gcc, kGccSnocDvm); s.v[3] = rd(s.gcc, kGccAxi);
    s.v[4] = rd(s.gcc, kGccVote); s.v[5] = rd(s.gcc, kGccVote4);
}
void readGpucc(Session &s) {
    const Region &c = s.gpucc;
    s.v[7] = rd(c, kPllMode); s.v[8] = rd(c, kPllL); s.v[9] = rd(c, kPllAlpha); s.v[10] = rd(c, kPllUser); s.v[11] = rd(c, kPllConfig);
    s.v[12] = rd(c, kPllOpmode); s.v[13] = rd(c, kPllStatus);
    s.v[14] = rd(c, kGfxCmd); s.v[15] = rd(c, kGfxCfg); s.v[16] = rd(c, kGmuCmd); s.v[17] = rd(c, kGmuCfg);
    s.v[18] = rd(c, kCxGdscr); s.v[19] = rd(c, kCxGdsHwCtrl); s.v[20] = rd(c, kGxGdscr); s.v[21] = rd(c, kGxBcr);
    s.v[22] = rd(c, kGxDomainMisc);
    s.v[23] = rd(c, kGfxCbcr); s.v[24] = rd(c, kGxCxo); s.v[25] = rd(c, kAhbCbcr); s.v[26] = rd(c, kCxGmuCbcr); s.v[27] = rd(c, kCxoCbcr);
    s.v[28] = rd(c, kSmmuVote);
}

bool snapshot(Session &s) {
    mark(s, 1);
    if (!mapRegion(kGcc, 0x80000, s.gcc) || !mapRegion(kQfprom, 0x7000, s.qfprom)) {fail(1, -12); return false;}
    readGcc(s);
    // gpu_speed_bin: QFPROM 0x6006, 8 bits from bit 5 (DTS nvmem cell); 0xEB expected on this tablet.
    s.v[6] = (rd(s.qfprom, 0x6004) >> 16 >> 5) & 0xff;
    rows(s);
    // GPU_CC sits behind gcc_gpu_cfg_ahb_clk: read it only if that branch runs.
    s.ccReadable = !(s.v[0] & (1u << 31));
    if (!mapRegion(kGpucc, 0x9000, s.gpucc)) {fail(1, -12); return false;}
    if (s.ccReadable) {mark(s, 11); readGpucc(s);}
    return true;
}

// The stock order (kgsl_pwrctrl: regulators then clocks; gdsc-regulator.c; clk-alpha-pll.c zonda; gpucc-khaje.c probe).
bool powerOn(Session &s) {
    const Region &g = s.gcc, &c = s.gpucc;
    mark(s, 20);
    set(g, kGccCfgAhb, 1);                       // the CPU's path to GPU_CC and the GPU
    set(g, kGccVote, 1);                         // GPLL0
    set(g, kGccVote4, (1u << 15) | (1u << 16));  // gcc_gpu_gpll0_clk_src, gcc_gpu_gpll0_div_clk_src (GMU RCG parents)
    set(g, kGccAxi, 1);
    set(g, kGccMemnoc, 1);
    set(g, kGccSnocDvm, 1);
    IODelay(10);
    readGcc(s);
    if (s.v[0] & (1u << 31)) {fail(20, -110); return false;}
    s.ccReadable = true;
    mark(s, 21);
    readGpucc(s);                                 // the state before any GPU_CC write
    // gpucc probe: GX CXO always on; GMU clock wake/sleep fields; CXO, AHB and the SMMU vote.
    set(c, kGxCxo, 1);
    wr(c, kCxGmuCbcr, (rd(c, kCxGmuCbcr) & ~0xff0u) | 0xf00u | 0xf0u);
    set(c, kCxoCbcr, 1);
    set(c, kAhbCbcr, 1);
    set(c, kSmmuVote, 1);
    // gpu_cc_gmu_clk_src at 200 MHz: GPLL0 (src 5) / 3 (hid 5): CFG 0x505, then UPDATE.
    wr(c, kGmuCfg, 0x505);
    set(c, kGmuCmd, 1);
    if (!poll(c, kGmuCmd, 1, 0, 500)) {fail(21, -110); readGpucc(s); return false;}
    set(c, kCxGmuCbcr, 1);
    // CX GDSC (DT gpu_cx_gdsc: clk-dis-wait 8, status in GDS_HW_CTRL bit 31): no HW_CONTROL, no SW_OVERRIDE, then power.
    mark(s, 22);
    wr(c, kCxGdscr, (rd(c, kCxGdscr) & ~((0xfu << 12) | 6u)) | (8u << 12));
    clear(c, kCxGdscr, 1);
    IODelay(1);
    if (!poll(c, kCxGdsHwCtrl, 1u << 31, 1u << 31, 500)) {fail(22, -110); readGpucc(s); return false;}
    IODelay(2);
    // GX GDSC: pulse the GX block reset, release the GMEM clamp, then power.
    mark(s, 23);
    clear(c, kGxGdscr, 6u);
    set(c, kGxBcr, 1);
    IODelay(1);
    clear(c, kGxBcr, 1);
    clear(c, kGxDomainMisc, 1);
    clear(c, kGxGdscr, 1);
    IODelay(1);
    if (!poll(c, kGxGdscr, 1u << 31, 1u << 31, 500)) {fail(23, -110); readGpucc(s); return false;}
    IODelay(2);
    // PLL0 (Zonda) at 640 MHz, or at twice the clock stage 9 chose (dvfsPrepare): configure (clk_zonda_pll_configure), then
    // enable (clk_zonda_pll_enable).
    mark(s, 24);
    if (rd(c, kPllMode) & (1u << 20)) {fail(24, -16); readGpucc(s); return false;}   // in FSM vote mode: not ours to drive
    wr(c, kPllL, gPllL);
    wr(c, kPllAlpha, gPllAlpha);
    wr(c, kPllConfig, 0x08200800);
    wr(c, kPllConfigU, 0x05022001);
    wr(c, kPllConfigU1, 0x00000010);
    wr(c, kPllUser, 0x01000101);
    clear(c, kPllMode, 2u);                       // BYPASSNL
    clear(c, kPllMode, 1u);                       // OUTCTRL
    wr(c, kPllOpmode, 0);                         // STANDBY
    set(c, kPllMode, 4u);                         // RESET_N
    set(c, kPllMode, 2u);
    IODelay(1);
    set(c, kPllMode, 4u);
    wr(c, kPllOpmode, 1);                         // RUN
    const uint32_t lock = (rd(c, kPllTest) & (1u << 16)) ? (1u << 29) : (1u << 31);
    if (!poll(c, kPllMode, lock, lock, 1000)) {fail(24, -110); readGpucc(s); return false;}
    set(c, kPllUser, 0xf);                        // the PLL's outputs
    set(c, kPllMode, 1u);                         // OUTCTRL
    // gfx3d RCG: PLL0 OUT_MAIN (src 1), divide by 1 (hid 1): 320 MHz.
    mark(s, 25);
    wr(c, kGfxCfg, 0x101);
    set(c, kGfxCmd, 1);
    if (!poll(c, kGfxCmd, 1, 0, 500)) {fail(25, -110); readGpucc(s); return false;}
    // Core clock with FORCE_MEM_CORE_ON and FORCE_MEM_PERIPH_ON (kgsl adreno_a6xx.c for A610).
    // Its CLK_OFF status is not reliable (gpucc-khaje.c: BRANCH_HALT_SKIP), so the GPU's own registers are the check.
    set(c, kGfxCbcr, (1u << 14) | (1u << 13) | 1u);
    IODelay(10);
    readGpucc(s);
    // The GPU itself: status, interrupt status, SQE state and the always-on counter twice. RBBM_SECVID_TRUST_CNTL (secure mode)
    // is left alone: whether the normal world may even read it is one of the open questions of the zap stage.
    mark(s, 26);
    if (!mapRegion(kGpu, 0x90000, s.gpu)) {fail(26, -12); return false;}
    s.v[29] = rd(s.gpu, kRbbmStatus); s.v[30] = rd(s.gpu, kRbbmInt0Status); s.v[31] = rd(s.gpu, kCpSqeCntl);
    s.v[33] = rd(s.gpu, kAlwaysOnHi); s.v[34] = rd(s.gpu, kAlwaysOnLo);
    IODelay(1000);
    s.v[35] = rd(s.gpu, kAlwaysOnHi); s.v[36] = rd(s.gpu, kAlwaysOnLo);
    return true;
}

// MMU-500 global space: sCR0 0x0, IDR0..2 0x20..0x28, SMRn 0x800, S2CRn 0xc00, CBARn (GR1) 0x1000; context banks from 0x8000 in
// 4 KiB pages (64 KiB block, half global, half banks).
bool smmuView(Session &s) {
    mark(s, 30);
    if (!mapRegion(kSmmu, 0x10000, s.smmu)) {fail(30, -12); return false;}
    s.v[37] = rd(s.smmu, 0x0); s.v[38] = rd(s.smmu, 0x20); s.v[39] = rd(s.smmu, 0x24); s.v[40] = rd(s.smmu, 0x28);
    for (unsigned i = 0; i < 4; ++i) {s.v[41 + i] = rd(s.smmu, 0x800 + 4 * i); s.v[45 + i] = rd(s.smmu, 0xc00 + 4 * i);}
    for (unsigned i = 0; i < 4; ++i) s.v[49 + i] = rd(s.smmu, 0x1000 + 4 * i);
    mark(s, 31);
    s.v[53] = rd(s.smmu, 0x8000);
    return true;
}

uint32_t msSince(uint64_t start) {
    uint64_t f = 0; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return uint32_t((mach_absolute_time() - start) / ((f ? f : 19200000) / 1000));
}
// SIP SMC as UniXNU-sheng's GpuPowerProbe.cpp scmCall (qcom_scm-smc.c): x0 function, x1 arginfo, x2.. arguments.
// QCOM_SCM_INTERRUPTED (1) resumes with x0 = 1 and the returned x6; QCOM_SCM_V2_EBUSY (-12) retries after 30 ms, at most
// 20 times; WAITQ_SLEEP (2) is returned to the caller (no SCM wait-queue interrupt here).
// tb128fu-022: PAS init_image answered INTERRUPTED 10000 times in a row (0.94 s) while this board takes about 1.5 million IRQ
// exceptions a second, so the probe now keeps this core's IRQs masked across the call and its resumes (tb128fu-gpu-scm=0
// leaves them enabled, as before); v[74] counts the resumes of the latest call.
uint32_t gScmResumes;
struct ScmRes {int64_t a0; uint64_t a1;};
ScmRes scmCallLoop(uint64_t fn, uint64_t arginfo, uint64_t p0, uint64_t p1, uint64_t p2);
ScmRes scmCall(uint64_t fn, uint64_t arginfo, uint64_t p0, uint64_t p1, uint64_t p2) {
    unsigned mask = 1;
    PE_parse_boot_argn("tb128fu-gpu-scm", &mask, sizeof mask);
    if (!mask) return scmCallLoop(fn, arginfo, p0, p1, p2);
    const boolean_t was = ml_set_interrupts_enabled(FALSE);
    const ScmRes r = scmCallLoop(fn, arginfo, p0, p1, p2);
    ml_set_interrupts_enabled(was);
    return r;
}
ScmRes scmCallLoop(uint64_t fn, uint64_t arginfo, uint64_t p0, uint64_t p1, uint64_t p2) {
    uint64_t first = fn, a6 = 0;
    gScmResumes = 0;
    for (unsigned resume = 0, busy = 0; resume < 10000; gScmResumes = resume) {
        register uint64_t x0 asm("x0") = first, x1 asm("x1") = arginfo, x2 asm("x2") = p0, x3 asm("x3") = p1;
        register uint64_t x4 asm("x4") = p2, x5 asm("x5") = 0, x6 asm("x6") = a6, x7 asm("x7") = 0;
        asm volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6), "+r"(x7)
                     :: "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
        if (x0 == 1) {first = 1; a6 = x6; ++resume; continue;}
        if (int64_t(x0) == -12 && busy++ < 20) {first = fn; a6 = 0; IODelay(30000); continue;}
        return ScmRes{int64_t(x0), x1};
    }
    return ScmRes{-1000, 0};
}
// ARM_SMCCC_CALL_VAL(STD, SMC_64, OWNER_SIP, svc << 8 | cmd): PIL is service 2, INFO service 6 (qcom_scm.h).
constexpr uint64_t kScmPil(uint32_t cmd) {return 0x42000000u | (0x02u << 8) | cmd;}
constexpr uint64_t kScmIsCallAvail = 0x42000601;
constexpr uint32_t kGpuPasId = 13;                     // DT qcom,kgsl-hyp qcom,pas-id
constexpr uint64_t kZapPhys = 0x55615000, kZapBytes = 0x2000;   // DT gpu_region@55615000, no-map

bool tzQueries(Session &s) {
    mark(s, 40);
    const uint64_t start = mach_absolute_time();
    // qcom_scm_is_call_available: ARGS(1), the function id of PAS init_image.
    ScmRes r = scmCall(kScmIsCallAvail, 1, kScmPil(0x01), 0, 0);
    s.v[54] = uint32_t(r.a0); s.v[55] = uint32_t(r.a1);
    mark(s, 41);
    // qcom_scm_pas_supported: PAS_IS_SUPPORTED (cmd 7), ARGS(1), the PAS id.
    r = scmCall(kScmPil(0x07), 1, kGpuPasId, 0, 0);
    s.v[56] = uint32_t(r.a0); s.v[57] = uint32_t(r.a1);
    s.v[58] = msSince(start);
    return s.v[54] == 0 && s.v[56] == 0;
}

uint32_t le32(const uint8_t *p) {return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;}
uint16_t le16(const uint8_t *p) {return uint16_t(p[0] | p[1] << 8);}
void cleanToPoc(const void *p, size_t len) {
    uint64_t ctr; __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    const size_t line = 4u << ((ctr >> 16) & 15);
    for (uintptr_t a = reinterpret_cast<uintptr_t>(p) & ~(line - 1); a < reinterpret_cast<uintptr_t>(p) + len; a += line)
        __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

// qcom_mdt_load for the zap shader (UniXNU-sheng zapAndSecure): the ELF header and program headers plus the hash segment as
// metadata for init_image, mem_setup for a relocatable image, the loadable segments copied into the reserved region, then
// auth_and_reset. v[59] is the step reached (negative: why it stopped).
bool zapLoad(Session &s) {
    uint32_t *v = s.v;
    auto stop = [&](int why) {v[59] = uint32_t(why); rows(s); return false;};
    mark(s, 50);
    v[59] = 1;
    // The region must not be memory XNU hands out: TrustZone locks it after authentication.
    if (kZapPhys + kZapBytes > vm_first_phys && kZapPhys < vm_last_phys) return stop(-20);
    const uint8_t *fw = tb128fu_gpu_zap;
    const uint64_t size = tb128fu_gpu_zap_size;
    if (size < 52 || fw[0] != 0x7f || fw[1] != 'E' || fw[2] != 'L' || fw[3] != 'F' || fw[4] != 1) return stop(-1);
    const uint32_t phoff = le32(fw + 28);
    const unsigned phent = le16(fw + 42), phnum = le16(fw + 44);
    v[66] = phnum;
    if (phent != 32 || phnum < 2 || phnum > 16 || phoff + uint64_t(phnum) * 32 > size) return stop(-2);
    auto ph = [&](unsigned i, unsigned field) {return le32(fw + phoff + i * 32 + field * 4);};   // type off va pa filesz memsz flags
    if (ph(0, 0) == 1) return stop(-3);
    unsigned hash = 0;
    for (unsigned i = 1; i < phnum && !hash; ++i) if (((ph(i, 6) >> 24) & 7) == 2) hash = i;
    v[67] = hash;
    if (!hash) return stop(-4);
    uint64_t lo = ~0ull, hi = 0;
    bool relocate = false;
    for (unsigned i = 0; i < phnum; ++i) {
        if (ph(i, 0) != 1 || ((ph(i, 6) >> 24) & 7) == 2 || !ph(i, 5)) continue;   // mdt_phdr_loadable
        if (ph(i, 6) & (1u << 27)) relocate = true;
        lo = lo < ph(i, 3) ? lo : ph(i, 3);
        const uint64_t end = (uint64_t(ph(i, 3)) + ph(i, 5) + 0xfff) & ~0xfffull;
        hi = hi > end ? hi : end;
    }
    v[68] = relocate; v[69] = uint32_t(hi - lo);
    if (lo >= hi || hi - lo > kZapBytes) return stop(-5);
    const uint64_t ehdr = ph(0, 4), hashBytes = ph(hash, 4), hashOff = ph(hash, 1);
    if (ehdr > size || hashOff + hashBytes > size || ehdr + hashBytes > 0x4000) return stop(-6);
    // Metadata: physically contiguous below 4 GiB, written back to the point of coherency (Linux: dma_alloc_coherent).
    v[59] = 2;
    const size_t metaBytes = size_t((ehdr + hashBytes + 0xfff) & ~0xfffull);
    s.zapMeta = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, static_cast<IOOptionBits>(kIODirectionInOut) | static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous),
        metaBytes, 0xFFFFF000ULL);
    if (!s.zapMeta || s.zapMeta->prepare() != kIOReturnSuccess) return stop(-7);
    uint8_t *meta = static_cast<uint8_t *>(s.zapMeta->getBytesNoCopy());
    const uint64_t metaPhys = s.zapMeta->getPhysicalSegment(0, nullptr);
    v[70] = uint32_t(metaPhys);
    if (!meta || !metaPhys || (metaPhys >> 32)) return stop(-8);
    for (uint64_t i = 0; i < ehdr; ++i) meta[i] = fw[i];
    for (uint64_t i = 0; i < hashBytes; ++i) meta[ehdr + i] = fw[hashOff + i];
    cleanToPoc(meta, metaBytes);
    // qcom_scm_pas_init_image: ARGS(2, VAL, RW).
    v[59] = 3; mark(s, 51);
    uint64_t start = mach_absolute_time();
    ScmRes r = scmCall(kScmPil(0x01), 2 | (2u << 6), kGpuPasId, metaPhys, 0);
    v[60] = uint32_t(r.a0); v[61] = uint32_t(r.a1); v[71] = msSince(start); v[74] = gScmResumes;
    if (r.a0 || r.a1) return stop(3);
    if (relocate) {
        // qcom_scm_pas_mem_setup: ARGS(3).
        v[59] = 4; mark(s, 52);
        start = mach_absolute_time();
        r = scmCall(kScmPil(0x02), 3, kGpuPasId, kZapPhys, hi - lo);
        v[62] = uint32_t(r.a0); v[63] = uint32_t(r.a1); v[72] = msSince(start);
        if (r.a0 || r.a1) return stop(4);
    }
    // qcom_mdt_load_no_init into the reserved region (relocatable: offsets from the lowest segment address).
    v[59] = 5; mark(s, 53);
    {
        IODeviceMemory *m = IODeviceMemory::withRange(kZapPhys, kZapBytes);
        OSSharedPtr<IOMemoryMap> map;
        if (m) {map = OSSharedPtr<IOMemoryMap>(m->map(kIOMapAnywhere | kIOMapWriteCombineCache), OSNoRetain); m->release();}
        if (!map) return stop(-9);
        volatile uint8_t *region = reinterpret_cast<volatile uint8_t *>(map->getVirtualAddress());
        const uint64_t base = relocate ? lo : kZapPhys;
        for (unsigned i = 0; i < phnum; ++i) {
            if (ph(i, 0) != 1 || ((ph(i, 6) >> 24) & 7) == 2 || !ph(i, 5)) continue;
            const uint64_t at = uint64_t(ph(i, 3)) - base, file = ph(i, 4), mem = ph(i, 5), off = ph(i, 1);
            if (ph(i, 3) < base || at + mem > kZapBytes || file > mem || off + file > size) return stop(-10);
            for (uint64_t b = 0; b < file; ++b) region[at + b] = fw[off + b];
            for (uint64_t b = file; b < mem; ++b) region[at + b] = 0;
        }
        __asm__ volatile("dsb sy" ::: "memory");
    }   // the CPU view is gone before TrustZone takes the region
    // qcom_scm_pas_auth_and_reset: ARGS(1).
    v[59] = 6; mark(s, 54);
    start = mach_absolute_time();
    r = scmCall(kScmPil(0x05), 1, kGpuPasId, 0, 0);
    v[64] = uint32_t(r.a0); v[65] = uint32_t(r.a1); v[73] = msSince(start);
    if (r.a0 || r.a1) return stop(6);
    v[59] = 7;   // TrustZone authenticated the zap shader
    return true;
}

// Stage 5. GPU memory: one physically contiguous 64 KiB buffer below 4 GiB. Neither the SMMU's table walk nor the GPU is
// coherent with the CPU caches (kgsl: no ADRENO_IOCOHERENT; kgsl_smmu "non-coherent table walk"), so the CPU writes through its
// cacheable mapping and cleans to the point of coherency, and invalidates before reading what the GPU wrote.
constexpr uint32_t kBufBytes = 0x10000, kOffL1 = 0x0, kOffL2 = 0x1000, kOffL3 = 0x2000, kOffRing = 0x3000, kOffScratch = 0x4000,
                   kOffSqe = 0x5000, kRingBytes = 0x1000;
constexpr uint64_t kGpuVa = 0x01000000;   // above the GMEM aperture 0x100000-0x120fff, below 4 GiB (ADRENO_QUIRK_4GB_VA)
// MMU-500 (arm-smmu.h): GR0 SMR/S2CR, GR1 CBAR/CBA2R, the context bank after the eight global pages (IDR1 NUMPAGENDXB 2).
constexpr uint32_t kSmr0 = 0x800, kS2cr0 = 0xc00, kCbar0 = 0x1000, kCba2r0 = 0x1800, kCb0 = 0x8000;
constexpr uint32_t kCbSctlr = 0x0, kCbTcr2 = 0x10, kCbTtbr0 = 0x20, kCbTtbr1 = 0x28, kCbTcr = 0x30, kCbContextIdr = 0x34,
                   kCbMair0 = 0x38, kCbMair1 = 0x3c, kCbFsr = 0x58, kCbFar = 0x60, kCbTlbiAll = 0x618, kCbTlbSync = 0x7f0,
                   kCbTlbStatus = 0x7f4;
void gw(const Session &s, uint32_t dword, uint32_t value) {wr(s.gpu, dword * 4, value);}
uint32_t gr(const Session &s, uint32_t dword) {return rd(s.gpu, dword * 4);}
uint64_t va(uint32_t off) {return kGpuVa + off;}

void cpRow(Session &s, unsigned at) {
    // marker at scratch+0 / +4 (invalidate first), RBBM_STATUS, CP_RB_RPTR, CP_RB_WPTR, CP_HW_FAULT, CP_INTERRUPT_STATUS,
    // RBBM_INT_0_STATUS, CB0 FSR, FAR.
    volatile uint32_t *scratch = reinterpret_cast<volatile uint32_t *>(s.cpu + kOffScratch);
    cleanToPoc(s.cpu + kOffScratch, 64);
    s.v[88] = scratch[0]; s.v[89] = scratch[1];
    s.v[90] = gr(s, 0x210); s.v[91] = gr(s, 0x806); s.v[92] = gr(s, 0x807); s.v[93] = gr(s, 0x821); s.v[94] = gr(s, 0x823);
    s.v[95] = gr(s, 0x201); s.v[96] = rd(s.smmu, kCb0 + kCbFsr); s.v[97] = rd(s.smmu, kCb0 + kCbFar);
    (void)at;
    rows(s);
}

// Appends words to the ring, writes it back to memory, then moves CP_RB_WPTR (a plain register write without a GMU).
void submit(Session &s, const uint32_t *words, unsigned count) {
    uint32_t *ring = reinterpret_cast<uint32_t *>(s.cpu + kOffRing);
    for (unsigned i = 0; i < count; ++i) ring[(s.wptr + i) % (kRingBytes / 4)] = words[i];
    cleanToPoc(ring, kRingBytes);
    s.wptr = (s.wptr + count) % (kRingBytes / 4);
    gw(s, 0x807, s.wptr);
}

// Polls the marker word (scratch index) for value, up to ms; returns the milliseconds taken or ~0u.
uint32_t waitMarker(Session &s, unsigned index, uint32_t value, unsigned ms) {
    const uint64_t start = mach_absolute_time();
    volatile uint32_t *scratch = reinterpret_cast<volatile uint32_t *>(s.cpu + kOffScratch);
    for (unsigned i = 0; i < ms * 10; ++i) {
        cleanToPoc(s.cpu + kOffScratch, 64);
        if (scratch[index] == value) return msSince(start);
        IODelay(100);
    }
    return ~0u;
}

bool firstCommand(Session &s) {
    uint32_t *v = s.v;
    mark(s, 60);
    s.buf = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, static_cast<IOOptionBits>(kIODirectionInOut) | static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous),
        kBufBytes, 0xFFFFF000ULL);
    if (!s.buf || s.buf->prepare() != kIOReturnSuccess) {fail(60, -12); return false;}
    s.cpu = static_cast<uint8_t *>(s.buf->getBytesNoCopy());
    s.phys = s.buf->getPhysicalSegment(0, nullptr);
    v[80] = uint32_t(s.phys);
    if (!s.cpu || !s.phys || (s.phys & 0xfff) || (s.phys >> 32)) {fail(60, -14); return false;}
    bzero(s.cpu, kBufBytes);
    // Stage-1 tables (VMSAv8-64, 4 KiB granule, 32-bit input: level 1 has 4 entries): every page of the buffer at kGpuVa + its
    // offset. Page descriptor: AF, AP[1] (unprivileged access, io-pgtable-arm without IOMMU_PRIV), AttrIndx 0 (Normal
    // non-cacheable in MAIR0), nG; non-shareable.
    uint64_t *l1 = reinterpret_cast<uint64_t *>(s.cpu + kOffL1), *l2 = reinterpret_cast<uint64_t *>(s.cpu + kOffL2),
             *l3 = reinterpret_cast<uint64_t *>(s.cpu + kOffL3);
    l1[(kGpuVa >> 30) & 3] = (s.phys + kOffL2) | 3;
    l2[(kGpuVa >> 21) & 511] = (s.phys + kOffL3) | 3;
    for (uint32_t off = 0; off < kBufBytes; off += 0x1000)
        l3[((kGpuVa + off) >> 12) & 511] = (s.phys + off) | (1ull << 11) | (1ull << 10) | (1ull << 6) | 3;
    // SQE: the file without its first dword (a6xx_ucode_load / adreno_load_fw).
    const uint32_t sqeBytes = tb128fu_gpu_sqe_size - 4;
    if (tb128fu_gpu_sqe_size < 8 || kOffSqe + sqeBytes > kBufBytes) {fail(60, -27); return false;}
    for (uint32_t i = 0; i < sqeBytes; ++i) s.cpu[kOffSqe + i] = tb128fu_gpu_sqe[4 + i];
    v[87] = le32(s.cpu + kOffSqe) & 0xfff;
    cleanToPoc(s.cpu, kBufBytes);
    // Context bank 0 (arm_smmu_write_context_bank order): AArch64 format, stage 1 with stage 2 bypass, VMID 1, non-shareable
    // bypass, write-back memattr; 36-bit output (IDR2 OAS 1); T0SZ 32, walks non-cacheable and non-shareable, TTBR1 disabled;
    // MAIR attr0 = Normal non-cacheable; SCTLR M, TRE, AFE, CFRE (faults abort the access; no context interrupt).
    mark(s, 61);
    const Region &m = s.smmu;
    wr(m, kCba2r0, 1);
    wr(m, kCbar0, (1u << 16) | (0xfu << 12) | (3u << 8) | 1u);
    wr(m, kCb0 + kCbTcr2, (7u << 15) | 1u);
    wr(m, kCb0 + kCbTtbr0, uint32_t(s.phys + kOffL1)); wr(m, kCb0 + kCbTtbr0 + 4, uint32_t((s.phys + kOffL1) >> 32));
    wr(m, kCb0 + kCbTtbr1, 0); wr(m, kCb0 + kCbTtbr1 + 4, 0);
    wr(m, kCb0 + kCbTcr, (1u << 23) | 32u);
    wr(m, kCb0 + kCbContextIdr, 0);
    wr(m, kCb0 + kCbMair0, 0x0004FF44); wr(m, kCb0 + kCbMair1, 0);
    wr(m, kCb0 + kCbFsr, 0xffffffff);
    wr(m, kCb0 + kCbSctlr, (1u << 5) | (1u << 2) | (1u << 1) | 1u);
    // Stream 0 with mask 1 (DT gfx3d_user iommus = <0 1>) to bank 0: S2CR first, then the valid SMR (arm_smmu_write_sme).
    wr(m, kS2cr0, 0);
    wr(m, kSmr0, (1u << 31) | (1u << 16) | 0u);
    wr(m, kCb0 + kCbTlbiAll, 0);
    wr(m, kCb0 + kCbTlbSync, 0);
    poll(m, kCb0 + kCbTlbStatus, 1, 0, 1000);
    v[81] = rd(m, kCb0 + kCbSctlr); v[82] = rd(m, kCb0 + kCbTcr); v[83] = rd(m, kCb0 + kCbTtbr0); v[84] = rd(m, kSmr0);
    v[85] = rd(m, kS2cr0); v[86] = rd(m, kCbar0);
    rows(s);
    // a6xx hardware init the CP depends on (mainline hw_init for A610; kgsl a6xx_start); no clock gating, no register
    // protection, interrupts masked (this probe polls).
    mark(s, 62);
    gw(s, 0x0016, 0); gw(s, 0x3C45, 0);                                   // RBBM_GBIF_HALT, GBIF_HALT
    static const uint32_t addrMode[] = {0x842, 0xC01, 0x8601, 0x8E05, 0x9E01, 0xBE05, 0xA601, 0x9601, 0xE00, 0xAE01, 0xB601, 0xF810};
    for (uint32_t reg : addrMode) gw(s, reg, 1);                           // *_ADDR_MODE_CNTL: 64-bit
    gw(s, 0xE05, 0xFFFFFFC0); gw(s, 0xE06, 0x0001FFFF);                    // UCHE_WRITE_RANGE_MAX
    gw(s, 0xE09, 0xFFFFF000); gw(s, 0xE0A, 0x0001FFFF);                    // UCHE_TRAP_BASE
    gw(s, 0xE07, 0xFFFFF000); gw(s, 0xE08, 0x0001FFFF);                    // UCHE_WRITE_THRU_BASE
    gw(s, 0xE0B, 0x00100000); gw(s, 0xE0C, 0);                             // UCHE_GMEM_RANGE_MIN
    gw(s, 0xE0D, 0x00120FFF); gw(s, 0xE0E, 0);                             // UCHE_GMEM_RANGE_MAX (132 KiB at 1 MiB)
    gw(s, 0xE18, 0x804); gw(s, 0xE17, 0x4);                                // UCHE_FILTER_CNTL, UCHE_CACHE_WAYS
    gw(s, 0x8C2, 0x00800060); gw(s, 0x8C1, 0x40201B16);                    // CP_ROQ_THRESHOLDS_2/_1
    gw(s, 0x8C3, 48); gw(s, 0x90E, 47);                                    // CP_MEM_POOL_SIZE, CP_MEM_POOL_DBG_ADDR (A610)
    gw(s, 0x9E00, 0x00080000);                                             // PC_DBG_ECO_CNTL: prim fifo threshold
    gw(s, 0x98D, 1);                                                       // CP_AHB_CNTL: error response
    gw(s, 0x500, 1);                                                       // RBBM_PERFCTR_CNTL
    gw(s, 0x001F, (1u << 30) | 0x3FFFF);                                   // RBBM_INTERFACE_HANG_INT_CNTL
    gw(s, 0xE19, 1);                                                       // UCHE_CLIENT_PF
    gw(s, 0x038, 0);                                                       // RBBM_INT_0_MASK
    // SQE and ring (a6xx_ucode_load, a6xx_hw_init): ring 4 KiB = 2^9 qwords, block 2^2 qwords, NO_UPDATE (no APRIV on A610).
    mark(s, 63);
    gw(s, 0x830, uint32_t(va(kOffSqe))); gw(s, 0x831, uint32_t(va(kOffSqe) >> 32));
    gw(s, 0x800, uint32_t(va(kOffRing))); gw(s, 0x801, uint32_t(va(kOffRing) >> 32));
    gw(s, 0x802, (1u << 27) | (2u << 8) | 9u);
    gw(s, 0x804, uint32_t(va(kOffScratch + 0x100))); gw(s, 0x805, uint32_t(va(kOffScratch + 0x100) >> 32));
    gw(s, 0x807, 0);
    s.wptr = 0;
    gw(s, 0x808, 1);                                                       // CP_SQE_CNTL: start the microcode
    IODelay(100);
    // CP_ME_INIT (mainline a6xx_cp_init) and a CP_MEM_WRITE of the first marker.
    mark(s, 64);
    uint32_t words[16] = {};
    if (AdrenoPM4Type7(0x48, 8, &words[0]) != AdrenoPM4Success || AdrenoPM4Type7(0x3D, 3, &words[9]) != AdrenoPM4Success) {
        fail(64, -22); return false;
    }
    words[1] = 0x2F; words[2] = 0x3; words[3] = 0x20000000;
    words[10] = uint32_t(va(kOffScratch)); words[11] = uint32_t(va(kOffScratch) >> 32); words[12] = 0xC0FFEE01;
    submit(s, words, 13);
    v[98] = waitMarker(s, 0, 0xC0FFEE01, 1000);
    cpRow(s, 0);
    if (v[98] == ~0u) {fail(64, -110); return false;}
    return true;
}

// Stage 6: the zap shader, then CP_SET_SECURE_MODE(0) (mainline a6xx_zap_shader_init / hw_init) and a second marker.
bool leaveSecure(Session &s) {
    if (!zapLoad(s)) return false;
    mark(s, 70);
    uint32_t words[8] = {};
    if (AdrenoPM4Type7(0x66, 1, &words[0]) != AdrenoPM4Success || AdrenoPM4Type7(0x3D, 3, &words[2]) != AdrenoPM4Success) {
        fail(70, -22); return false;
    }
    words[1] = 0;
    words[3] = uint32_t(va(kOffScratch + 4)); words[4] = uint32_t(va(kOffScratch + 4) >> 32); words[5] = 0xC0FFEE02;
    submit(s, words, 6);
    s.v[99] = waitMarker(s, 1, 0xC0FFEE02, 1000);
    cpRow(s, 1);
    if (s.v[99] == ~0u) {fail(70, -110); return false;}
    return true;
}

// Stage 7. GPU registers for NativeAdreno (dword indices) through the session's uncached window.
int naWrite(void *c, uint32_t reg, uint32_t value) {
    if (reg >= 0x90000 / 4) return 1;
    gw(*static_cast<Session *>(c), reg, value);
    return 0;
}
int naRead(void *c, uint32_t reg, uint32_t *value) {
    if (reg >= 0x90000 / 4) return 1;
    *value = gr(*static_cast<Session *>(c), reg);
    return 0;
}
int naBarrier(void *) {__asm__ volatile("dsb sy" ::: "memory"); return 0;}
// The kernel buffers are Normal non-cacheable for the GPU and write-combined for the CPU: ordering only, no cache maintenance.
int naPublish(void *, const volatile uint32_t *, size_t) {__asm__ volatile("dsb sy" ::: "memory"); return 0;}
int naAcquire(void *, volatile uint32_t *, size_t) {__asm__ volatile("dsb sy" ::: "memory"); return 0;}
uint64_t naNow(void *) {
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 1000;
}
// The GPU interrupt (DT kgsl_3d0_irq: SPI 177, level high = INTID 209). As a6xx_irq: read RBBM_INT_0_STATUS and clear it through
// RBBM_INT_CLEAR_CMD before anything else, so the level line drops before the EOI. A line with nothing to clear, or the same
// error status 100000 times in a row, masks the GPU interrupts (the waiters then fall back to their 100 us timeout).
volatile bool gIrqAttached;
volatile uint32_t gIrqCount, gIrqSeen, gIrqLast, gIrqRepeat, gIrqMasked;
void gpuIrq(void *c) {
    Session &s = *static_cast<Session *>(c);
    const uint32_t status = gr(s, 0x201);
    if (status) gw(s, 0x37, status);
    gIrqRepeat = (status == gIrqLast && !(status & (1u << 20))) ? gIrqRepeat + 1 : 0;
    gIrqLast = status;
    gIrqSeen = gIrqSeen | status;
    gIrqCount = gIrqCount + 1;
    if (gIrqRepeat > 100000) {gw(s, 0x38, 0); gIrqMasked = 1;}
    thread_wakeup(event_t(&gIrqCount));
}
// The fence waits' pause: with the line attached a waiter that may block sleeps until the next GPU interrupt or 100 us,
// whichever is first; before that, or where it may not block, it spins 20 us (sheng int-143).
int naIdle(void *) {
    if (gIrqAttached && ml_get_interrupts_enabled() && !ml_at_interrupt_context() && get_preemption_level() == 0) {
        assert_wait_timeout(event_t(&gIrqCount), THREAD_UNINT, 100, NSEC_PER_USEC);
        thread_block(THREAD_CONTINUE_NULL);
        native_gpu_idle_sleeps = native_gpu_idle_sleeps + 1;
    } else {
        IODelay(20);
        native_gpu_idle_spins = native_gpu_idle_spins + 1;
    }
    return 0;
}
void naProgress(uint32_t step) {tb128fu_gpu_step = 90 + step;}
int naTimerRead(void *c, uint32_t reg, uint32_t *value) {return naRead(c, reg, value);}

NativeAdrenoRing *gRing;
NativeAdrenoCpInit *gCpInit;
NativeAdrenoSubmit *gSubmit;
NativeAdrenoTimer *gTimer;
NativeAdrenoRingIo gRio;
NativeAdrenoRingCache gCache{nullptr, naPublish, naAcquire};
NativeAdrenoRingClock gClock{nullptr, naNow, naIdle};
NativeAdrenoTimeIo gTimerIo;
// Facts for the Vulkan driver: chip id from the tablet DT (qcom,chipid 0x06010001; Mesa's GPU id 610), GMEM 128 KiB + 4 KiB
// (adreno-gpulist.h, a6xx_catalog.c) at the 16 MiB window hw_init programs below (the 1 MiB window of Linux would hold
// NativeAdreno's SQE buffer at 0x100000), the UCHE trap base of a6xx (adreno_gpu.c), the client VA window above the kernel
// buffers and below the 2 GiB aperture, and the DT's UBWC facts (qcom,ubwc-mode 1 = UBWC 1.0, qcom,highest-bank-bit 14; the
// display side's swizzle 0x7; macrotile mode 0 as mainline's RBBM_NC_MODE_CNTL default).
const NativeAdrenoAbiDeviceInfo kDeviceInfo = {0x06010001u, 0x21000u, 0x1000000ull, 0x1fffffffff000ull, 0x20000000ull,
                                               0x60000000ull, 14, 7, 0, 1, NativeAdrenoDeviceClientIova, 0};

uint32_t smmuBank0(const Session &s) {
    // IDR1: PAGESIZE (bit 31) and NUMPAGENDXB (bits 30:28): the context banks start after 2^(NUMPAGENDXB + 1) pages.
    const uint32_t id1 = rd(s.smmu, 0x24);
    const uint32_t page = (id1 >> 31) ? 0x10000 : 0x1000;
    return page << (((id1 >> 28) & 7) + 1);
}

// The GPU's context: NativeAdrenoGpuContextBringup claims context bank 0 (arm-smmu-qcom: the GPU always gets bank 0) and
// stream-map slots 0 and 1 for SIDs 0 and 1 (DT kgsl_smmu iommus = <0 1>: SID 0 with mask 1), with a 2 GiB aperture of 4 KiB
// pages, then maps its five kernel buffers. The SM6225's GPU SMMU walks are not coherent (no dma-coherent in its node).
bool naContext(Session &s) {
    uint32_t *n = s.n;
    mark(s, 80);
    // SMR 0/1 invalid and S2CR 0/1 fault, as arm_smmu_device_reset leaves unused slots. This firmware traps S2CR: tb128fu-028
    // read 0x001200ff / 0x000209ff back after the fault writes (ignored), and on slot 3 a bypass write read back as 0x000200ff
    // (mainline qcom_smmu_cfg_probe's bypass quirk); translate writes do take (tb128fu-022: 0). The slots keep a fault type
    // with other fields, which ArmSmmu4K accepts for a slot it claims. Their values are recorded for row 15.
    for (unsigned i = 0; i < 2; ++i) {wr(s.smmu, 0x800 + 4 * i, 0); wr(s.smmu, 0xc00 + 4 * i, 0x20000);}
    for (unsigned i = 0; i < 4; ++i) n[33 + i] = rd(s.smmu, 0xc00 + 4 * i);
    n[37] = rd(s.smmu, 0x800); n[38] = rd(s.smmu, 0x804);
    native_adreno_smmu_coherent_walk = 0;
    native_adreno_wc_enabled = 1;
    // The A610 is not IO-coherent (kgsl adreno-gpulist.h gives it no ADRENO_IOCOHERENT), so every submission cleaned every import in
    // full: in tb128fu-045 a submission spent 12 ms on about 89 MB. The render targets the GPU writes are cleaned and invalidated
    // when mapped and left alone afterwards, as upstream does on coherent boards (NativeAdrenoVm.cpp importedUntouched); the
    // read-only imports, which their owners draw into with the CPU, are still cleaned every time.
    native_adreno_written_imports_owned = 1;
    // The BO pool, one contiguous block taken before memory fragments (sheng-gpu-pool on the 16 GiB SM8550 board, 1536 MiB);
    // this board has 4 GiB, so the default is 384 MiB; 0 = scattered BOs only.
    unsigned pool = 384;
    PE_parse_boot_argn("tb128fu-gpu-pool", &pool, sizeof pool);
    native_adreno_pool_mib = pool;
    mark(s, 81);
    const IOReturn ctx = NativeAdrenoGpuContextBringup(reinterpret_cast<volatile uint8_t *>(s.smmu.va), 0x10000, 0, 2, 0, 0, 1, 1,
                                                       naProgress, &s.report);
    n[0] = uint32_t(ctx);
    rows(s);
    if (ctx) {fail(81, int(ctx)); return false;}
    // CB0 ACTLR without CPRE (arm_mmu500_reset, MMU-500 errata 841119/826419: the next-page prefetcher can keep stale
    // translations after a TLB invalidation), then TLBIALL, TLBSYNC and TLBSTATUS of that bank.
    const uint32_t bank0 = smmuBank0(s);
    n[1] = rd(s.smmu, bank0 + 0x4);
    wr(s.smmu, bank0 + 0x4, n[1] & ~2u);
    wr(s.smmu, bank0 + 0x618, 0);
    wr(s.smmu, bank0 + 0x7f0, 0);
    poll(s.smmu, bank0 + 0x7f4, 1, 0, 1000);
    n[2] = rd(s.smmu, bank0 + 0x4);
    rows(s);
    return true;
}

void gw64(const Session &s, uint32_t dword, uint64_t value) {gw(s, dword, uint32_t(value)); gw(s, dword + 1, uint32_t(value >> 32));}

// Hardware clock gating of the A610: kgsl adreno-gpulist.h a612_hwcg_regs (mainline a6xx_catalog.c a612_hwcg, the same values),
// offsets from kgsl a6xx_reg.h (checked against mainline a6xx.xml). Without a GMU, kgsl a6xx_hwcg_set writes only these RBBM
// registers and then RBBM_CLOCK_CNTL (A610: 0xAAA8AA82); the GMU's CGC registers and the SPTPRAC clock are not touched.
constexpr uint32_t kHwcg[][2] = {
    {0x00B0, 0x22222222}, {0x00B4, 0x02222220}, {0x00B8, 0x00000081}, {0x00BC, 0x0000F3CF}, {0x00C0, 0x22222222}, {0x00C4, 0x22222222},
    {0x00C8, 0x22222222}, {0x00CC, 0x00022222}, {0x00D0, 0x11111111}, {0x00D4, 0x11111111}, {0x00D8, 0x11111111}, {0x00DC, 0x00011111},
    {0x00E0, 0x77777777}, {0x00E4, 0x77777777}, {0x00E8, 0x77777777}, {0x00EC, 0x00077777}, {0x00F0, 0x22222222}, {0x00F4, 0x01202222},
    {0x00F8, 0x00002220}, {0x0100, 0x00040F00}, {0x0104, 0x05522022}, {0x0105, 0x00005555}, {0x0106, 0x00000011}, {0x0107, 0x00445044},
    {0x0108, 0x04222222}, {0x0111, 0x00002222}, {0x0114, 0x02222222}, {0x0117, 0x00000002}, {0x011B, 0x00002222}, {0x0109, 0x00004000},
    {0x0112, 0x00002222}, {0x0115, 0x00000200}, {0x011C, 0x00000000}, {0x010A, 0x00000000}, {0x0113, 0x00000000}, {0x0116, 0x04104004},
    {0x011D, 0x00000000}, {0x010B, 0x22222222}, {0x0110, 0x00000004}, {0x010F, 0x00000002}, {0x0533, 0x00000182}, {0x0044, 0x00000000},
    {0x0042, 0x00000000}, {0x0118, 0x00000222}, {0x0119, 0x00000111}, {0x011A, 0x00000555},
};
// CP register protection of the A610: mainline a6xx_catalog.c a630_protect (kgsl a630_protected_regs, the same ranges), encoded
// as A6XX_PROTECT_RDONLY (start | (count - 1) << 18) and A6XX_PROTECT_NORDWR (that | 1 << 31); the last entry has an unbounded
// range (CP_PROTECT_CNTL LAST_SPAN_INF_RANGE).
constexpr uint32_t rdonly(uint32_t reg, uint32_t len) {return ((len & 0x3FFF) << 18) | (reg & 0x3FFFF);}
constexpr uint32_t nordwr(uint32_t reg, uint32_t len) {return (1u << 31) | rdonly(reg, len);}
constexpr uint32_t kProtect[32] = {
    rdonly(0x00000, 0x04ff), rdonly(0x00501, 0x0005), rdonly(0x0050b, 0x02f4), nordwr(0x0050e, 0x0000), nordwr(0x00510, 0x0000),
    nordwr(0x00534, 0x0000), nordwr(0x00800, 0x0082), nordwr(0x008a0, 0x0008), nordwr(0x008ab, 0x0024), rdonly(0x008de, 0x00ae),
    nordwr(0x00900, 0x004d), nordwr(0x0098d, 0x0272), nordwr(0x00e00, 0x0001), nordwr(0x00e03, 0x000c), nordwr(0x03c00, 0x00c3),
    rdonly(0x03cc4, 0x1fff), nordwr(0x08630, 0x01cf), nordwr(0x08e00, 0x0000), nordwr(0x08e08, 0x0000), nordwr(0x08e50, 0x001f),
    nordwr(0x09624, 0x01db), nordwr(0x09e70, 0x0001), nordwr(0x09e78, 0x0187), nordwr(0x0a630, 0x01cf), nordwr(0x0ae02, 0x0000),
    nordwr(0x0ae50, 0x032f), nordwr(0x0b604, 0x0000), nordwr(0x0be02, 0x0001), nordwr(0x0be20, 0x17df), nordwr(0x0f000, 0x0bff),
    rdonly(0x0fc00, 0x1fff), nordwr(0x11c00, 0x0000),
};
bool bootArgOn(const char *name) {
    unsigned on = 1;
    PE_parse_boot_argn(name, &on, sizeof on);
    return on != 0;
}

// hw_init of mainline a6xx_gpu.c for the A610 (a610_family, gmu wrapper), with the stock kgsl a6xx_start values where they
// agree; register offsets from kgsl a6xx_reg.h. Hardware clock gating (tb128fu-gpu-hwcg=0 leaves the clocks on) and CP register
// protection (tb128fu-gpu-protect=0 leaves it off) as above. The GMEM window moves to 16 MiB (see kDeviceInfo).
void naHwInit(Session &s) {
    gw(s, 0x3C45, 0); (void)gr(s, 0x3C45);                               // GBIF_HALT
    gw(s, 0x0016, 0); (void)gr(s, 0x0016);                               // RBBM_GBIF_HALT
    gw(s, 0xF803, 0);                                                    // RBBM_SECVID_TSB_CNTL
    gw64(s, 0xF800, 0); gw(s, 0xF802, 0);                                // TSB_TRUSTED_BASE, _SIZE: no trusted range
    static const uint32_t addrMode[] = {0x842, 0xC01, 0x8601, 0x8E05, 0x9E01, 0xBE05, 0xA601, 0x9601, 0xE00, 0xAE01, 0xB601, 0xF810};
    for (uint32_t reg : addrMode) gw(s, reg, 1);                         // *_ADDR_MODE_CNTL: 64-bit
    if (bootArgOn("tb128fu-gpu-hwcg") && gr(s, 0x00AE) != 0xAAA8AA82) {   // a6xx_set_hwcg(true), skipped if already on
        for (const auto &reg : kHwcg) gw(s, reg[0], reg[1]);
        gw(s, 0x00AE, 0xAAA8AA82);                                       // RBBM_CLOCK_CNTL
    }
    for (uint32_t reg = 0x3C03; reg <= 0x3C06; ++reg) gw(s, reg, 0x00071620);   // GBIF_QSB_SIDE0..3 (gmu wrapper CX config)
    gw(s, 0x0011, 0x3);                                                  // RBBM_GBIF_CLIENT_QOS_CNTL
    gw(s, 0x050B, 0xffffffff);                                           // RBBM_PERFCTR_GPU_BUSY_MASKED
    gw64(s, 0xE05, 0x1fffffffff000ull + 0xfc0);                          // UCHE_WRITE_RANGE_MAX
    gw64(s, 0xE09, 0x1fffffffff000ull);                                  // UCHE_TRAP_BASE
    gw64(s, 0xE07, 0x1fffffffff000ull);                                  // UCHE_WRITE_THRU_BASE
    gw64(s, 0xE0B, kDeviceInfo.gmem_base);                               // UCHE_GMEM_RANGE_MIN
    gw64(s, 0xE0D, kDeviceInfo.gmem_base + kDeviceInfo.gmem_bytes - 1);  // UCHE_GMEM_RANGE_MAX
    gw(s, 0xE18, 0x804); gw(s, 0xE17, 0x4);                              // UCHE_FILTER_CNTL, UCHE_CACHE_WAYS
    gw(s, 0x8C2, 0x00800060); gw(s, 0x8C1, 0x40201B16);                  // CP_ROQ_THRESHOLDS_2/_1 (a610 family)
    gw(s, 0x8C3, 48); gw(s, 0x90E, 47);                                  // CP_MEM_POOL_SIZE, CP_MEM_POOL_DBG_ADDR (A610)
    gw(s, 0x9E00, 0x00080000);                                           // PC_DBG_ECO_CNTL: prim_fifo_threshold
    gw(s, 0x98D, 1);                                                     // CP_AHB_CNTL: error response
    gw(s, 0x500, 1);                                                     // RBBM_PERFCTR_CNTL
    gw(s, 0x8D0, 0);                                                     // CP_PERFCTR_CP_SEL(0): PERF_CP_ALWAYS_COUNT
    // a6xx_set_ubwc_config / kgsl: UBWC 1.0 (mode 1), min access length 64 B, highest bank bit 14 (hbb_lo 1, hbb_hi 0).
    gw(s, 0x8E08, 0xb); gw(s, 0xB604, 0xb); gw(s, 0xAE02, 0xb);          // RB, TPL1, SP _NC_MODE_CNTL
    gw(s, 0xE01, (1u << 23) | (1u << 21));                               // UCHE_MODE_CNTL
    gw(s, 0x534, 0);                                                     // RBBM_NC_MODE_CNTL: macrotile mode 0
    gw(s, 0x001F, (1u << 30) | 0x3FFFF);                                 // RBBM_INTERFACE_HANG_INT_CNTL (A610)
    gw(s, 0xE19, (1u << 7) | 1u);                                        // UCHE_CLIENT_PF
    if (bootArgOn("tb128fu-gpu-protect")) {                              // a6xx_set_cp_protect
        gw(s, 0x84F, (1u << 0) | (1u << 1) | (1u << 3));                 // CP_PROTECT_CNTL: enable, fault, last span unbounded
        for (uint32_t i = 0; i < 32; ++i) gw(s, 0x850 + i, kProtect[i]); // CP_PROTECT(i)
    }
    gw(s, 0x038, NativeAdrenoIrq::maskA6xx());                           // RBBM_INT_0_MASK: A6XX_INT_MASK
}

// SQE, hw_init, ring 0, CP_ME_INIT, then a CP_MEM_WRITE into the scratch page that the CPU reads back.
bool naCp(Session &s) {
    uint32_t *n = s.n;
    mark(s, 82);
    n[4] = 1;
    if (NativeAdrenoGpuContextBuffers(s.nb)) {fail(82, -1); return false;}
    for (unsigned i = 0; i < 5; ++i) {
        IODeviceMemory *m = IODeviceMemory::withRange(s.nb[i].physical, s.nb[i].bytes);
        if (m) {
            s.nmap[i] = OSSharedPtr<IOMemoryMap>(m->map(kIOMapAnywhere | kIOMapWriteCombineCache), OSNoRetain);
            m->release();
        }
        if (!s.nmap[i]) {fail(82, -12); return false;}
        s.ncpu[i] = reinterpret_cast<volatile uint8_t *>(s.nmap[i]->getVirtualAddress());
    }
    // The SQE without its first dword (adreno_fw_create_bo).
    n[4] = 2;
    const uint32_t sqeBytes = tb128fu_gpu_sqe_size - 4;
    if (tb128fu_gpu_sqe_size < 8 || sqeBytes > s.nb[0].bytes) {fail(82, -27); return false;}
    for (uint32_t i = 0; i < sqeBytes; ++i) s.ncpu[0][i] = tb128fu_gpu_sqe[4 + i];
    __asm__ volatile("dsb sy" ::: "memory");
    for (uint32_t i = 0; i < sqeBytes && !n[5]; ++i) if (s.ncpu[0][i] != tb128fu_gpu_sqe[4 + i]) n[5] = i + 1;
    if (n[5]) {rows(s); fail(82, -5); return false;}
    n[4] = 3; mark(s, 83);
    naHwInit(s);
    n[54] = gr(s, 0x00AE); n[55] = gr(s, 0x84F);                         // RBBM_CLOCK_CNTL, CP_PROTECT_CNTL as programmed
    gw64(s, 0x830, s.nb[0].iova);                                        // CP_SQE_INSTR_BASE, before CP_SQE_CNTL
    // Ring 0: 32 KiB at its IOVA, the memptrs page for the fence; A6XX forms (NO_UPDATE, CACHE_FLUSH_TS fences).
    n[4] = 4; mark(s, 84);
    gRio = NativeAdrenoRingIo{&s, naWrite, naRead, naBarrier, nullptr, nullptr};   // no GMU, so no AHB fence to check
    gRing = new NativeAdrenoRing;
    gCpInit = new NativeAdrenoCpInit;
    if (!gRing || !gCpInit || gRing->setGeneration(6)) {fail(84, -12); return false;}
    n[6] = uint32_t(gRing->attach(&gRio, &gCache, const_cast<uint8_t *>(s.ncpu[1]), s.nb[1].bytes, s.nb[1].iova,
                                  const_cast<uint8_t *>(s.ncpu[2]), s.nb[2].bytes, s.nb[2].iova));
    if (n[6]) {rows(s); fail(84, int(n[6])); return false;}
    gw(s, 0x808, 1);                                                     // CP_SQE_CNTL: start the microcode
    // a6xx_cp_init: CP_ME_INIT alone (no APRIV, no second thread).
    n[4] = 5; mark(s, 85);
    NativeAdrenoCpInitConfig config{s.nb[0].iova, 0, 1};
    config.generation = 6;
    uint32_t fence = 0;
    n[7] = uint32_t(gCpInit->run(&gRio, gRing, &config, &fence));
    if (n[7]) {rows(s); fail(85, int(n[7])); return false;}
    n[8] = uint32_t(gRing->wait(fence, &gClock, 200000, &n[9]));
    auto cpState = [&] {
        n[12] = gr(s, 0x210); n[13] = gr(s, 0x806); n[14] = gr(s, 0x807); n[15] = gr(s, 0x821); n[16] = gr(s, 0x823);
        n[17] = rd(s.smmu, smmuBank0(s) + 0x58);
        rows(s);
    };
    if (n[8]) {cpState(); fail(85, -110); return false;}
    // The first command through NativeAdreno's mapping: CP_MEM_WRITE (address lo/hi, data) into the scratch page.
    n[4] = 6; mark(s, 86);
    volatile uint32_t *scratch = reinterpret_cast<volatile uint32_t *>(s.ncpu[4]);
    scratch[0] = 0;
    __asm__ volatile("dsb sy" ::: "memory");
    uint32_t words[4] = {};
    if (AdrenoPM4Type7(0x3d, 3, &words[0]) != AdrenoPM4Success) {fail(86, -22); return false;}
    words[1] = uint32_t(s.nb[4].iova); words[2] = uint32_t(s.nb[4].iova >> 32); words[3] = 0xA6100C0D;
    if (gRing->submit(words, 4, &fence)) {fail(86, -5); return false;}
    uint32_t seen = 0;
    n[10] = uint32_t(gRing->wait(fence, &gClock, 200000, &seen));
    n[11] = scratch[0];
    cpState();
    if (n[10] || n[11] != 0xA6100C0D) {fail(86, -110); return false;}
    n[4] = 7;
    rows(s);
    return true;
}

// The zap shader (stage 6's PAS load) and CP_SET_SECURE_MODE(0) through the ring.
bool naSecure(Session &s) {
    if (!zapLoad(s)) {fail(50, int(s.v[59])); return false;}
    mark(s, 87);
    uint32_t words[2] = {}, fence = 0, seen = 0;
    if (AdrenoPM4Type7(0x66, 1, &words[0]) != AdrenoPM4Success || gRing->submit(words, 2, &fence)) {fail(87, -22); return false;}
    s.n[18] = uint32_t(gRing->wait(fence, &gClock, 200000, &seen));
    rows(s);
    if (s.n[18]) {fail(87, -110); return false;}
    return true;
}

// The GPU interrupt line. The secure-mode fence left CP_CACHE_FLUSH_TS set, so a working line fires as soon as it is
// enabled; one more fence (CP_NOP) must fire it again.
bool naIrq(Session &s) {
    uint32_t *n = s.n;
    mark(s, 88);
    uint32_t readback = 0;
    uint64_t router = ~0ull;
    n[19] = uint32_t(armcpu_spi_attach(32 + 177, false, gpuIrq, &s, &readback, &router));
    n[20] = readback;
    gIrqAttached = n[19] == 0;
    IOSleep(20);
    n[21] = gIrqCount;
    uint32_t nop = 0, fence = 0, seen = 0;
    n[23] = AdrenoPM4Type7(0x10, 0, &nop) == AdrenoPM4Success && !gRing->submit(&nop, 1, &fence) &&
            !gRing->wait(fence, &gClock, 200000, &seen);
    IOSleep(20);
    n[22] = gIrqCount;
    n[24] = gIrqSeen;
    n[25] = gr(s, 0x201);
    rows(s);
    return n[19] == 0;
}

// The submission object, the CP always-on counter as the GPU clock, and the published device for user clients.
bool naPublish(Session &s) {
    uint32_t *n = s.n;
    mark(s, 89);
    NativeAdrenoVm *vm = NativeAdrenoGpuContextVm();
    gSubmit = new NativeAdrenoSubmit;
    gTimer = new NativeAdrenoTimer;
    if (!vm || !gSubmit || !gTimer) {fail(89, -12); return false;}
    n[26] = uint32_t(gSubmit->attach(vm, gRing));
    gTimerIo = NativeAdrenoTimeIo{&s, naTimerRead};
    n[27] = uint32_t(gTimer->attach(&gTimerIo, NativeAdrenoTimeCp));
    n[28] = uint32_t(native_adreno_publish(vm, gSubmit, gRing, &gClock, n[27] ? nullptr : gTimer, &kDeviceInfo, 31,
                                           s.report.output_bits));
    rows(s);
    if (n[26] || n[28]) {fail(89, int(n[26] ? n[26] : n[28])); return false;}
    return true;
}

// The user-client self test (sheng GpuPowerProbe.cpp wrapperRound): exactly the path a user-space client takes, minus the Mach
// trap. newUserClient (a second open is refused while one is live), the external-method table, two BOs mapped for the GPU and,
// through BoCpuMap and clientMemoryForType, write-combined for the CPU (the GPU's PTEs are non-cacheable: no cache maintenance),
// an IB of three CP_MEM_WRITEs submitted (the A6XX user prologue of NativeAdrenoRing) and waited for, the target read back, then
// clientClose (drain, unmap, free). n[41..44]: steps passed, steps, first error and its step; n[47]: the first
// clientMemoryForType status. Returns the matching dwords (of 4) or -1.
IOReturn ucCall(IOUserClient *uc, uint32_t method, const void *in, void *out) {
    IOExternalMethodArguments a{};
    a.version = kIOExternalMethodArgumentsCurrentVersion;
    a.selector = method;
    const size_t is = NativeAdrenoUserClient::inputSize(method), os = NativeAdrenoUserClient::outputSize(method);
    a.structureInput = is ? in : nullptr;
    a.structureInputSize = uint32_t(is);
    a.structureOutput = os ? out : nullptr;
    a.structureOutputSize = uint32_t(os);
    return uc->externalMethod(method, &a, nullptr, nullptr, nullptr);
}
int ucRound(IOService *svc, uint32_t *n) {
    auto step = [&](IOReturn status) {
        const uint32_t at = n[42]++;
        if (!status) ++n[41];
        else if (!n[43]) {n[43] = uint32_t(status); n[44] = at;}
        return !status;
    };
    // The published service serves only the Adreno plugin's open type (NativeAdrenoPublish.cpp NativeAdrenoPluginOpenType 0x4147;
    // tb128fu-030 opened with type 0 and was refused with kIOReturnUnsupported). One kernel connection at a time.
    constexpr UInt32 kPluginOpenType = 0x4147;
    IOUserClient *uc = nullptr;
    if (!step(svc->newUserClient(kernel_task, nullptr, kPluginOpenType, nullptr, &uc)) || !uc) return -1;
    IOUserClient *second = nullptr;
    step(svc->newUserClient(kernel_task, nullptr, kPluginOpenType, nullptr, &second) == kIOReturnBusy ? kIOReturnSuccess
                                                                                                      : kIOReturnError);
    if (second) second->release();
    NativeAdrenoAbiParams params{};
    step(ucCall(uc, NativeAdrenoMethodQuery, nullptr, &params));
    NativeAdrenoAbiDeviceInfo info{};
    step(ucCall(uc, NativeAdrenoMethodGetDeviceInfo, nullptr, &info));
    step(info.chip_id == kDeviceInfo.chip_id && info.gmem_bytes == kDeviceInfo.gmem_bytes ? kIOReturnSuccess : kIOReturnError);
    NativeAdrenoAbiBoAlloc alloc{4096, 12, 0};
    NativeAdrenoAbiBoAllocReply ib{}, tgt{};
    int matched = -1;
    IOMemoryDescriptor *desc[2] = {};
    IOMemoryMap *maps[2] = {};
    if (step(ucCall(uc, NativeAdrenoMethodBoAlloc, &alloc, &ib)) && step(ucCall(uc, NativeAdrenoMethodBoAlloc, &alloc, &tgt))) {
        const uint64_t ibIova = 0x20004000, tgtIova = 0x20005000;   // inside the client window of kDeviceInfo
        NativeAdrenoAbiBoMap mapIb{ib.handle, ibIova, 1, 0, 2, 0}, mapTgt{tgt.handle, tgtIova, 3, 0, 2, 0};
        NativeAdrenoAbiBoMapReply r{};
        NativeAdrenoAbiBoCpuMap cmIb{ib.handle}, cmTgt{tgt.handle};
        NativeAdrenoAbiBoCpuMapReply crIb{}, crTgt{};
        if (step(ucCall(uc, NativeAdrenoMethodBoMap, &mapIb, &r)) && step(ucCall(uc, NativeAdrenoMethodBoMap, &mapTgt, &r)) &&
            step(ucCall(uc, NativeAdrenoMethodBoCpuMap, &cmIb, &crIb)) && step(ucCall(uc, NativeAdrenoMethodBoCpuMap, &cmTgt, &crTgt))) {
            IOOptionBits opt[2] = {};
            const uint32_t tokens[2] = {crIb.token, crTgt.token};
            bool mapped = true;
            for (int i = 0; i < 2; ++i) {
                const IOReturn status = uc->clientMemoryForType(tokens[i], &opt[i], &desc[i]);
                if (!n[47]) n[47] = uint32_t(status);
                if (status || !desc[i]) {mapped = false; continue;}
                maps[i] = desc[i]->createMappingInTask(kernel_task, 0, opt[i]);
                if (!maps[i]) {mapped = false; if (!n[47]) n[47] = 0xbad;}
            }
            // Type 0 and a stale token must be refused.
            IOMemoryDescriptor *none = nullptr;
            IOOptionBits o = 0;
            step(uc->clientMemoryForType(0, &o, &none) != kIOReturnSuccess && !none ? kIOReturnSuccess : kIOReturnError);
            step(mapped ? kIOReturnSuccess : kIOReturnNoMemory);
            if (mapped) {
                volatile uint32_t *iw = reinterpret_cast<volatile uint32_t *>(maps[0]->getVirtualAddress());
                volatile uint32_t *tw = reinterpret_cast<volatile uint32_t *>(maps[1]->getVirtualAddress());
                for (unsigned i = 0; i < 1024; ++i) tw[i] = 0xdeadbeef;
                const uint32_t values[3] = {0x0badc0deu, 0xfeed0001u, 0x600d0003u};
                uint32_t words[12];
                bool built = true;
                for (unsigned i = 0; i < 3; ++i) {
                    built = built && AdrenoPM4Type7(0x3d, 3, &words[i * 4]) == AdrenoPM4Success;
                    words[i * 4 + 1] = uint32_t(tgtIova + i * 4); words[i * 4 + 2] = uint32_t(tgtIova >> 32);
                    words[i * 4 + 3] = values[i];
                }
                for (unsigned i = 0; i < 12; ++i) iw[i] = words[i];
                __asm__ volatile("dsb sy" ::: "memory");
                NativeAdrenoAbiSubmit submit{};
                submit.buffer_count = 1;
                submit.buffers[0] = {ib.handle, ibIova, sizeof words};
                NativeAdrenoAbiSubmitReply sr{};
                if (step(built ? kIOReturnSuccess : kIOReturnInternalError) && step(ucCall(uc, NativeAdrenoMethodSubmit, &submit, &sr))) {
                    NativeAdrenoAbiWaitFence wait{sr.fence, 200000000ull};
                    if (step(ucCall(uc, NativeAdrenoMethodWaitFence, &wait, nullptr))) {
                        __asm__ volatile("dsb sy" ::: "memory");
                        matched = (tw[0] == values[0]) + (tw[1] == values[1]) + (tw[2] == values[2]) + (tw[3] == 0xdeadbeef);
                    }
                }
                NativeAdrenoAbiTimestamp ts{};
                step(ucCall(uc, NativeAdrenoMethodGetTimestamp, nullptr, &ts));
            }
        }
    }
    for (auto *m : maps) if (m) m->release();
    for (auto *d : desc) if (d) d->release();
    step(uc->clientClose());
    uc->release();
    return matched;
}
// No imported-memory test here: BoImport pins memory of a user task, and NativeAdreno's host fixture refuses kernel_task imports
// (tests/XnuMemoryFixture.h). A kernel-buffer version of that test panicked tb128fu-033 ("kfree: ... found in kalloc type zone
// 'kalloc.type.var5.16384' but being freed to data"); the import path is exercised by the Metal plugin's own clients instead.
// Two rounds: the second proves the client slot and the IOVAs recycle after clientClose. tb128fu-gpu-selftest=0 skips them.
bool naSelfTest(Session &s) {
    unsigned on = 1;
    PE_parse_boot_argn("tb128fu-gpu-selftest", &on, sizeof on);
    if (!on) return true;
    mark(s, 100);
    IOService *svc = native_adreno_published_service();
    if (!svc) {fail(100, -19); return false;}
    s.n[45] = uint32_t(ucRound(svc, s.n));
    mark(s, 101);
    s.n[46] = uint32_t(ucRound(svc, s.n));
    rows(s);
    if (s.n[45] != 4 || s.n[46] != 4) {fail(101, s.n[43] ? int(s.n[43]) : -5); return false;}
    return true;
}

// Stage 8: the staged AGXAccelerator for the original Metal discovery (sheng publishMetal). The plugin bundle arrives through
// the boot-time addon mount (sheng-gpu-addon=1) and the trust cache; the render server is the one process granted its client.
bool naMetal(Session &s) {
    uint32_t *n = s.n;
    auto stop = [&](IOReturn why) {n[31] = uint32_t(why); rows(s); return false;};
    mark(s, 95);
    n[29] = 1;
    n[30] = uint32_t(OSKextLoadKextWithIdentifier("com.apple.iokit.IOGPUFamily"));
    if (n[30]) {rows(s); return false;}
    const OSSymbol *name = OSSymbol::withCString("IOAcceleratorES");
    const OSMetaClass *parent = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
    OSSafeReleaseNULL(name);
    if (!parent || parent->getClassSize() != 136) return stop(kIOReturnNotFound);
    n[29] = 2;
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) return stop(kIOReturnNoResources);
    native_adreno_accelerator_static_init();
    const IOReturn loaded = OSMetaClass::postModLoad(handle);
    if (loaded) return stop(loaded);
    if (native_adreno_accelerator_instance_size() != 136) return stop(kIOReturnInvalid);
    n[29] = 3;
    IOService *accelerator = native_adreno_accelerator_allocate();
    if (!accelerator) return stop(kIOReturnNoMemory);
    if (!accelerator->init()) {accelerator->release(); return stop(kIOReturnError);}
    // The two keys the original _MTLRegisterDevices helper reads (the plugin name is ours, no Apple device identity is claimed),
    // and the processes whose opens of this accelerator the kernel grants in place of the sandbox verdict
    // (patches/xnu/zzw-native-adreno-sandbox-grant.patch): the render server, and the two that crashed in CoreImage's OpenGL ES path
    // without a Metal device (iconservicesagent, and Settings' Accessibility page through MobileIcons; tb128fu-017/034/042). The GPU
    // addon's NativeAdrenoMetal.clients names the same two (make-gpu-addon.py --clients).
    accelerator->setProperty("MetalPluginName", "NativeAdrenoMetal");
    accelerator->setProperty("NativeSandboxGrantProcess", "backboardd,iconservicesagent,Preferences");
    accelerator->setProperty("MetalPluginClassName", "NativeAdrenoMetalDevice");
    // Turnip's options for this board, which replace the plugin's default "sysmem,nocb" (NativeAdrenoMetalPlugin.mm
    // applyBoardTurnipDebug). Turnip reports Vulkan 1.0 on a6xx gen1 (no hardware multiview) and metal2vulkan needs 1.2: noconform
    // makes it report 1.3. The plugin's default sysmem keeps every pass in system memory, against a hang of the A740's concurrent
    // binning; the A610 has none, so without it Turnip picks tiled GMEM or system memory per pass (tb128fu-048 compares the two:
    // with sysmem a compositor frame took about 10 ms and its scanout copy 6 ms at up to 1114.8 MHz). With each command buffer's GPU
    // time (home screen strokes, 5 or more passes): 068 per-pass choice and 069 forced GMEM about 8 ms per Mpx of pass area (Turnip
    // already chose GMEM: every pipeline ordered its attachment access, which makes a sysmem pass single-primitive); 070 orders only
    // the fetching pipelines (bridge patch 0009), no change; 071 forced system memory: 7.1 ms per Mpx, 11% less, so neither mode is
    // where the time goes. With the surfaces rendered in place (bridge patch 0010), a round of 40 home screen strokes took 47.4 ms of
    // GPU time per swap in system memory (077) and 45.3 ms in GMEM (076), against 64.0 ms before (071), though a compositor pass's
    // second color attachment, cleared and never stored, costs a full-screen clear in system memory alone.
    accelerator->setProperty("NativeTurnipDebug", "nocb,noconform,sysmem");
    // Turnip's printed GPU trace in the render server (NativeAdrenoMetalPlugin.mm applyBoardTurnipDebug): each render pass and command
    // buffer with its GPU time, size, draw count and per-pixel load and store bytes, exported as gputrace.txt (tb128fu-072). 072: half
    // the GPU time lay outside render passes, about 4 ms before a command buffer's first pass and 3.5 ms after its last; copies and
    // blits share the clear_blit toggle (073: textures over surfaces copied in from their buffers before a pass, 3 ms a full-screen
    // one, and back after it, 2.1 ms; 14 MiB of trace overflowed the reader's 32 MiB export copy). Off unless a round asks for it.
    // tb128fu-078 to 082 traced each draw (its fragment shader's hash) in the passes, with the frontend's draw census beside it
    // ("-all,+render_pass,+draw"): some full-screen draws took 25-63 ms where others of the same size took 2 ms.
    static constexpr bool kTurnipTrace = false;
    if (kTurnipTrace)
        accelerator->setProperty("NativeTurnipTrace", "-all,+render_pass,+draw");
    // Capabilities the Metal frontend withholds (NativeAdrenoMetalPlugin.mm). Every one of Core Animation's pipelines blends through
    // framebuffer fetch (tb128fu-079; its full-screen fills took 6 to 21 ms of GPU time), whatever this withholds: programmable blending
    // (080), the Apple1 family (081) and every Apple family (082) alike. Without the Apple families (082) Core Animation no longer
    // composites through its extended-range path (b3a8 targets, 63 ms a draw, and their conversion with a noise dither, 31 ms), which
    // this panel cannot show: home screen strokes took 21.7 against 30.4 ms of GPU time per swap, with the screen unchanged.
    accelerator->setProperty("NativeMetalWithhold", "apple-families");
    n[29] = 4;
    if (!accelerator->attach(IOService::getPlatform())) {accelerator->release(); return stop(kIOReturnNotAttached);}
    accelerator->registerService();
    n[32] = 1;
    n[29] = 5;
    rows(s);
    return true;
}

// Where the CP stands when it stops moving. Fence wait timeouts do not tell: in tb128fu-039 the render server's 2369 were polls
// (all waits together took 0.5 s) and every command buffer completed within 190 ms. Rows 8 and 9 belong to stages 5 and 6 and are
// free from stage 7 on. Once the ring is not drained and RPTR, IB1 base and IB1 remaining dwords are the same as one beat (2 s)
// before, row 8 is refreshed every such beat: seconds, RBBM_STATUS..STATUS3 (0x210..0x213; STATUS3 bit 24
// SMMU stalled on a fault), CP_RB_RPTR/WPTR (0x806/0x807), IB1 and IB2 base and remaining dwords (0x928/0x92a, 0x92b/0x92d),
// CP_HW_FAULT 0x821, CP_INTERRUPT_STATUS 0x823, CP_PROTECT_STATUS 0x824. Row 9: the SQE PC and the header of the packet it is on
// (CP_SQE_STAT_ADDR 0x908 = 0, then CP_SQE_STAT_DATA 0x909 twice, as crashdec reads them), context bank 0 FSR, FAR, FSYNR0, and
// the IB1/IB2 position of the first beat that saw the stall, so two log periods tell a stuck CP from a slow one.
void naHangRows(const Session &s, unsigned t) {
    static unsigned firstT;
    static uint64_t firstIb1, firstIb2;
    static uint32_t firstRem1, firstRem2;
    static bool seen;
    const uint64_t ib1 = gr(s, 0x928) | uint64_t(gr(s, 0x929)) << 32, ib2 = gr(s, 0x92b) | uint64_t(gr(s, 0x92c)) << 32;
    const uint32_t rem1 = gr(s, 0x92a), rem2 = gr(s, 0x92d);
    if (!seen) {seen = true; firstT = t; firstIb1 = ib1; firstIb2 = ib2; firstRem1 = rem1; firstRem2 = rem2;}
    snprintf(tb128fu_gpu_rows[8], sizeof tb128fu_gpu_rows[8],
        "GPU hang t%u st%08x %x/%x/%x r%x/%x | ib1 %llx+%x ib2 %llx+%x | hw%x cpi%x prot%x", t, gr(s, 0x210), gr(s, 0x211),
        gr(s, 0x212), gr(s, 0x213), gr(s, 0x806), gr(s, 0x807), (unsigned long long)ib1, rem1, (unsigned long long)ib2, rem2,
        gr(s, 0x821), gr(s, 0x823), gr(s, 0x824));
    gw(s, 0x908, 0);
    const uint32_t pc = gr(s, 0x909), op = gr(s, 0x909);
    const uint64_t far = rd(s.smmu, kCb0 + kCbFar) | uint64_t(rd(s.smmu, kCb0 + kCbFar + 4)) << 32;
    snprintf(tb128fu_gpu_rows[9], sizeof tb128fu_gpu_rows[9],
        "GPU hang sqe pc%x op%08x | fsr%x far%llx fsynr%x | first t%u ib1 %llx+%x ib2 %llx+%x", pc, op,
        rd(s.smmu, kCb0 + kCbFsr), (unsigned long long)far, rd(s.smmu, kCb0 + 0x68), firstT, (unsigned long long)firstIb1,
        firstRem1, (unsigned long long)firstIb2, firstRem2);
}

// Row 14, refreshed every 2 s once the device is published: interrupts (count, last status, all seen, masked), fence waits
// that slept or spun, submissions (submitted, completed, timeouts), user clients opened/closed, BO pool use.
[[noreturn]] void naHeartbeat(Session &s) {
    uint32_t lastRptr = ~0u, lastIb1 = ~0u, lastRem1 = ~0u;
    for (unsigned t = 0;; t += 2) {
        const NativeAdrenoSubmitStats st = gSubmit->stats();
        const uint32_t rptr = gr(s, 0x806), ib1 = gr(s, 0x928), rem1 = gr(s, 0x92a);
        if (rptr != gr(s, 0x807) && rptr == lastRptr && ib1 == lastIb1 && rem1 == lastRem1) naHangRows(s, t);
        lastRptr = rptr; lastIb1 = ib1; lastRem1 = rem1;
        uint32_t closed = 0;
        const uint32_t opened = native_adreno_publish_stats(&closed);
        snprintf(tb128fu_gpu_rows[14], sizeof tb128fu_gpu_rows[14],
            "GPU na live irq%u last%x seen%x m%u | idle %llu/%llu | sub %u/%u t%u p%u | uc %u/%u | pool %u/%u MiB fb%u sc%u",
            gIrqCount, gIrqLast, gIrqSeen, gIrqMasked, (unsigned long long)native_gpu_idle_sleeps,
            (unsigned long long)native_gpu_idle_spins,
            st.submissions, st.completed, st.timeouts, st.poisoned_jobs, opened, closed, native_adreno_pool_stats[3],
            native_adreno_pool_stats[0], native_adreno_pool_stats[2], native_adreno_pool_stats[5]);
        IOSleep(2000);
    }
}

// MiB/s of a cached copy, 8 MiB eight times: what the DDR vote changes, if the RPM honours it.
unsigned copyRate() {
    constexpr size_t kHalf = 8u << 20;
    uint8_t *buffer = static_cast<uint8_t *>(IOMallocAligned(2 * kHalf, 1u << 14));
    if (!buffer) return 0;
    memset(buffer, 0x5a, 2 * kHalf);
    uint64_t start, end;
    __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(start));
    for (unsigned i = 0; i < 8; ++i) memcpy(buffer + (i & 1 ? 0 : kHalf), buffer + (i & 1 ? kHalf : 0), kHalf);
    __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(end));
    IOFreeAligned(buffer, 2 * kHalf);
    return end > start ? unsigned((uint64_t(8) * kHalf * 19200000ull / (end - start)) >> 20) : 0;
}

// MiB/s of reading 32 MiB twice with eight loads per step, and the CPUs it began and ended on (0-3 Cortex-A53, 4-7 Cortex-A73).
volatile uint64_t gReadSink;
unsigned readRate(unsigned &first, unsigned &last) {
    constexpr size_t kBytes = 32u << 20;
    uint64_t *buffer = static_cast<uint64_t *>(IOMallocAligned(kBytes, 1u << 14));
    if (!buffer) return 0;
    memset(buffer, 0x5a, kBytes);
    first = unsigned(cpu_number());
    uint64_t start, end, sum = 0;
    __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(start));
    for (unsigned pass = 0; pass < 2; ++pass)
        for (size_t i = 0; i < kBytes / 8; i += 8)
            sum += buffer[i] ^ buffer[i + 1] ^ buffer[i + 2] ^ buffer[i + 3] ^ buffer[i + 4] ^ buffer[i + 5] ^ buffer[i + 6] ^ buffer[i + 7];
    __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(end));
    last = unsigned(cpu_number());
    gReadSink = sum;
    IOFreeAligned(buffer, kBytes);
    return end > start ? unsigned((uint64_t(2) * kBytes * 19200000ull / (end - start)) >> 20) : 0;
}

// The memory controller clock in MHz: MCCC (the DT's syscon 0x447d200) + 0x20 holds its period in picoseconds (debugcc-khaje.c
// mc_cc_debug_mux period_offset; clk-debug.c: rate = 10^12 / period). Mapped once, never unmapped.
IOMemoryMap *gMcccMap;
unsigned ddrMhz() {
    if (!gMcccMap) {
        IODeviceMemory *memory = IODeviceMemory::withRange(0x447d000, 0x1000);
        if (!memory) return 0;
        gMcccMap = memory->map(kIOMapAnywhere | kIOMapInhibitCache);
        memory->release();
        if (!gMcccMap) return 0;
    }
    const uint32_t period = *reinterpret_cast<volatile uint32_t *>(gMcccMap->getVirtualAddress() + 0x220);
    return period ? 1000000u / period : 0;
}

// The A610's levels (gpucc-khaje.c, speed bin 235) as PLL0 settings: the VCO runs at twice the gfx3d clock, 19.2 MHz x (L + alpha /
// 65536) with a signed 16-bit alpha. gfx3d needs CX at LOW_SVS, SVS, SVS_PLUS, NOM, TURBO and TURBO_NO_CPR for these; the VCO needs
// MX at LOW_SVS up to 1.8 GHz and SVS up to 2.4 GHz.
struct GpuLevel {uint16_t mhz; uint16_t l; uint16_t alpha;};
constexpr GpuLevel kLevels[] = {{320, 0x21, 0x5555}, {465, 0x30, 0x7000}, {600, 0x3f, 0x8000}, {785, 0x52, 0xc556},
                                {1025, 0x6b, 0xc556}, {1114, 0x74, 0x2000}};
constexpr unsigned kLevelCount = sizeof kLevels / sizeof kLevels[0], kBootLevel = 3;
unsigned gTopLevel;   // the highest level the RPM votes allow; 0 keeps 320 MHz and no governor

// Stage 9: votes through the RPM (RpmGlink.cpp) before the GPU is powered. The votes only go up and cover every level, so the
// governor (dvfsThread) only reprograms PLL0: CX at TURBO_NO_CPR (what 1114.8 MHz needs), MX at TURBO (two levels above what a
// 2.23 GHz VCO needs), the DDR clock at the highest rate, each in the active and sleep sets. Without MX the top is 785 MHz,
// without CX nothing changes.
// Then the RPM's clock scaling switch ("clk0" 2 "Enab"), as Linux's clk-smd-rpm probe does it: every RPM clock of
// qcom,rpmcc-bengal (this tablet's rpmcc) is voted first, a rate one to INT_MAX Hz and a branch on, in both sets, so no bus
// nobody voted for slows down when the switch turns on. While it is off the RPM keeps the boot loader's rates whatever the
// votes say (tb128fu-088: DDR 1555 MHz before and after the BIMC vote, of 200..2092 MHz in the DT's ddr-bw-opp-table).
struct RpmClock {uint32_t type, id, key, value;};
constexpr uint32_t kClk0 = 0x306b6c63, kClk1 = 0x316b6c63, kClk2 = 0x326b6c63, kClka = 0x616b6c63, kCe = 0x6563, kIpa = 0x617069,
                   kQup = 0x707571, kMmxi = 0x69786d6d, kEnab = 0x62616e45, kSwen = 0x6e657773, kKhz = 0x007a484b,
                   kIntMaxKhz = 2147484;   // DIV_ROUND_UP(INT_MAX, 1000)
// clk-smd-rpm.c bengal_clks: bi_tcxo, cnoc, bimc, snoc, qdss, ce1, ipa, qup, mmnrt, mmrt, snoc_periph, snoc_lpass, rf_clk1, rf_clk2.
constexpr RpmClock kRpmClocks[] = {
    {kClk0, 0, kEnab, 1}, {kClk1, 1, kKhz, kIntMaxKhz}, {kClk2, 0, kKhz, kIntMaxKhz}, {kClk1, 2, kKhz, kIntMaxKhz},
    {kClk0, 1, kEnab, 1}, {kCe, 0, kKhz, kIntMaxKhz}, {kIpa, 0, kKhz, kIntMaxKhz}, {kQup, 0, kKhz, kIntMaxKhz},
    {kMmxi, 0, kKhz, kIntMaxKhz}, {kMmxi, 1, kKhz, kIntMaxKhz}, {kClk1, 0, kKhz, kIntMaxKhz}, {kClk1, 5, kKhz, kIntMaxKhz},
    {kClka, 4, kSwen, 1}, {kClka, 5, kSwen, 1}};
constexpr unsigned kRpmClockCount = sizeof kRpmClocks / sizeof kRpmClocks[0];
void dvfsPrepare() {
    constexpr uint32_t kRwcx = 0x78637772, kRwmx = 0x786d7772, kVlvl = 0x6c766c76;
    constexpr uint32_t kCxTurboNoCpr = 0x1a0, kMxTurbo = 0x180, kDdrMax = 0x7fffffff;
    unsigned cpu[4] = {};
    const unsigned ddrBefore = ddrMhz(), readBefore = readRate(cpu[0], cpu[1]);
    const unsigned before = copyRate();
    const int open = tb128fu_rpm_open();
    int cx[3] = {1, 1, 1}, mx[3] = {1, 1, 1}, ddr[2] = {1, 1};   // swen, active, sleep
    int scaling[2] = {1, 1};   // sleep, active
    unsigned clocks = 0;   // RPM clock votes acknowledged, of 2 x kRpmClockCount
    if (!open) {
        cx[0] = tb128fu_rpm_request(0, kRwcx, 0, kSwen, 1);
        cx[1] = tb128fu_rpm_request(0, kRwcx, 0, kVlvl, kCxTurboNoCpr);
        cx[2] = tb128fu_rpm_request(1, kRwcx, 0, kVlvl, kCxTurboNoCpr);
        mx[0] = tb128fu_rpm_request(0, kRwmx, 0, kSwen, 1);
        mx[1] = tb128fu_rpm_request(0, kRwmx, 0, kVlvl, kMxTurbo);
        mx[2] = tb128fu_rpm_request(1, kRwmx, 0, kVlvl, kMxTurbo);
        ddr[0] = tb128fu_rpm_request(0, kClk2, 0, kKhz, kDdrMax);
        ddr[1] = tb128fu_rpm_request(1, kClk2, 0, kKhz, kDdrMax);
        for (const RpmClock &c : kRpmClocks)
            for (uint32_t set = 0; set < 2; ++set) clocks += tb128fu_rpm_request(set, c.type, c.id, c.key, c.value) == 0;
        // Only with every vote in place.
        if (clocks == 2 * kRpmClockCount) {
            scaling[0] = tb128fu_rpm_request(1, kClk0, 2, kEnab, 1);
            scaling[1] = tb128fu_rpm_request(0, kClk0, 2, kEnab, 1);
        }
        IOSleep(20);
    }
    const unsigned after = copyRate();
    const unsigned ddrAfter = ddrMhz(), readAfter = readRate(cpu[2], cpu[3]);
    snprintf(tb128fu_ddr_rows[0], sizeof tb128fu_ddr_rows[0],
        "DDR %u -> %u MHz | clocks %u/%u scaling %d %d | read %u -> %u copy %u -> %u MiB/s | cpu %u-%u %u-%u", ddrBefore,
        ddrAfter, clocks, 2 * kRpmClockCount, scaling[0], scaling[1], readBefore, readAfter, before, after, cpu[0], cpu[1],
        cpu[2], cpu[3]);
    const bool cxOk = !open && !cx[0] && !cx[1] && !cx[2], mxOk = cxOk && !mx[0] && !mx[1] && !mx[2];
    gTopLevel = mxOk ? kLevelCount - 1 : cxOk ? kBootLevel : 0;
    if (gTopLevel) {gPllL = kLevels[kBootLevel].l; gPllAlpha = kLevels[kBootLevel].alpha;}
    snprintf(tb128fu_gpu_dvfs[0], sizeof tb128fu_gpu_dvfs[0],
        "GPU dvfs open %d | cx %d %d %d mx %d %d %d ddr %d %d | copy %u -> %u MiB/s | boot %u MHz top %u MHz", open, cx[0], cx[1],
        cx[2], mx[0], mx[1], mx[2], ddr[0], ddr[1], before, after, unsigned(kLevels[gTopLevel ? kBootLevel : 0].mhz),
        unsigned(kLevels[gTopLevel].mhz));
}

// Moves the running PLL0 to a level as clk_zonda_pll_set_rate does (gpucc-khaje.c / clk-alpha-pll.c): ALPHA, then L, 5 us, the lock
// bit (FREQ_LOCK_DET, bit 29, when TEST_CTL bit 16 STAY_IN_CFA is set, else LOCK_DET, bit 31), 100 us; then the gfx3d RCG re-latches
// its unchanged CFG (0x101: PLL0 OUT_MAIN, divide by 1) with CMD UPDATE, as clk_rcg2 does after the parent's rate change.
bool setLevel(const Session &s, unsigned level) {
    const Region &c = s.gpucc;
    wr(c, kPllAlpha, kLevels[level].alpha);
    wr(c, kPllL, kLevels[level].l);
    IODelay(5);
    const uint32_t lock = (rd(c, kPllTest) & (1u << 16)) ? (1u << 29) : (1u << 31);
    const bool locked = poll(c, kPllMode, lock, lock, 200);
    IODelay(100);
    set(c, kGfxCmd, 1);
    return poll(c, kGfxCmd, 1, 0, 500) && locked;
}

// The governor: RBBM_STATUS sampled every millisecond, busy when any unit but the CX AHB master is (kgsl a6xx_hw_isidle); per
// 50 samples, 35 busy or more jumps to the top level, fewer than 8 for ten windows in a row steps one level down. Row 1 every
// 20 windows: the level, switches and failed ones, the newest window's load, ms at each level, STAY_IN_CFA.
void dvfsThread(void *arg, wait_result_t) {
    const Session &s = *static_cast<const Session *>(arg);
    unsigned level = kBootLevel, quiet = 0, switches = 0, failed = 0, ddrLow = ~0u, ddrHigh = 0, ddrSamples = 0;
    uint32_t levelMs[kLevelCount] = {};
    for (unsigned window = 0;; ++window) {
        const unsigned ddr = ddrMhz();
        if (ddr < ddrLow) ddrLow = ddr;
        if (ddr > ddrHigh) ddrHigh = ddr;
        ++ddrSamples;
        if (window % 20 == 0) {
            snprintf(tb128fu_ddr_rows[1], sizeof tb128fu_ddr_rows[1], "DDR now %u MHz | low %u high %u of %u samples, 50 ms apart",
                ddr, ddrLow, ddrHigh, ddrSamples);
            ddrLow = ~0u; ddrHigh = 0; ddrSamples = 0;
        }
        unsigned busy = 0;
        for (unsigned i = 0; i < 50; ++i) {busy += (gr(s, 0x210) & 0x00fffffeu) != 0; IOSleep(1);}
        unsigned target = level;
        if (busy >= 35) {target = gTopLevel; quiet = 0;}
        else if (busy < 8) {if (++quiet >= 10 && level) {target = level - 1; quiet = 0;}}
        else quiet = 0;
        if (target != level) {
            if (setLevel(s, target)) {level = target; ++switches;} else ++failed;
        }
        levelMs[level] += 50;
        if (window % 20 == 0)
            snprintf(tb128fu_gpu_dvfs[1], sizeof tb128fu_gpu_dvfs[1],
                "GPU dvfs now %u MHz | sw %u fail %u | busy %u%% | ms 320:%u 465:%u 600:%u 785:%u 1025:%u 1114:%u | cfa%u",
                unsigned(kLevels[level].mhz), switches, failed, busy * 2, levelMs[0], levelMs[1], levelMs[2], levelMs[3], levelMs[4],
                levelMs[5],
                (rd(s.gpucc, kPllTest) >> 16) & 1);
    }
}
void startDvfs(Session &s) {
    thread_t thread = nullptr;
    if (kernel_thread_start(dvfsThread, &s, &thread) == KERN_SUCCESS) thread_deallocate(thread);
}
}  // namespace

static void gpuThread(void *, wait_result_t) {
    Session *s = static_cast<Session *>(IOMallocType(Session));
    if (!s) {fail(0, -12); return;}
    bzero(s, sizeof *s);
    unsigned stage = tb128fu_gpu_stage_req;
    do {
        if (!snapshot(*s)) break;
        tb128fu_gpu_stage_done = 1;
        rows(*s);
        if (stage < 2) break;
        if (stage >= 9) dvfsPrepare();
        if (!powerOn(*s)) break;
        tb128fu_gpu_stage_done = 2;
        rows(*s);
        if (stage < 3) break;
        if (!smmuView(*s)) break;
        tb128fu_gpu_stage_done = 3;
        rows(*s);
        if (stage < 4) break;
        if (!tzQueries(*s)) {fail(40, -5); break;}
        tb128fu_gpu_stage_done = 4;
        rows(*s);
        if (stage < 5) break;
        if (stage >= 7) {
            // NativeAdreno owns the GPU's SMMU context and ring from here: stages 5 and 6 (the hand-built bank) stay out.
            if (!naContext(*s) || !naCp(*s) || !naSecure(*s) || !naIrq(*s) || !naPublish(*s)) break;
            const bool tested = naSelfTest(*s);
            if (tested) tb128fu_gpu_stage_done = 7;
            rows(*s);
            if (tested && stage >= 8 && naMetal(*s)) tb128fu_gpu_stage_done = 8;
            rows(*s);
            if (stage >= 9 && gTopLevel && tb128fu_gpu_stage_done >= 7) startDvfs(*s);
            naHeartbeat(*s);
        }
        if (!firstCommand(*s)) break;
        tb128fu_gpu_stage_done = 5;
        rows(*s);
        if (stage < 6) break;
        if (!leaveSecure(*s)) {if (!tb128fu_gpu_error) fail(50, int(s->v[59])); break;}
        tb128fu_gpu_stage_done = 6;
    } while (0);
    rows(*s);
}

extern "C" void tb128fu_gpu_probe(void) {
    unsigned stage = 0;
    if (!PE_parse_boot_argn("tb128fu-gpu", &stage, sizeof stage) || stage < 1 || tb128fu_gpu_stage_req) return;
    if (stage > 9) stage = 9;
    tb128fu_gpu_stage_req = stage;
    thread_t thread = nullptr;
    if (kernel_thread_start(gpuThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    else tb128fu_gpu_error = -12;
}
