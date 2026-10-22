// SPDX-License-Identifier: BSD-2-Clause-Patent
// TB128FU (SM6225 "khaje") touch controller board facts and the pure helpers around them, after UniXNU-sheng's TouchBoard.h
// (SM8550). Every constant states its source: DTS = the tablet's device tree (android-devinfo dump), DS = the msm-4.19 kernel for
// this tablet (github.com/Roynas-Android-Playground/kernel_lenovo_tb128fu: gcc-khaje.c, pinctrl-khaje.c, hxchipset). Nothing here
// touches hardware; TouchProbe.cpp owns the MMIO.
#ifndef TB128FU_TOUCH_BOARD_H
#define TB128FU_TOUCH_BOARD_H
#include <stdint.h>

// DTS qupv3_se0_spi: spi@4a80000 size 0x4000, SE0 of the only QUP wrapper qcom,qupv3_0_geni_se@4ac0000 (size 0x2000).
#define TB_QUP_WRAPPER_BASE 0x04ac0000ULL
#define TB_SE_BASE 0x04a80000ULL
#define TB_SE_SIZE 0x4000ULL
// DTS: the wrapper's iommus = <&apps_smmu 0xe3 0>; apps-smmu@c600000 size 0x80000 (only needed if the engine has no FIFO).
#define TB_QUP_SMMU_SID 0xe3
#define TB_APPS_SMMU_BASE 0x0c600000ULL
#define TB_APPS_SMMU_SIZE 0x80000ULL
// DTS clock-controller@1400000 (qcom,khaje-gcc); DS gcc-khaje.c: every QUP branch votes in 0x7900c.
#define TB_GCC_BASE 0x01400000ULL
#define TB_GCC_VOTE_ENABLE 0x7900c
#define TB_GCC_GPLL0_ENABLE 0x79000           // gpll0 enable_reg, bit 0
#define TB_GCC_GPLL0_MODE 0x0                 // Lucid PLL MODE, LOCK_DET bit 31
#define TB_VOTE_WRAP0_CORE_2X (1u<<9)         // gcc_qupv3_wrap0_core_2x_clk, halt 0x1f014
#define TB_VOTE_WRAP0_CORE (1u<<8)            // gcc_qupv3_wrap0_core_clk, halt 0x1f00c
#define TB_VOTE_WRAP0_M_AHB (1u<<6)           // gcc_qupv3_wrap_0_m_ahb_clk, halt 0x1f004 (hwcg bit 1)
#define TB_VOTE_WRAP0_S_AHB (1u<<7)           // gcc_qupv3_wrap_0_s_ahb_clk, halt 0x1f008 (hwcg bit 1)
#define TB_VOTE_WRAP0_S0 (1u<<10)             // gcc_qupv3_wrap0_s0_clk, halt 0x1f144
#define TB_VOTE_ALL (TB_VOTE_WRAP0_CORE_2X|TB_VOTE_WRAP0_CORE|TB_VOTE_WRAP0_M_AHB|TB_VOTE_WRAP0_S_AHB|TB_VOTE_WRAP0_S0)
#define TB_HALT_CORE_2X 0x1f014
#define TB_HALT_CORE 0x1f00c
#define TB_HALT_M_AHB 0x1f004
#define TB_HALT_S_AHB 0x1f008
#define TB_HALT_S0 0x1f144
#define TB_CBCR_CLK_OFF (1u<<31)
// gcc_qupv3_wrap0_s0_clk_src: cmd_rcgr 0x1f148, mnd 16, hid 5; registered for DFS, which applies only if DFSR bit 0 reads 1.
#define TB_S0_RCG 0x1f148
#define TB_RCG_CMD 0x0                        // bit 0 UPDATE, bit 31 ROOT_OFF
#define TB_RCG_CFG 0x4
#define TB_RCG_M 0x8
#define TB_RCG_N 0xc
#define TB_RCG_DFSR_CMD 0x14                  // bit0 DFS enable, bits 4:1 current level
#define TB_RCG_PERF_CFG(l) (0x1c+4*(l))
#define TB_RCG_PERF_M(l) (0x5c+4*(l))
#define TB_RCG_PERF_N(l) (0x9c+4*(l))
#define TB_DFS_LEVELS 8
// DTS pinctrl@400000 (qcom,khaje-pinctrl); DS pinctrl-khaje.c: all touch pins are in the WEST tile at 0x400000 + 0x100000,
// ctl_reg = 0x1000*gpio, io_reg = +4; pull 1:0, func 5:2, drive 8:6 (mA/2-1), OE bit 9; io bit 0 in, bit 1 out.
#define TB_TLMM_BASE 0x00500000ULL
#define TB_GPIO_STRIDE 0x1000
#define TB_GPIO_SPI_FIRST 0                   // DTS qupv3_se0_spi_active: gpio0/1/2 data and clock, qupv3_se0_spi_cs_active: gpio3
#define TB_GPIO_SPI_CS 3
#define TB_GPIO_IRQ 80                        // himax,irq-gpio = <&tlmm 80 0x2008>, active low
#define TB_GPIO_RESET 86                      // himax,rst-gpio = <&tlmm 86 0>
#define TB_FUNC_QUP0 1                        // pinctrl-khaje.c PINGROUP(0..3, qup0, ...): function slot 1
#define TB_FUNC_GPIO 0
#define TB_DRIVE_6MA 2                        // DTS drive-strength = <6>
#define TB_DRIVE_8MA 3                        // DTS ts_int_active / ts_reset_active drive-strength = <8>
#define TB_PULL_NONE 0
#define TB_PULL_UP 3
#define TB_GPIO_OE (1u<<9)
#define TB_GPIO_IN 1u
#define TB_GPIO_OUT 2u
// The Himax driver overrides the DT's spi-cpha with SPI_MODE_3 (DS hxchipset/himax_platform.c); 10 MHz (DTS spi-max-frequency).
#define TB_SPI_MODE 3
#define TB_SPI_DEFAULT_HZ 10000000u
#define TB_SPI_MAX_HZ 10000000u

