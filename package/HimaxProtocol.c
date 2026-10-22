// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "HimaxProtocol.h"
#include <string.h>

// AHB registers of the HX83102E (himax_ic_core.h / himax_ic_HX83102.c as built in the stock kernel).
enum {
    HX_REG_FW_ISR = 0x9000005C,           // addr_ctrl_fw_isr: 0xA5 stop request, 0x87 acknowledged, 0 = run
    HX_REG_CENTRAL_STATE = 0x900000A8,    // addr_cs_central_state
    HX_REG_ICID = 0x900000D0,
    HX_REG_SYSTEM_RESET = 0x90000018,     // 0x55
    HX_REG_TCON_RST = 0x80020020,
    HX_REG_ADC_RST = 0x80020094,
    HX_REG_RELOAD_TO_ACTIVE = 0x90000048, // 0xEC, reads back 0x01EC
    HX_REG_CRC_FROM = 0x80050020,         // addr_reload_addr_from
    HX_REG_CRC_CMD = 0x80050028,          // addr_reload_addr_cmd_beat: 0x0099 << 16 | words
    HX_REG_CRC_STATUS = 0x80050000,       // bit 0 busy
    HX_REG_CRC_RESULT = 0x80050018,
    HX_REG_HANDSHAKE = 0x10007FFC,        // A55A5AA5 cleared, 11111111 overlay requested, 22222222 overlay sent
    HX_REG_RELOAD_DISABLE = 0x10007F00,
    HX_REG_RAW_OUT_SEL = 0x100072EC,
    HX_REG_SORTING_MODE = 0x10007F04,
    HX_REG_SET_FRAME = 0x10007294,
    HX_REG_2ND_RELOAD = 0x100072C0,       // 0 before start, 0x000072C0 when the firmware has reloaded
    HX_REG_FW_VER = 0x10007004,
    HX_REG_PANEL_INFO = 0x100070F8,       // [0] max points, [1] flags
};
enum {HX_CMD_AHB_ADDR = 0x00, HX_CMD_AHB_RDATA = 0x08, HX_CMD_AHB_DIR = 0x0C, HX_CMD_INCR4 = 0x0D, HX_CMD_CONTI = 0x13,
      HX_CMD_PSW_LB = 0x31, HX_CMD_PSW_UB = 0x32, HX_CMD_EVENT = 0x30};
enum {HX_STATE_RUNNING = 0x05, HX_STATE_SAFE = 0x0C};
enum {HX_MAP_TP_CONFIG_TABLE = 0x00000A00, HX_FW_PAGE = 128, HX_MAIN_SIZE = 0x10000};

static void put32(uint8_t *p, uint32_t v) {p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);}
static uint32_t get32(const uint8_t *p) {return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;}
static void sleep_ms(struct hx_chip *c, unsigned ms) {c->bus->sleep_ms(c->bus->ctx, ms);}

uint32_t hx_crc_sw(const uint8_t *p, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i + 4 <= len; i += 4) {
        crc ^= get32(p + i);
        for (int j = 0; j < 32; ++j) crc = (crc & 1) ? (crc >> 1) ^ 0x82F63B78u : crc >> 1;
    }
    return crc;
}

