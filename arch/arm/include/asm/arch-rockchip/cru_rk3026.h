/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * RK3026 Clock and Reset Unit register layout
 *
 * Copyright (c) 2024, Mario Rugiero
 *
 * The RK3026 CRU shares the RK2928/RK3036 register family (same PLL block,
 * same clksel/clkgate stride).  It diverges from RK3036 in the SD/MMC clock
 * muxes, which select between only {cpll, gpll} with a 1-bit mux:
 *
 *   SDMMC: CLKSEL_CON(11), mux = bit 6  (0 = cpll, 1 = gpll), div = bits[5:0]
 *   SDIO:  CLKSEL_CON(12), mux = bit 6  (0 = cpll, 1 = gpll), div = bits[5:0]
 *   eMMC:  CLKSEL_CON(12), mux = bit 7  (0 = cpll, 1 = gpll), div = bits[14:8]
 *
 * Verified against the mainline clk-rk3026.c port (derived from the vendor
 * mach-rk3026/clock_data.c).
 */

#ifndef _ASM_ARCH_CRU_RK3026_H
#define _ASM_ARCH_CRU_RK3026_H

#include <linux/bitops.h>

#define RK3026_CRU_BASE		0x20000000

#define OSC_HZ		(24 * 1000 * 1000)

/*
 * Clock plan, taken from the vendor DDR blob's own CRU writes (stage 1, the
 * stage this SPL replaces) rather than from another SoC's port. Decoded from
 * set_pll at 0x10080a3c and its two callbacks; see docs/re/DDR-INIT.md.
 *
 * Do NOT substitute the rates Android runs at (APLL 912, GPLL 768, CPLL 560).
 * Those are the KERNEL's choice, applied long after the loader; the vendor
 * bootloader uses the values below and the MiniLoader never changes them.
 *
 * Divider semantics, which the previous rk3036-derived values got wrong:
 *   CORE_ACLK / CORE_PERI  divide core_pre (== APLL here), freediv, /(n+1)
 *   BUS_ACLK               divides the selected PLL, freediv, /(n+1)
 *   BUS_HCLK / BUS_PCLK    divide BUS_ACLK, freediv, /(n+1)   <-- not the PLL
 *   PERI_ACLK              divides the selected PLL, freediv, /(n+1)
 *   PERI_HCLK / PERI_PCLK  divide PERI_ACLK, SHIFT, >>n       <-- not freediv
 * Vendor kernel corroborates the two encodings: mach-rk3026/clock_data.c gives
 * hclk_cpu_pre/pclk_cpu_pre parent aclk_cpu_pre with clksel_recalc_div, and
 * hclk_periph/pclk_periph parent aclk_periph with clksel_recalc_shift.
 */
#define APLL_HZ		(600 * 1000000)		/* blob: (1,75,3,1), CONFIRMED */
#define GPLL_HZ		(132 * 1000000)		/* blob: (1,33,6,1) */
/*
 * CPLL is programmed by NO stage of the vendor bootloader — it stays in slow
 * mode (24 MHz) for the whole loader chain and only the kernel raises it (to
 * 560 MHz, live-measured). Kept only because the MMC muxes can name it; we
 * always select GPLL there, as the vendor does.
 */
#define CPLL_HZ		(594 * 1000000)		/* NOT vendor-established */

#define CORE_PERI_HZ	100000000		/* APLL/6,  blob div 5 */
#define CORE_ACLK_HZ	300000000		/* APLL/2,  blob div 1 */

#define BUS_ACLK_HZ	150000000		/* APLL/4,  blob div 3 */
#define BUS_HCLK_HZ	150000000		/* aclk/1,  blob div 0 */
#define BUS_PCLK_HZ	75000000		/* aclk/2,  blob div 1 */

#define PERI_ACLK_HZ	132000000		/* GPLL/1,  blob div 0 */
#define PERI_HCLK_HZ	132000000		/* aclk>>0, blob div 0 */
#define PERI_PCLK_HZ	66000000		/* aclk>>1, blob div 1 */

/* Private data for the clock driver - used by rockchip_get_cru() */
struct rk3026_clk_priv {
	struct rk3026_cru *cru;
	ulong rate;
};

