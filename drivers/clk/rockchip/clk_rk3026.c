// SPDX-License-Identifier: GPL-2.0
/*
 * RK3026 clock driver
 *
 * Copyright (c) 2024, Mario Rugiero
 *
 * Adapted from clk_rk3036.c (C) Copyright 2015 Google, Inc.  The RK3026 shares
 * the RK3036 PLL block and clksel stride; the SD/MMC clock muxes differ (1-bit
 * cpll/gpll select) and are handled explicitly below.
 */

#include <clk-uclass.h>
#include <boox_probe.h>
#include <dm.h>
#include <dt-structs.h>
#include <errno.h>
#include <log.h>
#include <malloc.h>
#include <syscon.h>
#include <asm/arch-rockchip/clock.h>
#include <asm/arch-rockchip/cru_rk3026.h>
#include <asm/arch-rockchip/hardware.h>
#include <dm/device-internal.h>
#include <dm/lists.h>
#include <dt-bindings/clock/rk3026-cru.h>
#include <linux/delay.h>
#include <linux/log2.h>
#include <linux/stringify.h>

enum {
	VCO_MAX_HZ	= 2400U * 1000000,
	VCO_MIN_HZ	= 600 * 1000000,
	OUTPUT_MAX_HZ	= 2400U * 1000000,
	OUTPUT_MIN_HZ	= 24 * 1000000,
};

#define DIV_TO_RATE(input_rate, div)	((input_rate) / ((div) + 1))

/* Bound on the PLL lock poll; see rkclk_set_pll(). */
#define PLL_LOCK_POLLS			100000

#define PLL_DIVISORS(hz, _refdiv, _postdiv1, _postdiv2) {\
	.refdiv = _refdiv,\
	.fbdiv = (u32)((u64)hz * _refdiv * _postdiv1 * _postdiv2 / OSC_HZ),\
	.postdiv1 = _postdiv1, .postdiv2 = _postdiv2};\
	_Static_assert(((u64)hz * _refdiv * _postdiv1 * _postdiv2 / OSC_HZ) *\
			 OSC_HZ / (_refdiv * _postdiv1 * _postdiv2) == hz,\
			 #hz "Hz cannot be hit with PLL "\
			 "divisors on line " __stringify(__LINE__));

/* use integer mode */
/* Both from the vendor DDR blob's set_pll calls: APLL (1,75,3,1) = 600 MHz,
 * GPLL (1,33,6,1) = 132 MHz. refdiv, postdiv1, postdiv2 here; fbdiv derived. */
static const struct pll_div apll_init_cfg = PLL_DIVISORS(APLL_HZ, 1, 3, 1);
static const struct pll_div gpll_init_cfg = PLL_DIVISORS(GPLL_HZ, 1, 6, 1);


static int rkclk_set_pll(struct rk3026_cru *cru, enum rk_clk_id clk_id,
			 const struct pll_div *div)
{
	int pll_id = rk_pll_id(clk_id);
	struct rk3026_pll *pll = &cru->pll[pll_id];
	uint vco_hz = OSC_HZ / 1000 * div->fbdiv / div->refdiv * 1000;
	uint output_hz = vco_hz / div->postdiv1 / div->postdiv2;
	u32 i;

	debug("PLL at %p: fbdiv=%d, refdiv=%d, postdiv1=%d, postdiv2=%d,\
		 vco=%u Hz, output=%u Hz\n",
			pll, div->fbdiv, div->refdiv, div->postdiv1,
			div->postdiv2, vco_hz, output_hz);
	assert(vco_hz >= VCO_MIN_HZ && vco_hz <= VCO_MAX_HZ &&
	       output_hz >= OUTPUT_MIN_HZ && output_hz <= OUTPUT_MAX_HZ);

	/* use integer mode */
	rk_setreg(&pll->con1, 1 << PLL_DSMPD_SHIFT);

	rk_clrsetreg(&pll->con0,
		     PLL_POSTDIV1_MASK | PLL_FBDIV_MASK,
		     (div->postdiv1 << PLL_POSTDIV1_SHIFT) | div->fbdiv);
	rk_clrsetreg(&pll->con1, PLL_POSTDIV2_MASK | PLL_REFDIV_MASK,
		     (div->postdiv2 << PLL_POSTDIV2_SHIFT |
		     div->refdiv << PLL_REFDIV_SHIFT));

	/* 18: reached the PLL lock wait (i.e. clk probe is running) */
	BOOX_STEP_AT(18);

	/*
	 * Wait for lock. The bit reads SET WHEN LOCKED: device tier, read under
	 * Android where all four PLLs are necessarily locked and all four read
	 * set (docs/PORT-FIXES.md §"the PLL lock-status test is INVERTED"), and
	 * blob tier, the vendor exits on set (lsr #10, lsl #31, bne) and caps
	 * its retries. Waiting *while* set, as this did, returned instantly
	 * whenever the PLL was NOT locked -- so mode_con went to normal with an
	 * unlocked PLL.
	 *
	 * Bounded, and deliberately WITHOUT udelay(): this runs inside the CRU
	 * probe, and in U-Boot proper udelay() -> get_ticks() -> dm_timer_init()
	 * re-enters device_probe() on the device being probed. The bound is a
	 * raw poll count, not the vendor's 1000 tries -- its per-try cost is
	 * unknown, so the two are not comparable. Unmeasured either way; the
	 * only guarantee wanted here is that it terminates.
	 */
	for (i = 0; i < PLL_LOCK_POLLS; i++)
		if (readl(&pll->con1) & (1 << PLL_LOCK_STATUS_SHIFT))
			break;

	/* 19: the PLL lock wait returned */
	BOOX_STEP_AT(19);

	return 0;
}