int hx_fw_parse(struct hx_fw *fw, const uint8_t *bin, size_t size, uint8_t *cfg, size_t cfg_cap) {
    memset(fw, 0, sizeof *fw);
    fw->bin = bin; fw->size = size;
    if (!bin || size <= HX_MAIN_SIZE || size < 16) return HX_ERR_FW;
    // Descriptor table (hx_mcu_bin_desc_get): pages of 16-byte entries from offset 0, the first page marked by eight zero bytes
    // and 0x87 at byte 14; each entry sums to 0 mod 256; an all-zero entry ends it.
    if (get32(bin) || get32(bin + 4) || bin[14] != 0x87) return HX_ERR_FW;
    int end = 0;
    for (size_t page = 0; !end && page + HX_FW_PAGE <= size; page += HX_FW_PAGE) {
        for (size_t i = page; i < page + HX_FW_PAGE; i += 16) {
            unsigned any = 0, sum = 0;
            for (size_t j = i; j < i + 16; ++j) {any |= bin[j]; sum += bin[j];}
            if (!any) {end = 1; break;}
            if (sum & 0xff) continue;
            if (get32(bin + i) == HX_MAP_TP_CONFIG_TABLE) fw->table = get32(bin + i + 4);
        }
    }
    if (!fw->table || fw->table + 16 > size) return HX_ERR_FW;
    // Partition table (himax_zf_part_info): count at byte 12; per entry the SRAM address, size and offset in the file; 0x88 0x77 at
    // bytes 14/15 marks the alg overlay section. Entry 0 is the main code; the others (but the overlay) form the config image.
    unsigned count = bin[fw->table + 12];
    if (count < 2 || count > HX_PARTS_MAX || fw->table + 16 * count > size) return HX_ERR_FW;
    uint32_t base = 0xFFFFFFFFu, top = 0, top_size = 0;
    for (unsigned i = 0; i < count; ++i) {
        const uint8_t *e = bin + fw->table + 16 * i;
        struct hx_part *p = &fw->part[i];
        p->sram = get32(e); p->size = get32(e + 4); p->offset = get32(e + 8);
        p->overlay = i && e[14] == 0x88 && e[15] == 0x77;
        if (p->offset > size || p->size > size - p->offset) return HX_ERR_FW;
        if (p->overlay) {fw->overlay = i; continue;}
        if (!i) continue;
        p->sram -= p->sram % 4;
        if (p->sram < base) base = p->sram;
        if (p->sram > top) {top = p->sram; top_size = p->size;}
    }
    fw->count = count;
    if (fw->part[0].size != HX_MAIN_SIZE || !fw->overlay || base == 0xFFFFFFFFu) return HX_ERR_FW;
    uint32_t cfg_size = (top - base) + top_size;
    cfg_size = (cfg_size + 15) & ~15u;
    if (cfg_size > HX_CFG_MAX || cfg_size > cfg_cap) return HX_ERR_FW;
    memset(cfg, 0, cfg_size);
    for (unsigned i = 1; i < count; ++i) {
        const struct hx_part *p = &fw->part[i];
        if (p->overlay) continue;
        if (p->sram - base + p->size > cfg_size) return HX_ERR_FW;
        memcpy(cfg + (p->sram - base), bin + p->offset, p->size);
    }
    fw->cfg_base = base; fw->cfg_size = cfg_size; fw->cfg_crc = hx_crc_sw(cfg, cfg_size);
    return HX_OK;
}

// Bus framing (himax_platform.c): a write is F2 cmd payload; a read is one full-duplex transfer of F3 cmd 00 plus len bytes,
// whose first three received bytes are discarded.
int hx_bus_write(struct hx_chip *c, uint8_t cmd, const uint8_t *data, size_t len) {
    if (len + 2 > HX_XFER_MAX) return HX_ERR_PARAM;
    c->tx[0] = 0xF2; c->tx[1] = cmd;
    if (len) memcpy(c->tx + 2, data, len);
    return c->bus->xfer(c->bus->ctx, c->tx, NULL, len + 2) ? HX_ERR_BUS : HX_OK;
}

int hx_bus_read(struct hx_chip *c, uint8_t cmd, uint8_t *buf, size_t len) {
    if (len + 3 > HX_XFER_MAX) return HX_ERR_PARAM;
    memset(c->tx, 0, len + 3);
    c->tx[0] = 0xF3; c->tx[1] = cmd;
    if (c->bus->xfer(c->bus->ctx, c->tx, c->rx, len + 3)) return HX_ERR_BUS;
    memcpy(buf, c->rx + 3, len);
    return HX_OK;
}

static int write1(struct hx_chip *c, uint8_t cmd, uint8_t value) {return hx_bus_write(c, cmd, &value, 1);}

// himax_mcu_burst_enable: continuous mode, and 4-byte address auto-increment for transfers longer than one word.
static int burst(struct hx_chip *c, int increment) {
    int r = write1(c, HX_CMD_CONTI, 0x31);
    return r ? r : write1(c, HX_CMD_INCR4, (uint8_t)(0x10 | (increment ? 1 : 0)));
}

