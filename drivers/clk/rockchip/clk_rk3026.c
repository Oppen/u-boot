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
	assert((aclk_div + 1) * CORE_ACLK_HZ == APLL_HZ && aclk_div < 0x7);

	pclk_div = APLL_HZ / CORE_PERI_HZ - 1;
	assert((pclk_div + 1) * CORE_PERI_HZ == APLL_HZ && pclk_div < 0xf);

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
	assert((aclk_div + 1) * BUS_ACLK_HZ == APLL_HZ && aclk_div <= 0x1f);

	pclk_div = BUS_ACLK_HZ / BUS_PCLK_HZ - 1;
	assert((pclk_div + 1) * BUS_PCLK_HZ == BUS_ACLK_HZ && pclk_div <= 0x7);

	hclk_div = BUS_ACLK_HZ / BUS_HCLK_HZ - 1;
	assert((hclk_div + 1) * BUS_HCLK_HZ == BUS_ACLK_HZ && hclk_div <= 0x3);

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
	assert((aclk_div + 1) * PERI_ACLK_HZ == GPLL_HZ && aclk_div < 0x1f);

	hclk_div = ilog2(PERI_ACLK_HZ / PERI_HCLK_HZ);
	assert((1 << hclk_div) * PERI_HCLK_HZ ==
		PERI_ACLK_HZ && (hclk_div < 0x4));

	pclk_div = ilog2(PERI_ACLK_HZ / PERI_PCLK_HZ);
	assert((1 << pclk_div) * PERI_PCLK_HZ ==
		PERI_ACLK_HZ && pclk_div < 0x8);

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
 * or gpll(1).  We always run from gpll (== cpll == 594 MHz here).  The DW MMC
 * controller halves the source internally, hence the extra /2.
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

	/* mmc clock is divided by 2 inside the DW MMC controller */
	src_clk_div = DIV_ROUND_UP(gpll_rate / 2, freq);
	assert(src_clk_div - 1 < 64);

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

static struct clk_ops rk3026_clk_ops = {
	.get_rate	= rk3026_clk_get_rate,
	.set_rate	= rk3026_clk_set_rate,
};

static int rk3026_clk_of_to_plat(struct udevice *dev)
{
	struct rk3026_clk_priv *priv = dev_get_priv(dev);

	priv->cru = dev_read_addr_ptr(dev);

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
	.ops		= &rk3026_clk_ops,
	.bind		= rk3026_clk_bind,
	.probe		= rk3026_clk_probe,
};