static void rkclk_init(struct rk3026_cru *cru)
{
	u32 aclk_div;
	u32 hclk_div;
	u32 pclk_div;

	/* pll enter slow-mode */
	rk_clrsetreg(&cru->cru_mode_con,
		     GPLL_MODE_MASK | APLL_MODE_MASK,
		     GPLL_MODE_SLOW << GPLL_MODE_SHIFT |
		     APLL_MODE_SLOW << APLL_MODE_SHIFT);

	/* init pll */
	rkclk_set_pll(cru, CLK_ARM, &apll_init_cfg);
	rkclk_set_pll(cru, CLK_GENERAL, &gpll_init_cfg);

	/*
	 * select apll as cpu/core clock pll source and
	 * set up dependent divisors for PERI and ACLK clocks.
	 * core hz : apll = 1:1
	 */
	aclk_div = APLL_HZ / CORE_ACLK_HZ - 1;
	_Static_assert(APLL_HZ % CORE_ACLK_HZ == 0 &&
		       APLL_HZ / CORE_ACLK_HZ - 1 < 0x7,
		       "core aclk: APLL/CORE_ACLK not exact, or divider > 3 bits");

	pclk_div = APLL_HZ / CORE_PERI_HZ - 1;
	_Static_assert(APLL_HZ % CORE_PERI_HZ == 0 &&
		       APLL_HZ / CORE_PERI_HZ - 1 < 0xf,
		       "core peri: APLL/CORE_PERI not exact, or divider > 4 bits");

	rk_clrsetreg(&cru->cru_clksel_con[0],
		     CORE_CLK_PLL_SEL_MASK | CORE_DIV_CON_MASK,
		     CORE_CLK_PLL_SEL_APLL << CORE_CLK_PLL_SEL_SHIFT |
		     0 << CORE_DIV_CON_SHIFT);

	rk_clrsetreg(&cru->cru_clksel_con[1],
		     CORE_ACLK_DIV_MASK | CORE_PERI_DIV_MASK,
		     aclk_div << CORE_ACLK_DIV_SHIFT |
		     pclk_div << CORE_PERI_DIV_SHIFT);

	/*
	 * pd_bus. The vendor sources aclk_cpu from APLL here (CLKSEL_CON0 bit
	 * 13 = 0), not from GPLL -- GPLL is only 132 MHz at this stage.
	 *
	 * BUS_HCLK and BUS_PCLK divide BUS_ACLK, not the PLL. Dividing the PLL
	 * is what the rk3036-derived version did, and with assert() compiled out
	 * it programmed 3 and 7 where the vendor writes 0 and 1 -- silently, so
	 * hclk/pclk came out 4x and 8x low.
	 */
	aclk_div = APLL_HZ / BUS_ACLK_HZ - 1;
	_Static_assert(APLL_HZ % BUS_ACLK_HZ == 0 &&
		       APLL_HZ / BUS_ACLK_HZ - 1 <= 0x1f,
		       "bus aclk: APLL/BUS_ACLK not exact, or divider > 5 bits");

	pclk_div = BUS_ACLK_HZ / BUS_PCLK_HZ - 1;
	_Static_assert(BUS_ACLK_HZ % BUS_PCLK_HZ == 0 &&
		       BUS_ACLK_HZ / BUS_PCLK_HZ - 1 <= 0x7,
		       "bus pclk: divides BUS_ACLK, not exact, or divider > 3 bits");

	hclk_div = BUS_ACLK_HZ / BUS_HCLK_HZ - 1;
	_Static_assert(BUS_ACLK_HZ % BUS_HCLK_HZ == 0 &&
		       BUS_ACLK_HZ / BUS_HCLK_HZ - 1 <= 0x3,
		       "bus hclk: divides BUS_ACLK, not exact, or divider > 2 bits");

	rk_clrsetreg(&cru->cru_clksel_con[0],
		     BUS_ACLK_PLL_SEL_MASK | BUS_ACLK_DIV_MASK,
		     BUS_ACLK_PLL_SEL_APLL << BUS_ACLK_PLL_SEL_SHIFT |
		     aclk_div << BUS_ACLK_DIV_SHIFT);

	rk_clrsetreg(&cru->cru_clksel_con[1],
		     BUS_PCLK_DIV_MASK | BUS_HCLK_DIV_MASK,
		     pclk_div << BUS_PCLK_DIV_SHIFT |
		     hclk_div << BUS_HCLK_DIV_SHIFT);

	/*
	 * select gpll as pd_peri bus clock source and
	 * set up dependent divisors for PCLK/HCLK and ACLK clocks.
	 */
	aclk_div = GPLL_HZ / PERI_ACLK_HZ - 1;
	_Static_assert(GPLL_HZ % PERI_ACLK_HZ == 0 &&
		       GPLL_HZ / PERI_ACLK_HZ - 1 < 0x1f,
		       "peri aclk: GPLL/PERI_ACLK not exact, or divider > 5 bits");

	hclk_div = ilog2(PERI_ACLK_HZ / PERI_HCLK_HZ);
	_Static_assert((1 << ilog2(PERI_ACLK_HZ / PERI_HCLK_HZ)) * PERI_HCLK_HZ ==
		       PERI_ACLK_HZ &&
		       ilog2(PERI_ACLK_HZ / PERI_HCLK_HZ) < 0x4,
		       "peri hclk: shift not exact, or shift > 2 bits");

	pclk_div = ilog2(PERI_ACLK_HZ / PERI_PCLK_HZ);
	_Static_assert((1 << ilog2(PERI_ACLK_HZ / PERI_PCLK_HZ)) * PERI_PCLK_HZ ==
		       PERI_ACLK_HZ &&
		       ilog2(PERI_ACLK_HZ / PERI_PCLK_HZ) < 0x8,
		       "peri pclk: shift not exact, or shift > 3 bits");

	rk_clrsetreg(&cru->cru_clksel_con[10],
		     PERI_PLL_SEL_MASK | PERI_PCLK_DIV_MASK |
		     PERI_HCLK_DIV_MASK | PERI_ACLK_DIV_MASK,
		     PERI_PLL_GPLL << PERI_PLL_SEL_SHIFT |
		     pclk_div << PERI_PCLK_DIV_SHIFT |
		     hclk_div << PERI_HCLK_DIV_SHIFT |
		     aclk_div << PERI_ACLK_DIV_SHIFT);

	/* PLL enter normal-mode */
	rk_clrsetreg(&cru->cru_mode_con,
		     GPLL_MODE_MASK | APLL_MODE_MASK,
		     GPLL_MODE_NORM << GPLL_MODE_SHIFT |
		     APLL_MODE_NORM << APLL_MODE_SHIFT);
}