int hx_reg_write(struct hx_chip *c, uint32_t addr, const uint8_t *data, size_t len) {
    for (size_t at = 0; at < len;) {
        size_t n = len - at < HX_CHUNK ? len - at : HX_CHUNK;
        int r = burst(c, n > 4);
        if (r) return r;
        c->tx[0] = 0xF2; c->tx[1] = HX_CMD_AHB_ADDR;
        put32(c->tx + 2, addr + (uint32_t)at);
        memcpy(c->tx + 6, data + at, n);
        if (c->bus->xfer(c->bus->ctx, c->tx, NULL, n + 6)) return HX_ERR_BUS;
        at += n;
        if (at < len) c->bus->delay_us(c->bus->ctx, 100);   // himax_mcu_write_sram_0f between chunks
    }
    return HX_OK;
}

int hx_reg_read(struct hx_chip *c, uint32_t addr, uint8_t *buf, size_t len) {
    uint8_t a[4];
    put32(a, addr);
    int r = burst(c, len > 4);
    if (!r) r = hx_bus_write(c, HX_CMD_AHB_ADDR, a, 4);
    if (!r) r = write1(c, HX_CMD_AHB_DIR, 0x00);
    if (!r) r = hx_bus_read(c, HX_CMD_AHB_RDATA, buf, len);
    if (!r && len > 4) r = burst(c, 0);
    return r;
}

int hx_reg_read32(struct hx_chip *c, uint32_t addr, uint32_t *value) {
    uint8_t b[4];
    int r = hx_reg_read(c, addr, b, 4);
    *value = r ? 0 : get32(b);
    return r;
}

int hx_reg_write32(struct hx_chip *c, uint32_t addr, uint32_t value) {
    uint8_t b[4];
    put32(b, value);
    return hx_reg_write(c, addr, b, 4);
}

static void pin_reset(struct hx_chip *c, unsigned low_ms, unsigned high_ms) {
    c->bus->reset(c->bus->ctx, 0);
    sleep_ms(c, low_ms);
    c->bus->reset(c->bus->ctx, 1);
    sleep_ms(c, high_ms);
}

// Stop the firmware and enter safe mode (hx83102_sense_off for detection, hx83102e_sense_off before a download): the stop
// request, then the password until the central state reads 0x0C, then the TCON reset (detection also releases TCON and ADC).
static int sense_off(struct hx_chip *c, int detect) {
    int r = hx_reg_write32(c, HX_REG_FW_ISR, 0xA5);
    if (r) return r;
    sleep_ms(c, detect ? 20 : 10);
    if ((r = hx_reg_read32(c, HX_REG_CENTRAL_STATE, &c->state))) return r;
    if ((c->state & 0xff) == HX_STATE_RUNNING) {
        uint32_t isr = 0;
        for (unsigned i = 0; i < 35 && (isr & 0xff) != 0x87; ++i) {
            if ((r = hx_reg_read32(c, HX_REG_FW_ISR, &isr))) return r;
            if ((isr & 0xff) != 0x87) sleep_ms(c, 10);
        }
    }
    for (c->safe_tries = 0; c->safe_tries < 6; ++c->safe_tries) {
        if ((r = write1(c, HX_CMD_PSW_LB, 0x27)) || (r = write1(c, HX_CMD_PSW_UB, 0x95))) return r;
        if ((r = hx_reg_read32(c, HX_REG_CENTRAL_STATE, &c->state))) return r;
        if ((c->state & 0xff) == HX_STATE_SAFE) {
            if ((r = hx_reg_write32(c, HX_REG_TCON_RST, 0))) return r;
            c->bus->delay_us(c->bus->ctx, 1000);
            if (detect) {
                if ((r = hx_reg_write32(c, HX_REG_TCON_RST, 1)) || (r = hx_reg_write32(c, HX_REG_ADC_RST, 0))) return r;
                c->bus->delay_us(c->bus->ctx, 1000);
                if ((r = hx_reg_write32(c, HX_REG_ADC_RST, 1))) return r;
            }
            return HX_OK;
        }
        sleep_ms(c, 5);
        if (detect) pin_reset(c, 5, 5); else pin_reset(c, 20, 50);
    }
    return HX_ERR_SAFE;
}

