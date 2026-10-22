// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of HimaxProtocol.c: the firmware tables of the tablet's own Himax_firmware.bin (values from the stock boot log) and
// the identify / download / event sequences against a simulated HX83102E that interprets the SPI framing.
// cc -std=c11 -Wall -Wextra -Werror -I package package/HimaxProtocolTests.c package/HimaxProtocol.c -o /tmp/hx && /tmp/hx FW.bin
#include "HimaxProtocol.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond) do {if (!(cond)) {fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++failures;}} while (0)

// The simulated chip: AHB memory for the code SRAM (0x20000000) and the data SRAM (0x10000000), a few registers, the CRC engine,
// and the firmware start handshake.
struct sim {
    uint8_t code[0x10000 + 0x4000], data[0x10000];
    uint32_t addr, state, isr, reload, crc_from, crc_result, fw_started, overlay_requested, reads_since_start;
    uint8_t conti, incr, psw_lb, psw_ub, rst;
    unsigned resets, transfers, bad;
    uint8_t event[HX_EVENT_LEN];
};

static uint8_t *mem(struct sim *s, uint32_t a) {
    if (a >= 0x20000000u && a < 0x20000000u + sizeof s->code) return s->code + (a - 0x20000000u);
    if (a >= 0x10000000u && a < 0x10000000u + sizeof s->data) return s->data + (a - 0x10000000u);
    return NULL;
}
static uint32_t get32(const uint8_t *p) {return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;}
static void put32(uint8_t *p, uint32_t v) {p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);}

static void reg_write(struct sim *s, uint32_t a, uint32_t v) {
    uint8_t *m = mem(s, a);
    if (m) {
        put32(m, v);
        if (a == 0x10007FFCu && v == 0x22222222u && s->overlay_requested) put32(mem(s, 0x100072C0u), 0x000072C0u);
        return;
    }
    switch (a) {
    case 0x9000005Cu: s->isr = v; if (v == 0xA5 && s->state == 0x05) s->isr = 0x87; break;
    case 0x90000018u: if (v == 0x55) {s->state = 0x04; s->psw_lb = s->psw_ub = 0;} break;
    case 0x90000048u: if (v == 0xEC) {s->reload = 0x01EC; s->fw_started = 1; s->state = 0x05;} break;
    case 0x80050020u: s->crc_from = v; break;
    case 0x80050028u: {
        uint32_t words = v & 0xffff;
        assert((v >> 16) == 0x99);
        s->crc_result = hx_crc_sw(mem(s, s->crc_from), words * 4);
        break;
    }
    default: break;
    }
}
static uint32_t reg_read(struct sim *s, uint32_t a) {
    uint8_t *m = mem(s, a);
    if (m) {
        // The started firmware asks for its overlay a little later.
        if (a == 0x10007FFCu && s->fw_started && get32(m) == 0xA55A5AA5u && ++s->reads_since_start > 2) {
            put32(m, 0x11111111u);
            s->overlay_requested = 1;
        }
        return get32(m);
    }
    switch (a) {
    case 0x900000A8u: return (s->psw_lb == 0x27 && s->psw_ub == 0x95 && !s->fw_started) ? 0x0C : s->state;
    case 0x900000D0u: return 0x83102E00u;
    case 0x9000005Cu: return s->isr;
    case 0x90000048u: return s->reload;
    case 0x80050000u: return 0;
    case 0x80050018u: return s->crc_result;
    default: return 0;
    }
}