/* Get pll rate by id */
/*
 * DM clock OPS -- U-Boot proper only.
 *
 * The SPL calls exactly one thing in this driver: rkclk_init(), from the probe.
 * It never asks for a rate and never gates a clock, so the rate getters, the
 * mmc divider maths and the 40-entry gate table are dead weight in a 6144 B
 * SRAM window. Measured: this file is 1233 B of the SPL, the largest item after
 * DDR init.
 */
#if !defined(CONFIG_XPL_BUILD)
static uint32_t rkclk_pll_get_rate(struct rk3026_cru *cru,
				   enum rk_clk_id clk_id)
{
	uint32_t refdiv, fbdiv, postdiv1, postdiv2;
	uint32_t con;
	int pll_id = rk_pll_id(clk_id);
	struct rk3026_pll *pll = &cru->pll[pll_id];
	static u8 clk_shift[CLK_COUNT] = {
		0xff, APLL_MODE_SHIFT, DPLL_MODE_SHIFT, CPLL_MODE_SHIFT,
		GPLL_MODE_SHIFT, 0xff
	};
	static u32 clk_mask[CLK_COUNT] = {
		0xffffffff, APLL_MODE_MASK, DPLL_MODE_MASK, CPLL_MODE_MASK,
		GPLL_MODE_MASK, 0xffffffff
	};
	uint shift;
	uint mask;