int hx_identify(struct hx_chip *c) {
    uint8_t dummy;
    c->stage = 1;
    pin_reset(c, 5, 5);
    int r = hx_bus_read(c, HX_CMD_CONTI, &dummy, 1);
    if (r) return c->rc = r;
    c->stage = 2;
    if ((r = sense_off(c, 1))) return c->rc = r;
    c->stage = 3;
    for (unsigned i = 0; i < 5; ++i) {
        if ((r = hx_reg_read(c, HX_REG_ICID, c->id, 4))) return c->rc = r;
        if (c->id[3] == 0x83 && c->id[2] == 0x10 && c->id[1] == 0x2E) return c->rc = HX_OK;
        sleep_ms(c, 10);
    }
    return c->rc = HX_ERR_CHIP;
}

// The chip's CRC engine over SRAM (himax_mcu_check_CRC); 0 for a section that carries its own CRC.
static int hw_crc(struct hx_chip *c, uint32_t addr, uint32_t len, uint32_t *crc) {
    int r = hx_reg_write32(c, HX_REG_CRC_FROM, addr);
    if (!r) r = hx_reg_write32(c, HX_REG_CRC_CMD, 0x00990000u | ((len / 4) & 0xffff));
    for (unsigned i = 0; !r && i < 100; ++i) {
        uint32_t status;
        if ((r = hx_reg_read32(c, HX_REG_CRC_STATUS, &status))) break;
        if (!(status & 1)) return hx_reg_read32(c, HX_REG_CRC_RESULT, crc);
        c->bus->delay_us(c->bus->ctx, 1000);
    }
    return r ? r : HX_ERR_CRC;
}

static int write_checked(struct hx_chip *c, uint32_t addr, const uint8_t *data, uint32_t len, uint32_t want, uint32_t *got) {
    int r = HX_ERR_CRC;
    for (unsigned i = 0; i < 3; ++i) {
        if ((r = hx_reg_write(c, addr, data, len))) continue;
        if ((r = hw_crc(c, addr, len, got))) continue;
        if (*got == want) return HX_OK;
        r = HX_ERR_CRC;
    }
    return r;
}

// Write a handshake word and read it back (up to five times).
static int handshake(struct hx_chip *c, uint32_t value) {
    int r = HX_ERR_HANDSHAKE;
    for (unsigned i = 0; i < 5; ++i) {
        if ((r = hx_reg_write32(c, HX_REG_HANDSHAKE, value))) continue;
        c->bus->delay_us(c->bus->ctx, 1000);
        if ((r = hx_reg_read32(c, HX_REG_HANDSHAKE, &c->handshake))) continue;
        if (c->handshake == value) return HX_OK;
        r = HX_ERR_HANDSHAKE;
    }
    return r;
}

// hx83102e_sense_on(0): interface on, clear the stop request, pin reset, then reload to active until 0x90000048 reads 0x01EC.
static int sense_on(struct hx_chip *c) {
    uint8_t b[4];
    int r = hx_bus_read(c, HX_CMD_AHB_RDATA, b, 4);
    for (unsigned i = 0; !r && i < 10; ++i) {
        uint8_t conti = 0, incr = 0;
        if ((r = burst(c, 0)) || (r = hx_bus_read(c, HX_CMD_CONTI, &conti, 1)) || (r = hx_bus_read(c, HX_CMD_INCR4, &incr, 1))) break;
        if (conti == 0x31 && incr == 0x10) break;
        c->bus->delay_us(c->bus->ctx, 1000);
    }
    if (!r) r = hx_reg_write32(c, HX_REG_FW_ISR, 0);
    if (r) return r;
    sleep_ms(c, 10);
    pin_reset(c, 5, 5);
    for (unsigned i = 0; i < 5; ++i) {
        if ((r = hx_reg_write32(c, HX_REG_RELOAD_TO_ACTIVE, 0xEC))) return r;
        c->bus->delay_us(c->bus->ctx, 1000);
        if ((r = hx_reg_read32(c, HX_REG_RELOAD_TO_ACTIVE, &c->reload))) return r;
        if ((c->reload & 0xffff) == 0x01EC) return HX_OK;
    }
    return HX_ERR_START;
}

