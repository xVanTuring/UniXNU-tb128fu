// SPDX-License-Identifier: MIT
// Reboot-into-fastboot control for the TB128FU (SM6225): the PM6125 PON peripheral's SOFT_RB_SPARE byte (what the stock kernel's
// qpnp-power-on writes for `reboot bootloader`, and the stock bootloader reads as the reboot reason) written through the SPMI PMIC arbiter,
// then a PSCI SYSTEM_RESET. Adapted from the sheng board's core (an SDAM cell behind a version 7 arbiter). The TB128FU devicetree's arbiter
// (spmi@1c40000: core 0x1c40000+0x1100, chnls 0x1e00000, obsrvr 0x3e00000, cnfg 0x1c0a000, EE 0) is version 5, laid out as the mainline
// spmi-pmic-arb driver documents it: APID map at core + 0x900, ownership at cnfg + 0x700, observer channels 0x10000 per EE and 0x80 per APID,
// read/write channels 0x10000 per APID. The byte has the same encoding as sheng's cell: bits 7:1, bootloader = 2 (a gen2 PON).
// The core is pure register-pointer code so a host test can drive it against a software model of the arbiter.
#ifndef BOOT_RESET_CORE_H
#define BOOT_RESET_CORE_H
#include <stdint.h>

enum {
  BR_VERSION_V5_MIN = 0x50000000u, BR_VERSION_V7_MIN = 0x70000000u,
  BR_FEATURES = 0x04, BR_FEATURES_APIDS_MASK = 0x7ffu,
  BR_MAX_APIDS = 512,                       // the map ends at core 0x900 + 4 * 512 = 0x1100, the devicetree's core size
  BR_APID_MAP = 0x900,                      // core + 0x900 + 4*apid: bits 19:8 PPID, bit 24 IRQ owner
  BR_CNFG_OWNER = 0x700,                    // cnfg + 0x700 + 4*apid: bits 2:0 the EE allowed to write the APID
  BR_OBS_EE_STRIDE = 0x10000, BR_OBS_APID_STRIDE = 0x80,  // observer (read) channels: 0x10000*EE + 0x80*APID
  BR_CHNL_APID_STRIDE = 0x10000,            // read/write channels: 0x10000*APID
  BR_CMD = 0x00, BR_STATUS = 0x08, BR_WDATA0 = 0x10, BR_RDATA0 = 0x18,
  BR_OP_EXT_WRITEL = 0, BR_OP_EXT_READL = 1,
  BR_STATUS_DONE = 1u << 0, BR_STATUS_ERRORS = 0xeu,      // bits 1..3: failed, denied, dropped
  BR_PON_PPID = 0x008,                      // PMIC SID 0, peripheral 0x08: the PM6125 PON at 0x800
  BR_PON_TYPE = 0x04, BR_PON_SUBTYPE = 0x05, BR_PERIPH_TYPE_PON = 0x01,
  BR_PON_GEN2_PRIMARY = 0x04, BR_PON_GEN2_SECONDARY = 0x05,
  BR_REBOOT_CELL = 0x8f,                    // SOFT_RB_SPARE
  BR_REBOOT_MASK = 0xfe, BR_REBOOT_SHIFT = 1, BR_REBOOT_BOOTLOADER = 2,
  BR_POLLS = 100000
};
struct br_arbiter {
  volatile uint32_t *core;   // the APID map: BR_APID_MAP + 4 * BR_MAX_APIDS bytes
  volatile uint32_t *cnfg;   // the ownership table: BR_CNFG_OWNER + 4 * BR_MAX_APIDS bytes
  uint8_t ee;
};
struct br_found {uint32_t version, apids; int apid, owner;};

static inline uint32_t br_cmd(uint8_t op, uint8_t byte_addr, uint8_t bc) {return ((uint32_t)op << 27) | ((uint32_t)byte_addr << 4) | (bc & 7u);}

// Version, APID count and the APID that serves the PON peripheral for this EE. 0 on success; every refusal has its own code.
static inline int br_find(const struct br_arbiter *a, struct br_found *f) {
  f->version = a->core[0]; f->apids = a->core[BR_FEATURES / 4] & BR_FEATURES_APIDS_MASK; f->apid = -1; f->owner = -1;
  if (f->version < BR_VERSION_V5_MIN || f->version >= BR_VERSION_V7_MIN) return 1;   // not the layout this code knows
  if (!f->apids || f->apids > BR_MAX_APIDS) return 2;
  for (uint32_t i = 0; i < f->apids; ++i) {
    uint32_t map = a->core[(BR_APID_MAP + 4 * i) / 4];
    if (!map || ((map >> 8) & 0xfff) != BR_PON_PPID) continue;
    int owner = (int)(a->cnfg[(BR_CNFG_OWNER + 4 * i) / 4] & 7u);
    // As the mainline driver picks it: the first mapping, replaced by one this EE owns.
    if (f->apid < 0 || owner == a->ee) {f->apid = (int)i; f->owner = owner;}
  }
  return f->apid < 0 ? 3 : 0;
}
static inline int br_wait(volatile uint32_t *ch) {
  for (unsigned n = 0; n < BR_POLLS; ++n) {
    uint32_t s = ch[BR_STATUS / 4];
    if (s & BR_STATUS_DONE) return (s & BR_STATUS_ERRORS) ? 2 : 0;
  }
  return 1;
}
// One byte of the peripheral. obs is this EE's observer channel of the APID, chn its read/write channel.
static inline int br_read8(volatile uint32_t *obs, uint8_t byte_addr, uint8_t *value) {
  obs[BR_CMD / 4] = br_cmd(BR_OP_EXT_READL, byte_addr, 0);
  int rc = br_wait(obs);
  if (rc) return rc;
  *value = (uint8_t)obs[BR_RDATA0 / 4];
  return 0;
}
static inline int br_write8(volatile uint32_t *chn, uint8_t byte_addr, uint8_t value) {
  chn[BR_WDATA0 / 4] = value;
  chn[BR_CMD / 4] = br_cmd(BR_OP_EXT_WRITEL, byte_addr, 0);
  return br_wait(chn);
}
// The peripheral must be a gen2 PON: only then is SOFT_RB_SPARE the reboot reason with this encoding. 0 = it is.
static inline int br_check_pon(volatile uint32_t *obs, uint8_t *type, uint8_t *subtype) {
  int rc = br_read8(obs, BR_PON_TYPE, type);
  if (!rc) rc = br_read8(obs, BR_PON_SUBTYPE, subtype);
  if (rc) return 10 + rc;
  if (*type != BR_PERIPH_TYPE_PON || (*subtype != BR_PON_GEN2_PRIMARY && *subtype != BR_PON_GEN2_SECONDARY)) return 20;
  return 0;
}
// Sets the reboot reason to "bootloader", leaving bit 0, and reads it back. 0 = written and verified.
static inline int br_set_bootloader(volatile uint32_t *obs, volatile uint32_t *chn, uint8_t *before, uint8_t *after) {
  int rc = br_read8(obs, BR_REBOOT_CELL, before);
  if (rc) return 10 + rc;
  uint8_t next = (uint8_t)((*before & ~BR_REBOOT_MASK) | (BR_REBOOT_BOOTLOADER << BR_REBOOT_SHIFT));
  rc = br_write8(chn, BR_REBOOT_CELL, next);
  if (rc) return 20 + rc;
  rc = br_read8(obs, BR_REBOOT_CELL, after);
  if (rc) return 30 + rc;
  return *after == next ? 0 : 40;
}
#endif