	con = readl(&cru->cru_mode_con);
	shift = clk_shift[clk_id];
	mask = clk_mask[clk_id];

	switch ((con & mask) >> shift) {
	case GPLL_MODE_SLOW:
		return OSC_HZ;
	case GPLL_MODE_NORM:
		/* normal mode */
		con = readl(&pll->con0);
		postdiv1 = (con & PLL_POSTDIV1_MASK) >> PLL_POSTDIV1_SHIFT;
		fbdiv = (con & PLL_FBDIV_MASK) >> PLL_FBDIV_SHIFT;
		con = readl(&pll->con1);
		postdiv2 = (con & PLL_POSTDIV2_MASK) >> PLL_POSTDIV2_SHIFT;
		refdiv = (con & PLL_REFDIV_MASK) >> PLL_REFDIV_SHIFT;
		return (24 * fbdiv / (refdiv * postdiv1 * postdiv2)) * 1000000;
	case GPLL_MODE_DEEP:
	default:
		return 32768;
	}
}

/*
 * SD/MMC clock.  On RK3026 the source mux is a single bit selecting cpll(0)
 * or gpll(1).  We always run from gpll, as the vendor does.  The DW MMC
 * controller halves the source internally, hence the extra /2.
 *
 * Selecting cpll would be wrong here, and not merely suboptimal: no stage of the
 * vendor bootloader programs or enables CPLL, and a device read at our own U-Boot
 * prompt confirms it -- MODE_CON = 0x1011 has the CPLL field in SLOW mode while
 * its CON0/CON1 still hold Android's 560 MHz configuration, because a warm reset
 * does not clear PLL_CON. So cpll reads as configured-but-bypassed: 24 MHz.
 *
 * gpll is 132 MHz at this stage (not the 594 MHz an earlier comment claimed), so
 * rates are computed from the live PLL rate rather than a constant.
 */
static ulong rockchip_mmc_get_clk(struct rk3026_cru *cru, uint gpll_rate,
				  int periph)
{
	uint div, mux;
	u32 con;

	switch (periph) {
	case HCLK_EMMC:
	case SCLK_EMMC:
		con = readl(&cru->cru_clksel_con[12]);
		mux = (con & EMMC_PLL_MASK) >> EMMC_PLL_SHIFT;
		div = (con & EMMC_DIV_MASK) >> EMMC_DIV_SHIFT;
		break;
	case HCLK_SDMMC:
	case SCLK_SDMMC:
		con = readl(&cru->cru_clksel_con[11]);
		mux = (con & SDMMC_PLL_MASK) >> SDMMC_PLL_SHIFT;
		div = (con & SDMMC_DIV_MASK) >> SDMMC_DIV_SHIFT;
		break;
	case HCLK_SDIO:
	case SCLK_SDIO:
		con = readl(&cru->cru_clksel_con[12]);
		mux = (con & SDIO_PLL_MASK) >> SDIO_PLL_SHIFT;
		div = (con & SDIO_DIV_MASK) >> SDIO_DIV_SHIFT;
		break;
	default:
		return -EINVAL;
	}

	/* cpll and gpll are configured to the same rate here */
	return DIV_TO_RATE(gpll_rate, div) / 2;
}

