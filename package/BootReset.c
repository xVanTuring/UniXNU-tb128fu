// SPDX-License-Identifier: MIT
// Board glue for BootResetCore.h: PSCI feature probe, a read-only look at the PMIC arbiter and the PON's reboot-reason byte, and the guarded
// "restart into fastboot" (boot argument sheng-autoreset=<seconds>, see BootLog.c). Nothing here runs without BootLog's boot arguments.
// Adapted from the sheng board's BootReset.c for the TB128FU's version 5 arbiter and PON.
#include <kern/thread.h>
#include <machine/machine_routines.h>
#include <string.h>
#include <pexpert/pexpert.h>
#include "BootResetCore.h"

// Physical windows from the TB128FU devicetree (spmi@1c40000 reg, reg-names core/chnls/obsrvr/intr/cnfg).
enum {SpmiCore = 0x1c40000, SpmiCoreSize = 0x2000, SpmiChnls = 0x1e00000, SpmiObsrvr = 0x3e00000, SpmiCnfg = 0x1c0a000, SpmiCnfgSize = 0x1000,
      SpmiPage = 0x1000};
enum {PsciVersion = 0x84000000u, PsciFeatures = 0x8400000au, PsciSystemReset = 0x84000009u, PsciSystemReset2_32 = 0x84000012u,
      PsciSystemReset2_64 = 0xc4000012u};

static uint64_t psci(uint64_t fid, uint64_t a1) {
  register uint64_t x0 __asm__("x0") = fid, x1 __asm__("x1") = a1, x2 __asm__("x2") = 0, x3 __asm__("x3") = 0;
  __asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) :: "memory", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13",
                   "x14", "x15", "x16", "x17");
  return x0;
}

// ml_io_map hands out permanent mappings (io_submap, KMA_PERMANENT) that ml_io_unmap cannot free: its kmem_free on kernel_map panics with the
// map lock held (int-105 hung launchd that way). Mappings that are unmapped again must be unmappable ones (flags 7 = VM_WIMG_IO).
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, uint32_t flags);
static volatile uint32_t *map(uint64_t phys, uint64_t size) {return (volatile uint32_t *)ml_io_map_unmappable((vm_offset_t)phys, (vm_size_t)size, 7);}

struct reset_maps {
  struct br_arbiter a;
  struct br_found f;
  volatile uint32_t *obs_page, *chn_page;   // the mapped pages
  volatile uint32_t *obs, *chn;             // this APID's channels inside them
  int find;   // br_find's result, -1 before it ran
};
static void unmap_all(struct reset_maps *m) {
  if (m->a.core) ml_io_unmap((vm_offset_t)m->a.core, SpmiCoreSize);
  if (m->a.cnfg) ml_io_unmap((vm_offset_t)m->a.cnfg, SpmiCnfgSize);
  if (m->obs_page) ml_io_unmap((vm_offset_t)m->obs_page, SpmiPage);
  if (m->chn_page) ml_io_unmap((vm_offset_t)m->chn_page, SpmiPage);
  memset(m, 0, sizeof *m);
}
// Maps the arbiter, finds the PON's APID and maps its observer (and, when asked, read/write) channel page. 0 = ready.
static int open_maps(struct reset_maps *m, int want_write) {
  memset(m, 0, sizeof *m);
  m->find = -1;
  m->a.ee = 0;   // qcom,ee = <0>
  m->a.core = map(SpmiCore, SpmiCoreSize);
  m->a.cnfg = map(SpmiCnfg, SpmiCnfgSize);
  if (!m->a.core || !m->a.cnfg) return 1;
  m->find = br_find(&m->a, &m->f);
  if (m->find) return 2;
  // Only this APID's pages: the read/write channels are 64 KiB apart, the arbiter's whole channel window 32 MiB.
  const uint64_t obs = (uint64_t)BR_OBS_EE_STRIDE * m->a.ee + (uint64_t)BR_OBS_APID_STRIDE * (uint64_t)m->f.apid;
  m->obs_page = map((uint64_t)SpmiObsrvr + (obs & ~(uint64_t)(SpmiPage - 1)), SpmiPage);
  if (!m->obs_page) return 3;
  m->obs = m->obs_page + (obs & (SpmiPage - 1)) / 4;
  if (want_write) {
    m->chn_page = map((uint64_t)SpmiChnls + (uint64_t)BR_CHNL_APID_STRIDE * (uint64_t)m->f.apid, SpmiPage);
    if (!m->chn_page) return 4;
    m->chn = m->chn_page;
  }
  return 0;
}

// The arbiter and the PON's reboot-reason byte, read only. Fills text; returns 0 when the byte can be written (version known, APID found and
// owned by this EE, the peripheral a gen2 PON).
int boot_reset_probe(char *text, unsigned cap) {
  const uint64_t ver = psci(PsciVersion, 0);
  const int64_t f_reset = (int64_t)psci(PsciFeatures, PsciSystemReset), f2_32 = (int64_t)psci(PsciFeatures, PsciSystemReset2_32),
                f2_64 = (int64_t)psci(PsciFeatures, PsciSystemReset2_64);
  struct reset_maps m;
  uint8_t type = 0xff, subtype = 0xff, cell = 0xff;
  int open_rc = open_maps(&m, 0), pon_rc = -1, read_rc = -1;
  if (!open_rc) pon_rc = br_check_pon(m.obs, &type, &subtype);
  if (!open_rc) read_rc = br_read8(m.obs, BR_REBOOT_CELL, &cell);
  const int writable = !open_rc && !pon_rc && !read_rc && m.f.owner == m.a.ee;
  snprintf(text, cap, "psci v%x reset=%lld reset2_32=%lld reset2_64=%lld | spmi v%08x apids=%u open=%d find=%d apid=%d owner=%d "
           "pon=%02x/%02x(%d) spare8f=%02x read=%d ok=%d",
           (unsigned)ver, (long long)f_reset, (long long)f2_32, (long long)f2_64, m.a.core ? m.f.version : 0, m.a.core ? m.f.apids : 0, open_rc,
           m.find, m.find ? -1 : m.f.apid, m.find ? -1 : m.f.owner, type, subtype, pon_rc, cell, read_rc, writable);
  unmap_all(&m);
  return writable ? 0 : 1;
}

// Writes "bootloader" into the reboot-reason byte, verifies it, then PSCI SYSTEM_RESET. Returns only when something refused (text says what);
// the byte is left as it was in every refusal before the write.
int boot_reset_bootloader(char *text, unsigned cap) {
  struct reset_maps m;
  uint8_t before = 0xff, after = 0xff, type = 0xff, subtype = 0xff;
  int rc = open_maps(&m, 1);
  if (rc) {
    snprintf(text, cap, "reset refused: open=%d find=%d", rc, m.find);
    unmap_all(&m);
    return rc;
  }
  if (m.f.owner != m.a.ee) {
    snprintf(text, cap, "reset refused: apid %d is owned by EE %d", m.f.apid, m.f.owner);
    unmap_all(&m);
    return 5;
  }
  rc = br_check_pon(m.obs, &type, &subtype);
  if (rc) {
    snprintf(text, cap, "reset refused: peripheral %02x/%02x is not a gen2 PON (rc=%d)", type, subtype, rc);
    unmap_all(&m);
    return 6;
  }
  rc = br_set_bootloader(m.obs, m.chn, &before, &after);
  snprintf(text, cap, "reset: spare8f %02x -> %02x rc=%d", before, after, rc);
  unmap_all(&m);
  if (rc) return 10 + rc;
  psci(PsciSystemReset, 0);
  return 99;   // SYSTEM_RESET returned
}