static inline uint32_t tb_pin_cfg(unsigned func, unsigned drive_code, unsigned pull) {return (pull&3u)|(func&15u)<<2|(drive_code&7u)<<6;}
static inline uint32_t tb_tlmm_ctl(unsigned gpio) {return (uint32_t)gpio*TB_GPIO_STRIDE;}
static inline uint32_t tb_tlmm_io(unsigned gpio) {return (uint32_t)gpio*TB_GPIO_STRIDE+4;}

// Source clock rates by RCG source-select value (gcc-khaje.c gcc_parent_map_1: 0 = TCXO, 1 = GPLL0 main, 2 = GPLL0 even,
// 4 = GPLL6 even). The PLL rates are inferred from the frequency tables (300 MHz = main / 2, 75 MHz = even / 4, 384 MHz = GPLL6 even).
static inline uint64_t tb_src_hz(unsigned sel) {
    return sel==0?19200000ULL:sel==1?600000000ULL:sel==2?300000000ULL:sel==4?384000000ULL:0;
}
// clk-rcg2.c calc_rate: half-integer divider, then M/N.
static inline uint64_t tb_calc_rate(uint64_t prate, uint32_t m, uint32_t n, unsigned mode, unsigned hid) {
    if (hid) {prate*=2;prate/=hid+1;}
    if (mode && n) {prate*=m;prate/=n;}
    return prate;
}
// Frequency of an RCG configuration from its CFG (src 10:8, mode 13:12, hid 4:0), M register and N register (stored inverted:
// n = (~N & mask) + m, mnd_width 16). Used for the DFS levels and for the plain RCG when DFS is off.
static inline uint64_t tb_rcg_hz(uint32_t cfg, uint32_t mreg, uint32_t nreg) {
    unsigned src=(cfg>>8)&7,mode=(cfg>>12)&3,hid=cfg&0x1f;
    uint32_t m=0,n=0;
    if (mode) {m=mreg&0xffff;n=((~nreg)&0xffff)+m;}
    return tb_calc_rate(tb_src_hz(src),m,n,mode,hid);
}
struct tb_clock_choice {unsigned level, div;uint64_t sclk_hz, actual_hz;};
// Choose the level that gives the highest SPI rate not above target with divider div = ceil(sclk / (os * target)). Returns 0 on
// success.
static inline int tb_choose_clock(const uint64_t *level_hz, unsigned n, unsigned os, unsigned target, struct tb_clock_choice *out) {
    int found=0;
    for (unsigned l=0;l<n;++l) {
        uint64_t f=level_hz[l];if (!f) continue;
        uint64_t d=((f+(uint64_t)os*target-1)/((uint64_t)os*target));if (d<1) d=1;if (d>0xfff) continue;
        uint64_t actual=f/(os*d);if (actual>target) continue;
        if (!found || actual>out->actual_hz || (actual==out->actual_hz && d<out->div)) {
            out->level=l;out->div=(unsigned)d;out->sclk_hz=f;out->actual_hz=actual;found=1;
        }
    }
    return found?0:-1;
}
#endif