static ulong rockchip_mmc_set_clk(struct rk3026_cru *cru, uint gpll_rate,
				  int periph, uint freq)
{
	int src_clk_div;

	debug("%s: gpll_rate=%u, freq=%u\n", __func__, gpll_rate, freq);

	/*
	 * mmc clock is divided by 2 inside the DW MMC controller.
	 *
	 * The divider field is 6 bits, so src_clk_div - 1 must be <= 63. The
	 * MMC core asks for 400 kHz during identification, and with gpll at
	 * 132 MHz that wants DIV_ROUND_UP(66 MHz, 400 kHz) = 165: the old code
	 * wrote 164 = 0xa4 into the field, assert() being compiled out in a
	 * release build. MEASURED consequence on hardware 2026-07-29,
	 * CLKSEL_CON11 read from the U-Boot prompt as 0x000027e4 -- field
	 * [5:0] = 0xa4 & 0x3f = 36, and 0xa4's bit 7 spilled outside the field.
	 * The card therefore ran at 132/37/2 = 1.78 MHz during identification
	 * instead of 400 kHz: every command still answered, and ACMD51
	 * SEND_SCR -- the first data transfer -- returned -110 every time.
	 *
	 * Clamp instead, and return the rate actually programmed so the caller
	 * can make up the difference with the controller's own CLKDIV. That is
	 * what the BootROM does: it writes CLKSEL_CON11 once and then uses
	 * CLKDIV = 30 for identification, CLKDIV = 1 afterwards
	 * (docs/re/BROM-SD-PATH.md section 5, BINARY tier).
	 */
	src_clk_div = DIV_ROUND_UP(gpll_rate / 2, freq);
	if (src_clk_div > 64)
		src_clk_div = 64;

	switch (periph) {
	case HCLK_EMMC:
	case SCLK_EMMC:
		rk_clrsetreg(&cru->cru_clksel_con[12],
			     EMMC_PLL_MASK | EMMC_DIV_MASK,
			     EMMC_SEL_GPLL << EMMC_PLL_SHIFT |
			     (src_clk_div - 1) << EMMC_DIV_SHIFT);
		break;
	case HCLK_SDMMC:
	case SCLK_SDMMC:
		rk_clrsetreg(&cru->cru_clksel_con[11],
			     SDMMC_PLL_MASK | SDMMC_DIV_MASK,
			     SDMMC_SEL_GPLL << SDMMC_PLL_SHIFT |
			     (src_clk_div - 1) << SDMMC_DIV_SHIFT);
		break;
	case HCLK_SDIO:
	case SCLK_SDIO:
		rk_clrsetreg(&cru->cru_clksel_con[12],
			     SDIO_PLL_MASK | SDIO_DIV_MASK,
			     SDIO_SEL_GPLL << SDIO_PLL_SHIFT |
			     (src_clk_div - 1) << SDIO_DIV_SHIFT);
		break;
	default:
		return -EINVAL;
	}

	return rockchip_mmc_get_clk(cru, gpll_rate, periph);
}

/*
 * The peripheral bus tree, read from the live dividers rather than assumed from
 * the *_HZ constants -- those describe what rkclk_init() intends to program, and
 * a consumer needs what is actually there.
 *
 * CLKSEL_CON10 encodes two different divider styles, which is a documented trap
 * (cru_rk3026.h): PERI_ACLK is a freedivider off the selected PLL, /(n+1), while
 * PERI_HCLK and PERI_PCLK are SHIFTS off PERI_ACLK, >>n.
 */
static ulong rk3026_peri_get_rate(struct rk3026_cru *cru, ulong gpll_rate,
				  ulong cpll_rate, int id)
{
	u32 con = readl(&cru->cru_clksel_con[10]);
	ulong parent, aclk;
	u32 div;

	parent = ((con & PERI_PLL_SEL_MASK) >> PERI_PLL_SEL_SHIFT) ==
			PERI_PLL_CPLL ? cpll_rate : gpll_rate;

	div = (con & PERI_ACLK_DIV_MASK) >> PERI_ACLK_DIV_SHIFT;
	aclk = parent / (div + 1);

	switch (id) {
	case ACLK_PERI:
		return aclk;
	case HCLK_PERI:
		return aclk >> ((con & PERI_HCLK_DIV_MASK) >> PERI_HCLK_DIV_SHIFT);
	default:
		return aclk >> ((con & PERI_PCLK_DIV_MASK) >> PERI_PCLK_DIV_SHIFT);
	}
}

