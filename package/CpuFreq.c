// SPDX-License-Identifier: MIT
// The SM6225 CPU frequency hardware (qcom,cpufreq-hw in the TB128FU devicetree): freq-domain0 at 0xf521000 runs CPUs 0-3
// (Cortex-A53), freq-domain1 at 0xf523000 CPUs 4-7 (Cortex-A73). Register facts as Linux's qcom-cpufreq-hw driver uses them for
// this generation: 0x000 enable, 0x110 + 32*i the frequency lookup table (bits 31:30 the source, 0 = the 300 MHz alternate
// clock; bits 7:0 the multiple of the 19.2 MHz XO), 0x920 the requested performance level (an index into the table). Under
// Android the Cortex-A53s ran up to 1900.8 MHz and the Cortex-A73s up to 2400 MHz; nothing in UniXNU sets them, so they run at
// the level the boot loader left.
// boot_cpufreq_report() describes both domains for the UFS log (BootLog.c). tb128fu-cpufreq=1 additionally asks each domain,
// once it is enabled (bit 0 of the enable word; the Cortex-A73 domain read 2 while only the Cortex-A53s were started), for its
// highest table level; the hardware's own limits management (LMh) still caps it.
#include <kern/thread.h>
#include <machine/machine_routines.h>
#include <string.h>
#include <pexpert/pexpert.h>

enum {DomainSize = 0x1000, RegEnable = 0x000, RegLut = 0x110, LutStride = 32, LutEntries = 12, RegPerfState = 0x920};
static const uint64_t domain_phys[2] = {0xf521000, 0xf523000};
static volatile uint32_t *domains[2];
static int request_parsed, requested[2];
static uint32_t want;

// Appends piece to text and returns the new length, never past cap - 1.
static unsigned append(char *text, unsigned cap, unsigned at, const char *piece) {
  while (*piece && at + 1 < cap) text[at++] = *piece++;
  text[at] = 0;
  return at;
}

// The table's levels in MHz; returns how many are valid (a repeated last entry ends it, as in the Linux driver).
static unsigned levels(volatile uint32_t *r, unsigned mhz[LutEntries]) {
  unsigned count = 0;
  for (unsigned i = 0; i < LutEntries; ++i) {
    uint32_t entry = r[(RegLut + LutStride * i) / 4];
    unsigned source = (entry >> 30) & 3, multiple = entry & 0xff;
    if (source && !multiple) break;
    unsigned value = source ? multiple * 192 / 10 : 300;
    if (count && value == mhz[count - 1]) break;
    mhz[count++] = value;
  }
  return count;
}

int boot_cpufreq_report(char *text, unsigned cap) {
  if (!cap) return -1;
  unsigned at = 0, mhz[LutEntries];
  char piece[96];
  text[0] = 0;
  if (!request_parsed) {PE_parse_boot_argn("tb128fu-cpufreq", &want, sizeof want); request_parsed = 1;}
  for (int d = 0; d < 2; ++d) {
    if (!domains[d]) domains[d] = (volatile uint32_t *)ml_io_map((vm_offset_t)domain_phys[d], DomainSize);
    volatile uint32_t *r = domains[d];
    if (!r) {snprintf(piece, sizeof piece, "d%d unmapped | ", d); at = append(text, cap, at, piece); continue;}
    unsigned count = levels(r, mhz);
    uint32_t enable = r[RegEnable / 4];
    if (want == 1 && !requested[d] && count && (enable & 1)) {
      r[RegPerfState / 4] = count - 1;
      __asm__ volatile("dsb sy" ::: "memory");
      requested[d] = 1;
    }
    uint32_t perf = r[RegPerfState / 4];
    snprintf(piece, sizeof piece, "d%d en=%x perf=%u (%u MHz)%s table", d, enable, perf, perf < count ? mhz[perf] : 0,
             requested[d] ? " requested top" : "");
    at = append(text, cap, at, piece);
    for (unsigned i = 0; i < count; ++i) {snprintf(piece, sizeof piece, " %u", mhz[i]); at = append(text, cap, at, piece);}
    at = append(text, cap, at, " | ");
  }
  append(text, cap, at, want == 1 ? "tb128fu-cpufreq=1" : "read only");
  return 0;
}