struct rk3026_cru {
	struct rk3026_pll {
		unsigned int con0;
		unsigned int con1;
		unsigned int con2;
		unsigned int con3;
	} pll[4];
	unsigned int cru_mode_con;
	unsigned int cru_clksel_con[35];
	unsigned int cru_clkgate_con[11];
	unsigned int reserved;
	unsigned int cru_glb_srst_fst_value;
	unsigned int cru_glb_srst_snd_value;
	unsigned int reserved1[2];
	unsigned int cru_softrst_con[9];
	unsigned int cru_misc_con;
	unsigned int reserved2[2];
	unsigned int cru_glb_cnt_th;
	unsigned int cru_sdmmc_con[2];
	unsigned int cru_sdio_con[2];
	unsigned int cru_emmc_con[2];
};

struct pll_div {
	u32 refdiv;
	u32 fbdiv;
	u32 postdiv1;
	u32 postdiv2;
	u32 frac;
};

enum {
	/* PLLCON0 */
	PLL_POSTDIV1_SHIFT	= 12,
	PLL_POSTDIV1_MASK	= 7 << PLL_POSTDIV1_SHIFT,
	PLL_FBDIV_SHIFT		= 0,
	PLL_FBDIV_MASK		= 0xfff,

	/* PLLCON1 */
	PLL_RST_SHIFT		= 14,
	PLL_DSMPD_SHIFT		= 12,
	PLL_DSMPD_MASK		= 1 << PLL_DSMPD_SHIFT,
	PLL_LOCK_STATUS_SHIFT	= 10,
	PLL_LOCK_STATUS_MASK	= 1 << PLL_LOCK_STATUS_SHIFT,
	PLL_POSTDIV2_SHIFT	= 6,
	PLL_POSTDIV2_MASK	= 7 << PLL_POSTDIV2_SHIFT,
	PLL_REFDIV_SHIFT	= 0,
	PLL_REFDIV_MASK		= 0x3f,

	/* CRU_MODE */
	GPLL_MODE_SHIFT		= 12,
	GPLL_MODE_MASK		= 3 << GPLL_MODE_SHIFT,
	GPLL_MODE_SLOW		= 0,
	GPLL_MODE_NORM,
	GPLL_MODE_DEEP,
	CPLL_MODE_SHIFT		= 8,
	CPLL_MODE_MASK		= 1 << CPLL_MODE_SHIFT,
	CPLL_MODE_SLOW		= 0,
	CPLL_MODE_NORM,
	DPLL_MODE_SHIFT		= 4,
	DPLL_MODE_MASK		= 1 << DPLL_MODE_SHIFT,
	DPLL_MODE_SLOW		= 0,
	DPLL_MODE_NORM,
	APLL_MODE_SHIFT		= 0,
	APLL_MODE_MASK		= 1 << APLL_MODE_SHIFT,
	APLL_MODE_SLOW		= 0,
	APLL_MODE_NORM,

	/* CRU_CLKSEL0_CON */
	/*
	 * ONE bit at 13, not two at [15:14]. The vendor blob's CLKSEL_CON0 write
	 * mask is 0x3f9f, which excludes bits 14 and 15 entirely, and it drives
	 * bit 13 = 0 to source aclk_cpu from APLL. Live Android has bit 13 = 1
	 * (GPLL): CON0 = 0x00002300 with aclk_cpu = 768/4 = 192 MHz.
	 */
	BUS_ACLK_PLL_SEL_SHIFT	= 13,
	BUS_ACLK_PLL_SEL_MASK	= 1 << BUS_ACLK_PLL_SEL_SHIFT,
	BUS_ACLK_PLL_SEL_APLL	= 0,
	BUS_ACLK_PLL_SEL_GPLL	= 1,
	BUS_ACLK_DIV_SHIFT	= 8,
	BUS_ACLK_DIV_MASK	= 0x1f << BUS_ACLK_DIV_SHIFT,
	CORE_CLK_PLL_SEL_SHIFT	= 7,
	CORE_CLK_PLL_SEL_MASK	= 1 << CORE_CLK_PLL_SEL_SHIFT,
	CORE_CLK_PLL_SEL_APLL	= 0,
	CORE_CLK_PLL_SEL_GPLL,
	CORE_DIV_CON_SHIFT	= 0,
	CORE_DIV_CON_MASK	= 0x1f << CORE_DIV_CON_SHIFT,