int hx_download(struct hx_chip *c, const struct hx_fw *fw, const uint8_t *cfg) {
    int r;
    const struct hx_part *code = &fw->part[0], *ovl = &fw->part[fw->overlay];
#define STEP(n, expr) do {c->stage = (n); if ((r = (expr))) return c->rc = r;} while (0)
    STEP(10, hx_reg_write32(c, HX_REG_SYSTEM_RESET, 0x55));
    STEP(11, sense_off(c, 0));
    STEP(12, write_checked(c, code->sram, fw->bin + code->offset, code->size, 0, &c->crc_main));
    STEP(13, write_checked(c, fw->cfg_base, cfg, fw->cfg_size, fw->cfg_crc, &c->crc_cfg));
    STEP(14, handshake(c, 0xA55A5AA5u));
    STEP(15, hx_reg_write32(c, HX_REG_RELOAD_DISABLE, 0));
    STEP(15, hx_reg_write32(c, HX_REG_RAW_OUT_SEL, 0));
    STEP(15, hx_reg_write32(c, HX_REG_SORTING_MODE, 0));
    STEP(15, hx_reg_write32(c, HX_REG_SET_FRAME, 1));
    STEP(15, hx_reg_write32(c, HX_REG_2ND_RELOAD, 0));
    STEP(16, sense_on(c));
    // The firmware asks for its alg overlay section by writing 0x11111111 to the handshake word.
    c->stage = 17;
    for (unsigned i = 0; ; ++i) {
        c->bus->delay_us(c->bus->ctx, 3000);
        if ((r = hx_reg_read32(c, HX_REG_HANDSHAKE, &c->handshake))) return c->rc = r;
        if (c->handshake == 0x11111111u) break;
        if (i >= 30) return c->rc = HX_ERR_OVERLAY;
    }
    STEP(18, write_checked(c, ovl->sram, fw->bin + ovl->offset, ovl->size, 0, &c->crc_ovl));
    STEP(19, handshake(c, 0x22222222u));
    c->stage = 20;
    for (unsigned i = 0; ; ++i) {
        if ((r = hx_reg_read32(c, HX_REG_2ND_RELOAD, &c->done))) return c->rc = r;
        if (c->done == 0x000072C0u) break;
        if (i >= 30) return c->rc = HX_ERR_RELOAD;
        sleep_ms(c, 10);
    }
    STEP(21, hx_reg_read32(c, HX_REG_CENTRAL_STATE, &c->state));
    STEP(21, hx_reg_read32(c, HX_REG_FW_VER, &c->fw_ver));
    STEP(21, hx_reg_read32(c, HX_REG_PANEL_INFO, &c->panel_info));
#undef STEP
    c->stage = 22;
    return c->rc = HX_OK;
}

int hx_read_event(struct hx_chip *c, uint8_t *frame) {return hx_bus_read(c, HX_CMD_EVENT, frame, HX_EVENT_LEN);}

int hx_parse_event(const uint8_t *frame, struct hx_point *pts, unsigned max) {
    unsigned sum = 0, zeros = 0;
    for (unsigned i = 0; i < HX_EVENT_LEN; ++i) {sum += frame[i]; zeros += !frame[i];}
    if ((sum & 0xff) || zeros == HX_EVENT_LEN) return HX_ERR_FRAME;
    if (frame[52] == 0xFF) return 0;      // no finger down
    unsigned n = 0;
    for (unsigned i = 0; i < HX_MAX_PT && n < max; ++i) {
        int x = frame[4 * i] << 8 | frame[4 * i + 1], y = frame[4 * i + 2] << 8 | frame[4 * i + 3];
        if (x > HX_X_MAX || y > HX_Y_MAX) continue;   // released or unused slot (0xFFFF)
        pts[n].slot = (int)i; pts[n].x = x; pts[n].y = y; pts[n].w = frame[40 + i];
        ++n;
    }
    return (int)n;
}