static int sim_xfer(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len) {
    struct sim *s = ctx;
    ++s->transfers;
    if (!s->rst || len < 2) {++s->bad; return -1;}
    if (tx[0] == 0xF2) {
        assert(!rx);
        uint8_t cmd = tx[1];
        const uint8_t *d = tx + 2;
        size_t n = len - 2;
        if (cmd == 0x13) s->conti = d[0];
        else if (cmd == 0x0D) s->incr = d[0];
        else if (cmd == 0x31) s->psw_lb = d[0];
        else if (cmd == 0x32) s->psw_ub = d[0];
        else if (cmd == 0x0C) assert(d[0] == 0);
        else if (cmd == 0x00) {
            assert(n >= 4);
            s->addr = get32(d);
            if (n > 4) {
                assert(s->conti == 0x31);
                assert(n == 8 || (s->incr & 1));          // more than one word needs auto-increment
                for (size_t i = 4; i + 4 <= n; i += 4) reg_write(s, s->addr + (uint32_t)(i - 4), get32(d + i));
            }
        } else ++s->bad;
        return 0;
    }
    if (tx[0] == 0xF3) {
        assert(rx && len >= 3 && tx[2] == 0);
        memset(rx, 0x5a, 3);                              // the three header bytes are discarded by the host
        uint8_t *out = rx + 3;
        size_t n = len - 3;
        if (tx[1] == 0x08) for (size_t i = 0; i < n; i += 4) {uint8_t w[4]; put32(w, reg_read(s, s->addr + (uint32_t)i)); memcpy(out + i, w, n - i < 4 ? n - i : 4);}
        else if (tx[1] == 0x13) out[0] = s->conti;
        else if (tx[1] == 0x0D) out[0] = s->incr;
        else if (tx[1] == 0x30) memcpy(out, s->event, n < HX_EVENT_LEN ? n : HX_EVENT_LEN);
        else memset(out, 0, n);
        return 0;
    }
    ++s->bad;
    return -1;
}
static void sim_reset(void *ctx, int high) {struct sim *s = ctx; if (!high) ++s->resets; s->rst = (uint8_t)high;}
static void sim_delay(void *ctx, unsigned us) {(void)ctx; (void)us;}

static void make_event(uint8_t *e, int x0, int y0, int x3, int y3) {
    memset(e, 0xff, 40);
    memset(e + 40, 0, HX_EVENT_LEN - 40);
    e[0] = (uint8_t)(x0 >> 8); e[1] = (uint8_t)x0; e[2] = (uint8_t)(y0 >> 8); e[3] = (uint8_t)y0;
    e[12] = (uint8_t)(x3 >> 8); e[13] = (uint8_t)x3; e[14] = (uint8_t)(y3 >> 8); e[15] = (uint8_t)y3;
    e[40] = 7; e[43] = 9; e[52] = 2;
    unsigned sum = 0;
    for (unsigned i = 0; i < HX_EVENT_LEN - 1; ++i) sum += e[i];
    e[55] = (uint8_t)(0x100 - ((sum - e[55]) & 0xff));
}