static ulong rk3026_clk_get_rate(struct clk *clk)
{
	struct rk3026_clk_priv *priv = dev_get_priv(clk->dev);
	ulong gclk_rate;

	switch (clk->id) {
	/* PLL DT ids: PLL_APLL(0) PLL_DPLL(1) PLL_CPLL(2) PLL_GPLL(3) */
	case PLL_APLL ... PLL_GPLL:
		return rkclk_pll_get_rate(priv->cru, clk->id);
	case HCLK_EMMC:
	case SCLK_EMMC:
	case HCLK_SDMMC:
	case SCLK_SDMMC:
	case HCLK_SDIO:
	case SCLK_SDIO:
		gclk_rate = rkclk_pll_get_rate(priv->cru, CLK_GENERAL);
		return rockchip_mmc_get_clk(priv->cru, gclk_rate, clk->id);
	/*
	 * The peripheral bus roots, and the APB clocks that hang off PCLK_PERI.
	 * rk_i2c calls clk_get_rate() with NO IS_ERR_VALUE() check
	 * (drivers/i2c/rk_i2c.c:77), so an unhandled id here becomes 0xfffffffe
	 * and lands as a ~2683 divider: SCL drops to about 3 kHz off a 66 MHz
	 * PCLK and every transfer crawls, then times out. MEASURED that way on
	 * hardware 2026-07-30 -- `i2c probe` returned an empty list on both
	 * buses while the GRF select and the pad mux were both verified correct.
	 *
	 * NOT LISTED, DELIBERATELY: PCLK_TIMER0. timer_pre_probe() takes clk
	 * index 0 as the timer's COUNTING rate and returns before reading the
	 * DT's clock-frequency, so teaching this switch about PCLK_TIMER0 would
	 * silently run the timebase at the APB rate instead of xin24m's 24 MHz.
	 * See docs/PORT-FIXES.md §3b.
	 */
	case ACLK_PERI:
	case HCLK_PERI:
	case PCLK_PERI:
	case PCLK_I2C0:
	case PCLK_I2C1:
	case PCLK_I2C2:
	case PCLK_I2C3:
		return rk3026_peri_get_rate(priv->cru,
					    rkclk_pll_get_rate(priv->cru,
							       CLK_GENERAL),
					    rkclk_pll_get_rate(priv->cru,
							       CLK_CODEC),
					    clk->id);
	default:
		return -ENOENT;
	}
}

static ulong rk3026_clk_set_rate(struct clk *clk, ulong rate)
{
	struct rk3026_clk_priv *priv = dev_get_priv(clk->dev);
	ulong new_rate, gclk_rate;

	gclk_rate = rkclk_pll_get_rate(priv->cru, CLK_GENERAL);
	switch (clk->id) {
	case PLL_APLL ... PLL_GPLL:
		return 0;
	case HCLK_EMMC:
	case SCLK_EMMC:
	case HCLK_SDMMC:
	case SCLK_SDMMC:
	case HCLK_SDIO:
	case SCLK_SDIO:
		new_rate = rockchip_mmc_set_clk(priv->cru, gclk_rate,
						clk->id, rate);
		break;
	default:
		return -ENOENT;
	}

	return new_rate;
}

/*
 * Clock gates.
 *
 * Every (reg, bit) below is attributed in docs/re/CRU-GATES.md §6.2, which maps
 * 100 of the 160 (register, bit) slots from the running vendor kernel's own clock
 * table [blob] and cross-checks them against a live CLKGATE_CON snapshot [device].
 * Nothing here is guessed: an id whose gate is not attributed is simply absent
 * from the table.
 *
 * Two rules that are not obvious and both cost real time to establish:
 *
 *  - cru_clkgate_con[] is declared with 11 entries (cru_rk3026.h) but the hardware
 *    has only 10 registers, CON0..CON9. Writing index 10 pokes whatever follows.
 *    rk3026_gate_set() refuses it rather than trusting the table.
 *
 *  - Several leaves sit behind a bus gate, so entries list their parents FIRST and
 *    enable walks the list in order (the parent-before-child rule from
 *    clk_enable_nolock, CRU-GATES §1). disable() touches only the leaf: parents are
 *    shared, and gating one because a single child went away would take out its
 *    siblings.
 */
