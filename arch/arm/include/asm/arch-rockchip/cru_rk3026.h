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

#define APLL_HZ		(600 * 1000000)
#define GPLL_HZ		(594 * 1000000)
#define CPLL_HZ		(594 * 1000000)

#define CORE_PERI_HZ	150000000
#define CORE_ACLK_HZ	300000000

#define BUS_ACLK_HZ	148500000
#define BUS_HCLK_HZ	148500000
#define BUS_PCLK_HZ	74250000

#define PERI_ACLK_HZ	148500000
#define PERI_HCLK_HZ	148500000
#define PERI_PCLK_HZ	74250000

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
	BUS_ACLK_PLL_SEL_SHIFT	= 14,
	BUS_ACLK_PLL_SEL_MASK	= 3 << BUS_ACLK_PLL_SEL_SHIFT,
	BUS_ACLK_PLL_SEL_APLL	= 0,
	BUS_ACLK_PLL_SEL_DPLL,
	BUS_ACLK_PLL_SEL_GPLL,
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
	PERI_PLL_SEL_SHIFT	= 14,
	PERI_PLL_SEL_MASK	= 3 << PERI_PLL_SEL_SHIFT,
	PERI_PLL_APLL		= 0,
	PERI_PLL_DPLL,
	PERI_PLL_GPLL,
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