	/* CRU_CLKSEL1_CON */
	BUS_PCLK_DIV_SHIFT	= 12,
	BUS_PCLK_DIV_MASK	= 7 << BUS_PCLK_DIV_SHIFT,
	BUS_HCLK_DIV_SHIFT	= 8,
	BUS_HCLK_DIV_MASK	= 3 << BUS_HCLK_DIV_SHIFT,
	CORE_ACLK_DIV_SHIFT	= 4,
	CORE_ACLK_DIV_MASK	= 7 << CORE_ACLK_DIV_SHIFT,
	CORE_PERI_DIV_SHIFT	= 0,
	CORE_PERI_DIV_MASK	= 0xf << CORE_PERI_DIV_SHIFT,

	/* CRU_CLKSEL10_CON */
	/*
	 * ONE bit at 15. The blob's CLKSEL_CON10 write mask is 0xb31f — bit 15
	 * in, bit 14 out — and it drives 0 for GPLL. Live Android agrees:
	 * CON10 = 0x00002103, bit 15 clear, aclk_periph = GPLL/4 = 192 MHz.
	 */
	PERI_PLL_SEL_SHIFT	= 15,
	PERI_PLL_SEL_MASK	= 1 << PERI_PLL_SEL_SHIFT,
	PERI_PLL_GPLL		= 0,
	PERI_PLL_CPLL		= 1,
	PERI_PCLK_DIV_SHIFT	= 12,
	PERI_PCLK_DIV_MASK	= 3 << PERI_PCLK_DIV_SHIFT,
	PERI_HCLK_DIV_SHIFT	= 8,
	PERI_HCLK_DIV_MASK	= 3 << PERI_HCLK_DIV_SHIFT,
	PERI_ACLK_DIV_SHIFT	= 0,
	PERI_ACLK_DIV_MASK	= 0x1f << PERI_ACLK_DIV_SHIFT,

	/*
	 * CRU_CLKSEL11_CON — SDMMC
	 *   mux  = bit 6  (0 = cpll, 1 = gpll)
	 *   div  = bits[5:0]
	 */
	SDMMC_PLL_SHIFT		= 6,
	SDMMC_PLL_MASK		= 1 << SDMMC_PLL_SHIFT,
	SDMMC_SEL_CPLL		= 0,
	SDMMC_SEL_GPLL		= 1,
	SDMMC_DIV_SHIFT		= 0,
	SDMMC_DIV_MASK		= 0x3f << SDMMC_DIV_SHIFT,

	/*
	 * CRU_CLKSEL12_CON — SDIO (low half) + eMMC (high half)
	 *   SDIO: mux = bit 6,  div = bits[5:0]
	 *   eMMC: mux = bit 7,  div = bits[14:8]
	 */
	SDIO_PLL_SHIFT		= 6,
	SDIO_PLL_MASK		= 1 << SDIO_PLL_SHIFT,
	SDIO_SEL_CPLL		= 0,
	SDIO_SEL_GPLL		= 1,
	SDIO_DIV_SHIFT		= 0,
	SDIO_DIV_MASK		= 0x3f << SDIO_DIV_SHIFT,
	EMMC_PLL_SHIFT		= 7,
	EMMC_PLL_MASK		= 1 << EMMC_PLL_SHIFT,
	EMMC_SEL_CPLL		= 0,
	EMMC_SEL_GPLL		= 1,
	EMMC_DIV_SHIFT		= 8,
	EMMC_DIV_MASK		= 0x7f << EMMC_DIV_SHIFT,

	/* CRU_SOFTRST5_CON */
	DDRCTRL_PSRST_SHIFT	= 11,
	DDRCTRL_SRST_SHIFT	= 10,
	DDRPHY_PSRST_SHIFT	= 9,
	DDRPHY_SRST_SHIFT	= 8,
};

#endif /* _ASM_ARCH_CRU_RK3026_H */