#define RK3026_NR_CLKGATE_CON	10

struct rk3026_gate_bit {
	u8	reg;
	u8	bit;
};

struct rk3026_gate {
	u16			id;
	u8			n;
	struct rk3026_gate_bit	bits[2];	/* parent first, leaf last */
};

static const struct rk3026_gate rk3026_gates[] = {
	/* bus roots */
	{ ACLK_PERI,	1, { { 2, 1 } } },
	{ HCLK_PERI,	1, { { 2, 2 } } },
	{ PCLK_PERI,	1, { { 2, 3 } } },

	/* USB OTG device mode: hclk_usb_peri (CON9 b13) gates hclk_otg0 */
	{ HCLK_OTG,	2, { { 9, 13 }, { 5, 13 } } },

	/* SD/MMC */
	{ SCLK_SDMMC,	1, { { 2, 11 } } },
	{ HCLK_SDMMC,	1, { { 5, 10 } } },

	/* SARADC -- the source clock's parent is xin24m, not a bus clock */
	{ SCLK_SARADC,	1, { { 2, 8 } } },
	{ PCLK_SARADC,	1, { { 7, 14 } } },

	/* console */
	{ PCLK_UART2,	1, { { 8, 2 } } },
	{ SCLK_UART2,	1, { { 1, 12 } } },

	/* timers: APB access only. The 24 MHz SOURCE clocks (CON1 b0/b1) have no
	 * id in dt-bindings/clock/rk3026-cru.h, so they cannot be requested. */
	{ PCLK_TIMER0,	1, { { 7, 7 } } },
	{ PCLK_TIMER1,	1, { { 7, 8 } } },

	{ PCLK_GPIO0,	1, { { 8, 9 } } },
	{ PCLK_GPIO1,	1, { { 8, 10 } } },
	{ PCLK_GPIO2,	1, { { 8, 11 } } },
	{ PCLK_GPIO3,	1, { { 8, 12 } } },

	{ PCLK_I2C0,	1, { { 8, 4 } } },
	{ PCLK_I2C1,	1, { { 8, 5 } } },
	{ PCLK_I2C2,	1, { { 8, 6 } } },
	{ PCLK_I2C3,	1, { { 8, 7 } } },

	{ HCLK_NANDC,	1, { { 5, 9 } } },

	/* pclk_grf's parent is pclk_cpu_pre (CON0 b5), not pclk_periph */
	{ PCLK_GRF,	1, { { 5, 4 } } },
};

/* 1 = gated. Hiword mask: [31:16] write-enable, [15:0] value. */
static void rk3026_gate_set(struct rk3026_cru *cru, u8 reg, u8 bit, bool on)
{
	if (reg >= RK3026_NR_CLKGATE_CON) {
		log_err("rk3026 clk: CLKGATE_CON(%u) does not exist\n", reg);
		return;
	}

	writel(on ? BIT(bit + 16) : (BIT(bit) | BIT(bit + 16)),
	       &cru->cru_clkgate_con[reg]);
}

static const struct rk3026_gate *rk3026_gate_find(ulong id)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(rk3026_gates); i++)
		if (rk3026_gates[i].id == id)
			return &rk3026_gates[i];

	return NULL;
}

/*
 * An id with no attributed gate returns 0 rather than -ENOENT on purpose: callers
 * that treat clk_enable() as fatal would otherwise fail a probe over a clock this
 * driver simply does not model yet, which is a regression against the current
 * behaviour where clk_enable() is a no-op for everything.
 */
static int rk3026_clk_enable(struct clk *clk)
{
	struct rk3026_clk_priv *priv = dev_get_priv(clk->dev);
	const struct rk3026_gate *g = rk3026_gate_find(clk->id);
	int i;

	if (!g)
		return 0;

	for (i = 0; i < g->n; i++)
		rk3026_gate_set(priv->cru, g->bits[i].reg, g->bits[i].bit, true);

	return 0;
}

static int rk3026_clk_disable(struct clk *clk)
{
	struct rk3026_clk_priv *priv = dev_get_priv(clk->dev);
	const struct rk3026_gate *g = rk3026_gate_find(clk->id);

	if (!g)
		return 0;

	/* leaf only -- see the comment above the table */
	rk3026_gate_set(priv->cru, g->bits[g->n - 1].reg,
			g->bits[g->n - 1].bit, false);

	return 0;
}

