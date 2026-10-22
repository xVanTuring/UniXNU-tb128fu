// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host side of the Himax HX83102E touch controller (TDDI, zero flash: the host loads the firmware into the chip's SRAM after
// every reset) over SPI. Sequences follow the Himax common driver (hxchipset) that the stock TB128FU kernel builds, version
// 2.0.0.81: himax_platform.c (bus framing), himax_ic_incell_core.c (AHB access, CRC, himax_zf_part_info) and himax_ic_HX83102.c
// (hx83102_chip_detect, hx83102e_sense_off/on, hx83102e_reload_to_active, hx83102e_read_event_stack); the constants were checked
// against the stock kernel image and its boot log. No code copied. Pure C over the callbacks below; TouchProbe.cpp owns the
// hardware.
#ifndef HIMAX_PROTOCOL_H
#define HIMAX_PROTOCOL_H
#include <stdint.h>
#include <stddef.h>

#define HX_CHUNK 4096                 // bytes per AHB write (the stock driver sends up to 128 KiB at once; any split works)
#define HX_XFER_MAX (HX_CHUNK + 8)    // F2 00 a0..a3 + chunk, F3 cmd 00 + read
#define HX_EVENT_LEN 68               // HX_MAX_PT 10: 10 x (X, Y) + 10 widths + 2 + point count + 3 + 12 stylus (touch_info_size)
#define HX_MAX_PT 10
#define HX_X_MAX 1199                 // panel-coords = <0 1199 0 1999> (DTS), firmware X/Y resolution 1200 x 2000
#define HX_Y_MAX 1999
#define HX_PARTS_MAX 16
#define HX_CFG_MAX 0x8000             // DSRAM_SIZE: the merged config image must fit

enum {
    HX_OK = 0, HX_ERR_BUS = -5, HX_ERR_PARAM = -22, HX_ERR_SAFE = -60, HX_ERR_CHIP = -61, HX_ERR_FW = -62, HX_ERR_CRC = -63,
    HX_ERR_HANDSHAKE = -64, HX_ERR_START = -65, HX_ERR_OVERLAY = -66, HX_ERR_RELOAD = -67, HX_ERR_FRAME = -68,
};

struct hx_bus {
    void *ctx;
    // One chip-select assertion of len bytes; rx NULL = write only. 0 on success.
    int (*xfer)(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len);
    void (*reset)(void *ctx, int high);       // TP_RST (GPIO 86) level
    void (*delay_us)(void *ctx, unsigned us); // busy wait
    void (*sleep_ms)(void *ctx, unsigned ms); // may sleep
};

struct hx_part {uint32_t sram, size, offset; int overlay;};
struct hx_fw {
    const uint8_t *bin;
    size_t size;
    uint32_t table;                           // the partition table (descriptor map code TP_CONFIG_TABLE)
    unsigned count, overlay;                  // partitions; index of the alg overlay section
    struct hx_part part[HX_PARTS_MAX];
    uint32_t cfg_base, cfg_size, cfg_crc;     // merged config image: SRAM base, size, expected CRC
};

struct hx_chip {
    struct hx_bus *bus;
    uint8_t *tx, *rx;                         // HX_XFER_MAX bytes each
    // Evidence for the diagnostic rows.
    uint8_t id[4];                            // 0x900000D0 as read: [3] [2] [1] = 83 10 2E
    uint32_t state;                           // 0x900000A8 central state at the last check (04 idle, 05 running, 0C safe mode)
    unsigned safe_tries, stage;
    int rc;
    uint32_t crc_main, crc_cfg, crc_ovl, handshake, reload, done, fw_ver, panel_info;
};

// The firmware's own CRC (himax_mcu_Calculate_CRC_with_AP): CRC-32C over little-endian words, initial 0xFFFFFFFF, no final XOR.
uint32_t hx_crc_sw(const uint8_t *p, size_t len);
// Parses the descriptor and partition tables and merges the config sections into cfg (zero filled, cfg_cap bytes).
int hx_fw_parse(struct hx_fw *fw, const uint8_t *bin, size_t size, uint8_t *cfg, size_t cfg_cap);

int hx_bus_read(struct hx_chip *c, uint8_t cmd, uint8_t *buf, size_t len);
int hx_bus_write(struct hx_chip *c, uint8_t cmd, const uint8_t *data, size_t len);
int hx_reg_read(struct hx_chip *c, uint32_t addr, uint8_t *buf, size_t len);
int hx_reg_write(struct hx_chip *c, uint32_t addr, const uint8_t *data, size_t len);
int hx_reg_read32(struct hx_chip *c, uint32_t addr, uint32_t *value);
int hx_reg_write32(struct hx_chip *c, uint32_t addr, uint32_t value);

// Pin reset, safe mode, chip id (hx83102_chip_detect). HX_OK if the id is HX83102E.
int hx_identify(struct hx_chip *c);
// System reset, safe mode, main code, config, start, alg overlay (himax_mcu_firmware_update_0f). c->stage says how far it got.
int hx_download(struct hx_chip *c, const struct hx_fw *fw, const uint8_t *cfg);

struct hx_point {int slot, x, y, w;};
// The event stack (bus command 0x30), HX_EVENT_LEN bytes.
int hx_read_event(struct hx_chip *c, uint8_t *frame);
// The fingers of an event frame: count (0 = none down), or HX_ERR_FRAME for a frame that fails the checksum or is all zero.
int hx_parse_event(const uint8_t *frame, struct hx_point *pts, unsigned max);
#endif