int main(int argc, char **argv) {
    if (argc < 2) {fprintf(stderr, "usage: %s Himax_firmware.bin\n", argv[0]); return 2;}
    FILE *f = fopen(argv[1], "rb");
    if (!f) {perror(argv[1]); return 2;}
    static uint8_t bin[0x40000], cfg[HX_CFG_MAX];
    size_t size = fread(bin, 1, sizeof bin, f);
    fclose(f);

    struct hx_fw fw;
    CHECK(hx_fw_parse(&fw, bin, size, cfg, sizeof cfg) == HX_OK);
    // Stock log: CONFIG_TABLE in 10400; 12 parts; cfg_sz = 17632, dsram_base = 1000472C; overlay section 11 at 2000B780, 12288
    // bytes; config CRC D2 69 F1 85.
    CHECK(fw.table == 0x10400);
    CHECK(fw.count == 12);
    CHECK(fw.part[0].sram == 0x20000000u && fw.part[0].size == 0x10000 && fw.part[0].offset == 0x400);
    CHECK(fw.overlay == 11 && fw.part[11].sram == 0x2000B780u && fw.part[11].size == 12288 && fw.part[11].offset == 0x1A000);
    CHECK(fw.cfg_base == 0x1000472Cu && fw.cfg_size == 17632);
    CHECK(fw.cfg_crc == 0xD269F185u);
    CHECK(hx_crc_sw(bin + 0x400, 0x10000) == 0);
    CHECK(hx_crc_sw(bin + 0x1A000, 12288) == 0);

    static struct sim s;
    memset(&s, 0, sizeof s);
    s.state = 0x04; s.rst = 1;
    struct hx_bus bus = {&s, sim_xfer, sim_reset, sim_delay, sim_delay};
    static uint8_t tx[HX_XFER_MAX], rx[HX_XFER_MAX];
    struct hx_chip c;
    memset(&c, 0, sizeof c);
    c.bus = &bus; c.tx = tx; c.rx = rx;

    CHECK(hx_identify(&c) == HX_OK);
    CHECK(c.id[3] == 0x83 && c.id[2] == 0x10 && c.id[1] == 0x2E);
    CHECK((c.state & 0xff) == 0x0C);

    int r = hx_download(&c, &fw, cfg);
    if (r) fprintf(stderr, "download stage %u rc %d crc %08x/%08x/%08x hs %08x\n", c.stage, r, c.crc_main, c.crc_cfg, c.crc_ovl, c.handshake);
    CHECK(r == HX_OK);
    CHECK(c.stage == 22 && c.crc_main == 0 && c.crc_cfg == 0xD269F185u && c.crc_ovl == 0);
    CHECK(c.done == 0x000072C0u && (c.reload & 0xffff) == 0x01EC && (c.state & 0xff) == 0x05);
    // The alg overlay (0x2000B780..0x2000E780) replaces part of the main code once the firmware asks for it.
    CHECK(memcmp(s.code, bin + 0x400, 0xB780) == 0);
    CHECK(memcmp(s.code + 0xB780, bin + 0x1A000, 12288) == 0);
    CHECK(memcmp(s.code + 0xE780, bin + 0x400 + 0xE780, 0x10000 - 0xE780) == 0);
    {
        // The config image covers the start-up words written after it; apply those writes to the expected image.
        static uint8_t want[HX_CFG_MAX];
        memcpy(want, cfg, fw.cfg_size);
        static const uint32_t later[][2] = {{0x10007F00u, 0}, {0x100072ECu, 0}, {0x10007F04u, 0}, {0x10007294u, 1},
                                            {0x100072C0u, 0x000072C0u}, {0x10007FFCu, 0x22222222u}};
        for (unsigned i = 0; i < sizeof later / sizeof later[0]; ++i) put32(want + (later[i][0] - fw.cfg_base), later[i][1]);
        CHECK(memcmp(s.data + 0x472C, want, fw.cfg_size) == 0);
    }
    CHECK(s.bad == 0);

    uint8_t frame[HX_EVENT_LEN];
    struct hx_point pts[HX_MAX_PT];
    make_event(s.event, 1138, 1186, 10, 1999);
    CHECK(hx_read_event(&c, frame) == HX_OK);
    CHECK(hx_parse_event(frame, pts, HX_MAX_PT) == 2);
    CHECK(pts[0].slot == 0 && pts[0].x == 1138 && pts[0].y == 1186 && pts[0].w == 7);
    CHECK(pts[1].slot == 3 && pts[1].x == 10 && pts[1].y == 1999 && pts[1].w == 9);
    frame[5] ^= 1;
    CHECK(hx_parse_event(frame, pts, HX_MAX_PT) == HX_ERR_FRAME);
    memset(frame, 0, sizeof frame);
    CHECK(hx_parse_event(frame, pts, HX_MAX_PT) == HX_ERR_FRAME);
    memset(frame, 0xff, 52); memset(frame + 52, 0, 16); frame[52] = 0xFF;
    {unsigned sum = 0; for (unsigned i = 0; i < HX_EVENT_LEN; ++i) sum += frame[i]; frame[55] = (uint8_t)(0x100 - (sum & 0xff));}
    CHECK(hx_parse_event(frame, pts, HX_MAX_PT) == 0);

    printf("%s: %u transfers, %u resets, %d failures\n", failures ? "FAIL" : "PASS", s.transfers, s.resets, failures);
    return failures != 0;
}