static struct clk_ops rk3026_clk_ops = {
	.get_rate	= rk3026_clk_get_rate,
	.set_rate	= rk3026_clk_set_rate,
	.enable		= rk3026_clk_enable,
	.disable	= rk3026_clk_disable,
};
#endif /* !CONFIG_XPL_BUILD */

/*
 * The SPL builds this driver with OF_PLATDATA, where there is no live device
 * tree to read: dtoc turns the cru node into a dtd_ struct at build time. Same
 * driver, same rkclk_init(), two ways in.
 */
#if CONFIG_IS_ENABLED(OF_PLATDATA)
struct rk3026_clk_plat {
	struct dtd_rockchip_rk3026_cru dtd;
};
#endif

static int rk3026_clk_of_to_plat(struct udevice *dev)
{
	struct rk3026_clk_priv *priv = dev_get_priv(dev);

	if (CONFIG_IS_ENABLED(OF_REAL)) {
		priv->cru = dev_read_addr_ptr(dev);
	} else {
#if CONFIG_IS_ENABLED(OF_PLATDATA)
		struct rk3026_clk_plat *plat = dev_get_plat(dev);

		priv->cru = (struct rk3026_cru *)(uintptr_t)plat->dtd.reg[0];
#endif
	}

	return 0;
}

static int rk3026_clk_probe(struct udevice *dev)
{
	struct rk3026_clk_priv *priv = dev_get_priv(dev);

	rkclk_init(priv->cru);

	/* 40: the whole CRU probe finished, PLLs and dividers included */
	BOOX_STEP_AT(40);

	return 0;
}

/*
 * Runtime binding is unavailable in the SPL and unnecessary there.
 *
 * SPL_OF_PLATDATA_INST declares udevices at build time and its companion
 * SPL_OF_PLATDATA_NO_BIND (default y) removes the run-time bind path entirely --
 * which is what lets the SPL have DM without SPL_FRAMEWORK, SPL_SYS_MALLOC_F or
 * a malloc pool. This function's children (sysreset, and the reset controller)
 * belong to U-Boot proper; the SPL only needs rkclk_init() from the probe.
 */
#if !defined(CONFIG_XPL_BUILD)
static int rk3026_clk_bind(struct udevice *dev)
{
	int ret;
	struct udevice *sys_child;
	struct sysreset_reg *priv;

	/* The reset driver does not have a device node, so bind it here */
	ret = device_bind_driver(dev, "rockchip_sysreset", "sysreset",
				 &sys_child);
	if (ret) {
		debug("Warning: No sysreset driver: ret=%d\n", ret);
	} else {
		priv = malloc(sizeof(struct sysreset_reg));
		priv->glb_srst_fst_value = offsetof(struct rk3026_cru,
						    cru_glb_srst_fst_value);
		priv->glb_srst_snd_value = offsetof(struct rk3026_cru,
						    cru_glb_srst_snd_value);
		dev_set_priv(sys_child, priv);
	}

#if CONFIG_IS_ENABLED(RESET_ROCKCHIP)
	ret = offsetof(struct rk3026_cru, cru_softrst_con[0]);
	ret = rockchip_reset_bind(dev, ret, 9);
	if (ret)
		debug("Warning: software reset driver bind failed\n");
#endif

	return 0;
}
#endif /* !CONFIG_XPL_BUILD */

static const struct udevice_id rk3026_clk_ids[] = {
	{ .compatible = "rockchip,rk3026-cru" },
	{ }
};

U_BOOT_DRIVER(rockchip_rk3026_cru) = {
	.name		= "clk_rk3026",
	.id		= UCLASS_CLK,
	.of_match	= rk3026_clk_ids,
	.priv_auto	= sizeof(struct rk3026_clk_priv),
	.of_to_plat	= rk3026_clk_of_to_plat,
#if !defined(CONFIG_XPL_BUILD)
	.ops		= &rk3026_clk_ops,
#endif
#if !defined(CONFIG_XPL_BUILD)
	.bind		= rk3026_clk_bind,
#endif
	.probe		= rk3026_clk_probe,
};
