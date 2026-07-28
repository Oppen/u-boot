// SPDX-License-Identifier: GPL-2.0+ OR BSD-3-Clause
/*
 * (C) Copyright 2015 Rockchip Electronics Co., Ltd
 * Copyright (c) 2024, Mario Rugiero (RK3026 port)
 *
 * DDR3 init for RK3026, ported from rk3036/sdram_rk3036.c.  RK3026 is the same
 * RK2928-generation die: CRU 0x20000000, GRF 0x20008000, DDR_PCTL 0x20004000,
 * DDR_PHY 0x2000a000, CPU_AXI 0x10128000 all match the vendor kernel's
 * arch/arm/mach-rk2928/include/mach/io.h, so no register base changed.
 *
 * The SPL must do this before back_to_bootrom(): the mask ROM's next action is
 * to load U-Boot proper *into DRAM*.
 *
 * Timings, PLL and register writes below are transcribed from the vendor DDR
 * init blob (load base 0x10080800), which runs this board's DDR at 300 MHz.
 */
#include <config.h>
#include <init.h>
#include <asm/types.h>
#include <asm/arch-rockchip/cru_rk3026.h>
#include <asm/arch-rockchip/grf_rk3026.h>
#include <asm/arch-rockchip/hardware.h>
#include <asm/arch-rockchip/sdram_rk3026.h>
#include <asm/arch-rockchip/uart.h>
#include <linux/delay.h>

/*
 * Hard-coded rather than read from the device tree: the SPL budget is 6 KiB
 * (IRAM 0x10080800-0x10082000, docs/BROM.md §4).  The 4 KiB figure this comment
 * carried is rk3036's.  Whether SPL_OF_PLATDATA would fit here is unmeasured;
 * rk3188 does it on the same CPU, but with 30 KiB.
 */
#define CRU_BASE	0x20000000
#define GRF_BASE	0x20008000
#define DDR_PHY_BASE	0x2000a000
#define DDR_PCTL_BASE	0x20004000
#define CPU_AXI_BUS_BASE	0x10128000

/*
 * Absolute GRF addresses the vendor DDR blob uses.  Written literally rather
 * than through struct rk3026_grf: the GRF map above offset 0x100 is only
 * partly verified, and these two are the ones proven from the blob.
 *	0x200081cc — DRAM geometry word (os_reg[1])
 *	0x20008148 — masked bit-7 set, value 0x00800080
 */
#define GRF_OS_REG1		0x200081cc
#define GRF_SOC_CON2_ADDR	0x20008148

struct rk3026_sdram_priv {
	struct rk3026_cru *cru;
	struct rk3026_grf *grf;
	struct rk3026_ddr_phy *phy;
	struct rk3026_ddr_pctl *pctl;
	struct rk3026_service_sys *axi_bus;

	/* ddr die config */
	struct rk3026_ddr_config ddr_config;
};

/*
 * integer mode, dpll output 600MHz and ddr get 300MHz
 * refdiv, fbdiv, postdiv1, postdiv2
 */
const struct pll_div dpll_init_cfg = {1, 75, 3, 1};

/* 300MHz ddr timing, transcribed from the vendor blob's parameter struct */
const struct rk3026_ddr_timing ddr_timing = {0x12c,
	{0x12c, 0xc8, 0x1f4, 0x1e, 0x4e,
	0x4, 0x69, 0x06, 0x03, 0x0, 0x06, 0x05, 0x0c, 0x10, 0x06, 0x04, 0x04,
	0x05, 0x04, 0x200, 0x03, 0x0a, 0x40, 0x2710, 0x01, 0x05, 0x05, 0x03,
	0x0c, 0x1e, 0x100, 0x0, 0x04, 0x0},
	{{0x220, 0x42, 0x0, 0x0}, 0x01, 0x60},
	{0x24716310} };

/*
 * [7:6]  bank(n:n bit bank)
 * [5:4]  row(13+n)
 * [3]    cs(0:1 cs, 1:2 cs)
 * [2:1]  bank(n:n bit bank)
 * [0]    col(10+n)
 */
const char ddr_cfg_2_rbc[] = {
	((3 << 6) | (3 << 4) | (0 << 3) | (0 << 1) | 1),
	((0 << 6) | (1 << 4) | (0 << 3) | (3 << 1) | 0),
	((0 << 6) | (2 << 4) | (0 << 3) | (3 << 1) | 0),
	((0 << 6) | (3 << 4) | (0 << 3) | (3 << 1) | 0),
	((0 << 6) | (1 << 4) | (0 << 3) | (3 << 1) | 1),
	((0 << 6) | (2 << 4) | (0 << 3) | (3 << 1) | 1),
	((0 << 6) | (3 << 4) | (0 << 3) | (3 << 1) | 1),
	((0 << 6) | (0 << 4) | (0 << 3) | (3 << 1) | 0),
	((0 << 6) | (0 << 4) | (0 << 3) | (3 << 1) | 1),
	((0 << 6) | (3 << 4) | (1 << 3) | (3 << 1) | 0),
	((0 << 6) | (3 << 4) | (1 << 3) | (3 << 1) | 1),
	((1 << 6) | (2 << 4) | (0 << 3) | (2 << 1) | 0),
	((3 << 6) | (2 << 4) | (0 << 3) | (0 << 1) | 1),
	((3 << 6) | (3 << 4) | (0 << 3) | (0 << 1) | 0),
};

/* DDRPHY REG */
enum {
	/* DDRPHY_REG1 */
	SOFT_RESET_MASK				= 3,
	SOFT_RESET_SHIFT			= 2,
	BYTE_LANE_ENABLE			= 3 << 4,

	/* DDRPHY_REG2 */
	MEMORY_SELECT_DDR3			= 0 << 6,
	DQS_SQU_CAL_NORMAL_MODE			= 0 << 1,
	DQS_SQU_CAL_START			= 1 << 0,
	DQS_SQU_NO_CAL				= 0 << 0,

	/* DDRPHY_REG2A */
	CMD_DLL_BYPASS				= 1 << 4,
	CMD_DLL_BYPASS_DISABLE			= 0 << 4,
	HIGH_8BIT_DLL_BYPASS			= 1 << 3,
	HIGH_8BIT_DLL_BYPASS_DISABLE		= 0 << 3,
	LOW_8BIT_DLL_BYPASS			= 1 << 2,
	LOW_8BIT_DLL_BYPASS_DISABLE		= 0 << 2,

	/* DDRPHY_REG19 */
	CMD_FEEDBACK_ENABLE			= 1 << 5,
	CMD_SLAVE_DLL_INVERSE_MODE		= 1 << 4,
	CMD_SLAVE_DLL_NO_INVERSE_MODE		= 0 << 4,
	CMD_SLAVE_DLL_ENALBE			= 1 << 3,
	CMD_TX_SLAVE_DLL_DELAY_MASK		= 7,
	CMD_TX_SLAVE_DLL_DELAY_SHIFT		= 0,

	/* DDRPHY_REG6 */
	LEFT_CHN_TX_DQ_PHASE_BYPASS_90		= 1 << 4,
	LEFT_CHN_TX_DQ_PHASE_BYPASS_0		= 0 << 4,
	LEFT_CHN_TX_DQ_DLL_ENABLE		= 1 << 3,
	LEFT_CHN_TX_DQ_DLL_DELAY_MASK		= 7,
	LEFT_CHN_TX_DQ_DLL_DELAY_SHIFT		= 0,

	/* DDRPHY_REG8 */
	LEFT_CHN_RX_DQS_DELAY_TAP_MASK		= 3,
	LEFT_CHN_RX_DQS_DELAY_TAP_SHIFT		= 0,

	/* DDRPHY_REG9 */
	RIGHT_CHN_TX_DQ_PHASE_BYPASS_90		= 1 << 4,
	RIGHT_CHN_TX_DQ_PHASE_BYPASS_0		= 0 << 4,
	RIGHT_CHN_TX_DQ_DLL_ENABLE		= 1 << 3,
	RIGHT_CHN_TX_DQ_DLL_DELAY_MASK		= 7,
	RIGHT_CHN_TX_DQ_DLL_DELAY_SHIFT		= 0,

	/* DDRPHY_REG11 */
	RIGHT_CHN_RX_DQS_DELAY_TAP_MASK		= 3,
	RIGHT_CHN_RX_DQS_DELAY_TAP_SHIFT	= 0,

	/* DDRPHY_REG62 */
	CAL_DONE_MASK				= 3,
	HIGH_8BIT_CAL_DONE			= 1 << 1,
	LOW_8BIT_CAL_DONE			= 1 << 0,
};

/* PTCL */
enum {
	/* PCTL_DFISTCFG0 */
	DFI_INIT_START			= 1 << 0,
	DFI_DATA_BYTE_DISABLE_EN	= 1 << 2,

	/* PCTL_DFISTCFG1 */
	DFI_DRAM_CLK_SR_EN		= 1 << 0,
	DFI_DRAM_CLK_DPD_EN		= 1 << 1,

	/* PCTL_DFISTCFG2 */
	DFI_PARITY_INTR_EN		= 1 << 0,
	DFI_PARITY_EN			= 1 << 1,

	/* PCTL_DFILPCFG0 */
	TLP_RESP_TIME_SHIFT		= 16,
	LP_SR_EN			= 1 << 8,
	LP_PD_EN			= 1 << 0,

	/* PCTL_DFIODTCFG */
	RANK0_ODT_WRITE_SEL		= 1 << 3,
	RANK1_ODT_WRITE_SEL		= 1 << 11,

	/* PCTL_DFIODTCFG1 */
	ODT_LEN_BL8_W_SHIFT		= 16,

	/* PCTL_MCFG */
	TFAW_CFG_MASK			= 3,
	TFAW_CFG_SHIFT			= 18,
	PD_EXIT_SLOW_MODE		= 0 << 17,
	PD_ACTIVE_POWER_DOWN		= 1 << 16,
	PD_IDLE_MASK			= 0xff,
	PD_IDLE_SHIFT			= 8,
	MEM_BL4				= 0 << 0,
	MEM_BL8				= 1 << 0,
	/* vendor blob writes 0x00050021 to 0x20004080; bit 5 undocumented */
	MCFG_VENDOR_BIT5		= 1 << 5,

	/* PCTL_MCFG1 */
	HW_EXIT_IDLE_EN_MASK		= 1,
	HW_EXIT_IDLE_EN_SHIFT		= 31,
	SR_IDLE_MASK			= 0x1ff,
	SR_IDLE_SHIFT			= 0,

	/* PCTL_SCFG */
	HW_LOW_POWER_EN			= 1 << 0,

	/* PCTL_POWCTL */
	POWER_UP_START			= 1 << 0,

	/* PCTL_POWSTAT */
	POWER_UP_DONE			= 1 << 0,

	/* PCTL_MCMD */
	START_CMD			= 1 << 31,
	BANK_ADDR_MASK			= 7,
	BANK_ADDR_SHIFT			= 17,
	CMD_ADDR_MASK			= 0x1fff,
	CMD_ADDR_SHIFT			= 4,
	DESELECT_CMD			= 0,
	PREA_CMD,
	REF_CMD,
	MRS_CMD,
	ZQCS_CMD,
	ZQCL_CMD,
	RSTL_CMD,
	MRR_CMD				= 8,

	/* PCTL_STAT */
	INIT_MEM			= 0,
	CONFIG,
	CFG_REQ,
	ACCESS,
	ACCESS_REQ,
	LOW_POWER,
	LOW_POWER_ENTRY_REQ,
	LOW_POWER_EXIT_REQ,
	PCTL_STAT_MASK			= 7,

	/* PCTL_SCTL */
	INIT_STATE			= 0,
	CFG_STATE			= 1,
	GO_STATE			= 2,
	SLEEP_STATE			= 3,
	WAKEUP_STATE			= 4,
};

/* GRF_SOC_CON2 */
#define	MSCH4_MAINDDR3		(1 << 7)
#define PHY_DRV_ODT_SET(n)	((n << 4) | n)
#define DDR3_DLL_RESET		(1 << 8)

/* CK pull up/down driver strength control */
enum {
	PHY_RON_DISABLE		= 0,
	PHY_RON_309OHM		= 1,
	PHY_RON_155OHM,
	PHY_RON_103OHM		= 3,
	PHY_RON_63OHM		= 5,
	PHY_RON_45OHM		= 7,
	PHY_RON_77OHM,
	PHY_RON_62OHM,
	PHY_RON_52OHM,
	PHY_RON_44OHM,
	PHY_RON_39OHM,
	PHY_RON_34OHM,
	PHY_RON_31OHM,
	PHY_RON_28OHM,
};

/* DQ pull up/down control */
enum {
	PHY_RTT_DISABLE		= 0,
	PHY_RTT_861OHM		= 1,
	PHY_RTT_431OHM,
	PHY_RTT_287OHM,
	PHY_RTT_216OHM,
	PHY_RTT_172OHM,
	PHY_RTT_145OHM,
	PHY_RTT_124OHM,
	PHY_RTT_215OHM,
	PHY_RTT_144OHM		= 0xa,
	PHY_RTT_123OHM,
	PHY_RTT_108OHM,
	PHY_RTT_96OHM,
	PHY_RTT_86OHM,
	PHY_RTT_78OHM,
};

/* DQS squelch DLL delay */
enum {
	DQS_DLL_NO_DELAY	= 0,
	DQS_DLL_22P5_DELAY,
	DQS_DLL_45_DELAY,
	DQS_DLL_67P5_DELAY,
	DQS_DLL_90_DELAY,
	DQS_DLL_112P5_DELAY,
	DQS_DLL_135_DELAY,
	DQS_DLL_157P5_DELAY,
};

/* GRF_OS_REG1 */
enum {
	/*
	 * 000: lpddr
	 * 001: ddr
	 * 010: ddr2
	 * 011: ddr3
	 * 100: lpddr2-s2
	 * 101: lpddr2-s4
	 * 110: lpddr3
	 */
	DDR_TYPE_MASK		= 7,
	DDR_TYPE_SHIFT		= 13,

	/* 0: 1 chn, 1: 2 chn */
	DDR_CHN_CNT_SHIFT	= 12,

	/* 0: 1 rank, 1: 2 rank */
	DDR_RANK_CNT_MASK	= 1,
	DDR_RANK_CNT_SHIFT	= 11,

	/*
	 * 00: 9col
	 * 01: 10col
	 * 10: 11col
	 * 11: 12col
	 */
	DDR_COL_MASK		= 3,
	DDR_COL_SHIFT		= 9,

	/* 0: 8 bank, 1: 4 bank*/
	DDR_BANK_MASK		= 1,
	DDR_BANK_SHIFT		= 8,

	/*
	 * 00: 13 row
	 * 01: 14 row
	 * 10: 15 row
	 * 11: 16 row
	 */
	DDR_CS0_ROW_MASK	= 3,
	DDR_CS0_ROW_SHIFT	= 6,
	DDR_CS1_ROW_MASK	= 3,
	DDR_CS1_ROW_SHIFT	= 4,

	/*
	 * 00: 32 bit
	 * 01: 16 bit
	 * 10: 8 bit
	 * rk3026 only support 16bit
	 */
	DDR_BW_MASK		= 3,
	DDR_BW_SHIFT		= 2,
	DDR_DIE_BW_MASK		= 3,
	DDR_DIE_BW_SHIFT	= 0,
};

static void rkdclk_init(struct rk3026_sdram_priv *priv)
{
	struct rk3026_pll *pll = &priv->cru->pll[1];
	int i;

	/* pll enter slow-mode */
	rk_clrsetreg(&priv->cru->cru_mode_con, DPLL_MODE_MASK,
		     DPLL_MODE_SLOW << DPLL_MODE_SHIFT);

	/* use integer mode */
	rk_setreg(&pll->con1, 1 << PLL_DSMPD_SHIFT);

	rk_clrsetreg(&pll->con0,
		     PLL_POSTDIV1_MASK | PLL_FBDIV_MASK,
		     (dpll_init_cfg.postdiv1 << PLL_POSTDIV1_SHIFT) |
			dpll_init_cfg.fbdiv);
	rk_clrsetreg(&pll->con1, PLL_POSTDIV2_MASK | PLL_REFDIV_MASK,
		     (dpll_init_cfg.postdiv2 << PLL_POSTDIV2_SHIFT |
		      dpll_init_cfg.refdiv << PLL_REFDIV_SHIFT));

	/*
	 * Wait for lock.  The blob exits when the bit is SET (lsr #10, lsl #31,
	 * bne) and gives up after 1000 tries rather than spinning forever; the
	 * inherited rk3036 loop had the sense inverted and was unbounded.
	 */
	for (i = 0; i < 1000; i++) {
		if (readl(&pll->con1) & (1 << PLL_LOCK_STATUS_SHIFT))
			break;
		udelay(1);
	}

	/* PLL enter normal-mode */
	rk_clrsetreg(&priv->cru->cru_mode_con, DPLL_MODE_MASK,
		     DPLL_MODE_NORM << DPLL_MODE_SHIFT);
}

static void copy_to_reg(u32 *dest, const u32 *src, u32 n)
{
	int i;

	for (i = 0; i < n / sizeof(u32); i++) {
		writel(*src, dest);
		src++;
		dest++;
	}
}

void phy_pctrl_reset(struct rk3026_sdram_priv *priv)
{
	struct rk3026_ddr_phy *ddr_phy = priv->phy;
	u32 saved;

	rk_clrsetreg(&priv->cru->cru_softrst_con[5], 1 << DDRCTRL_PSRST_SHIFT |
			1 << DDRCTRL_SRST_SHIFT | 1 << DDRPHY_PSRST_SHIFT |
			1 << DDRPHY_SRST_SHIFT,
			1 << DDRCTRL_PSRST_SHIFT | 1 << DDRCTRL_SRST_SHIFT |
			1 << DDRPHY_PSRST_SHIFT | 1 << DDRPHY_SRST_SHIFT);

	udelay(10);

	rk_clrreg(&priv->cru->cru_softrst_con[5], 1 << DDRPHY_PSRST_SHIFT |
						  1 << DDRPHY_SRST_SHIFT);
	udelay(10);

	rk_clrreg(&priv->cru->cru_softrst_con[5], 1 << DDRCTRL_PSRST_SHIFT |
						  1 << DDRCTRL_SRST_SHIFT);
	udelay(10);

	/*
	 * Pulse the PHY soft reset (0x2000a000 bits[3:2]) low and put back
	 * exactly what was there: the vendor blob saves and restores this
	 * word rather than forcing the bits to 3.
	 */
	saved = readl(&ddr_phy->ddrphy_reg1);
	writel(saved & ~(SOFT_RESET_MASK << SOFT_RESET_SHIFT),
	       &ddr_phy->ddrphy_reg1);
	udelay(10);
	writel(saved, &ddr_phy->ddrphy_reg1);

	udelay(1);
}

void phy_dll_bypass_set(struct rk3026_sdram_priv *priv, unsigned int freq)
{
	struct rk3026_ddr_phy *ddr_phy = priv->phy;

	if (freq < ddr_timing.freq) {
		writel(CMD_DLL_BYPASS | HIGH_8BIT_DLL_BYPASS |
			LOW_8BIT_DLL_BYPASS, &ddr_phy->ddrphy_reg2a);

		writel(LEFT_CHN_TX_DQ_PHASE_BYPASS_90 |
			LEFT_CHN_TX_DQ_DLL_ENABLE |
			(0 & LEFT_CHN_TX_DQ_DLL_DELAY_MASK) <<
			 LEFT_CHN_TX_DQ_DLL_DELAY_SHIFT, &ddr_phy->ddrphy_reg6);

		writel(RIGHT_CHN_TX_DQ_PHASE_BYPASS_90 |
			RIGHT_CHN_TX_DQ_DLL_ENABLE |
			(0 & RIGHT_CHN_TX_DQ_DLL_DELAY_MASK) <<
			 RIGHT_CHN_TX_DQ_DLL_DELAY_SHIFT,
			&ddr_phy->ddrphy_reg9);
	} else {
		writel(CMD_DLL_BYPASS_DISABLE | HIGH_8BIT_DLL_BYPASS_DISABLE |
			LOW_8BIT_DLL_BYPASS_DISABLE, &ddr_phy->ddrphy_reg2a);

		writel(LEFT_CHN_TX_DQ_PHASE_BYPASS_0 |
			LEFT_CHN_TX_DQ_DLL_ENABLE |
			(4 & LEFT_CHN_TX_DQ_DLL_DELAY_MASK) <<
			 LEFT_CHN_TX_DQ_DLL_DELAY_SHIFT,
			&ddr_phy->ddrphy_reg6);

		writel(RIGHT_CHN_TX_DQ_PHASE_BYPASS_0 |
			RIGHT_CHN_TX_DQ_DLL_ENABLE |
			(4 & RIGHT_CHN_TX_DQ_DLL_DELAY_MASK) <<
			 RIGHT_CHN_TX_DQ_DLL_DELAY_SHIFT,
			&ddr_phy->ddrphy_reg9);
	}

	writel(CMD_SLAVE_DLL_NO_INVERSE_MODE | CMD_SLAVE_DLL_ENALBE |
			(0 & CMD_TX_SLAVE_DLL_DELAY_MASK) <<
			CMD_TX_SLAVE_DLL_DELAY_SHIFT, &ddr_phy->ddrphy_reg19);

	/*
	 * The RX DQS delay taps at 0x2000a0e0 and 0x2000a120 are inherited
	 * from rk3036; the vendor blob never writes either, so leave them at
	 * reset.
	 */
}

static void send_command(struct rk3026_ddr_pctl *pctl,
			 u32 rank, u32 cmd, u32 arg)
{
	writel((START_CMD | (rank << 20) | arg | cmd), &pctl->mcmd);
	udelay(1);
	while (readl(&pctl->mcmd) & START_CMD)
		;
}

static void memory_init(struct rk3026_sdram_priv *priv)
{
	struct rk3026_ddr_pctl *pctl = priv->pctl;

	send_command(pctl, 3, DESELECT_CMD, 0);
	udelay(1);
	send_command(pctl, 3, PREA_CMD, 0);
	send_command(pctl, 3, MRS_CMD,
		     (0x02 & BANK_ADDR_MASK) << BANK_ADDR_SHIFT |
		     (ddr_timing.phy_timing.mr[2] & CMD_ADDR_MASK) <<
		     CMD_ADDR_SHIFT);

	send_command(pctl, 3, MRS_CMD,
		     (0x03 & BANK_ADDR_MASK) << BANK_ADDR_SHIFT |
		     (ddr_timing.phy_timing.mr[3] & CMD_ADDR_MASK) <<
		     CMD_ADDR_SHIFT);

	send_command(pctl, 3, MRS_CMD,
		     (0x01 & BANK_ADDR_MASK) << BANK_ADDR_SHIFT |
		     (ddr_timing.phy_timing.mr[1] & CMD_ADDR_MASK) <<
		     CMD_ADDR_SHIFT);

	/*
	 * DLL reset is MR0 bit 8, so it must be OR'd into the mode-register
	 * value before the shift into the command word.  The vendor blob does
	 * ubfx / orr #0x100 / lsl #4 in that order; OR-ing after the shift
	 * lands the bit on A4 instead, which never resets the DLL and raises
	 * MR0 CL to 7 while the controller is programmed for 6.
	 */
	send_command(pctl, 3, MRS_CMD,
		     (0x00 & BANK_ADDR_MASK) << BANK_ADDR_SHIFT |
		     ((ddr_timing.phy_timing.mr[0] | DDR3_DLL_RESET) &
		      CMD_ADDR_MASK) << CMD_ADDR_SHIFT);

	send_command(pctl, 3, ZQCL_CMD, 0);
}

static void data_training(struct rk3026_sdram_priv *priv)
{
	struct rk3026_ddr_phy *ddr_phy = priv->phy;
	struct rk3026_ddr_pctl *pctl = priv->pctl;
	u32 value;
	u32 refresh, rank_mask;

	/* disable auto refresh */
	value = readl(&pctl->trefi),
	writel(0, &pctl->trefi);

	clrsetbits_le32(&ddr_phy->ddrphy_reg2, 0x03,
			DQS_SQU_CAL_NORMAL_MODE | DQS_SQU_CAL_START);

	/* Blob waits 6 us between starting calibration and polling. */
	udelay(6);
	while ((readl(&ddr_phy->ddrphy_reg62) & CAL_DONE_MASK) !=
		(HIGH_8BIT_CAL_DONE | LOW_8BIT_CAL_DONE)) {
		;
	}

	clrsetbits_le32(&ddr_phy->ddrphy_reg2, 0x03,
			DQS_SQU_CAL_NORMAL_MODE | DQS_SQU_NO_CAL);

	/*
	 * Catch up the refreshes lost while auto-refresh was off: the vendor
	 * blob issues 2 with rank mask 1 for one rank, 4 with mask 3 for two.
	 */
	rank_mask = (priv->ddr_config.rank > 1) ? 3 : 1;
	refresh = (priv->ddr_config.rank > 1) ? 4 : 2;
	while (refresh--)
		send_command(pctl, rank_mask, REF_CMD, 0);

	writel(value, &pctl->trefi);
}

static void move_to_config_state(struct rk3026_sdram_priv *priv)
{
	unsigned int state;
	struct rk3026_ddr_pctl *pctl = priv->pctl;

	while (1) {
		state = readl(&pctl->stat) & PCTL_STAT_MASK;
		switch (state) {
		case LOW_POWER:
			writel(WAKEUP_STATE, &pctl->sctl);
			while ((readl(&pctl->stat) & PCTL_STAT_MASK)
				!= ACCESS)
				;
			/*
			 * If at low power state, need wakeup first, and then
			 * enter the config, so fallthrough
			 */
		case ACCESS:
			/* fallthrough */
		case INIT_MEM:
			writel(CFG_STATE, &pctl->sctl);
			while ((readl(&pctl->stat) & PCTL_STAT_MASK) != CONFIG)
				;
			break;
		case CONFIG:
			return;
		default:
			break;
		}
	}
}

static void move_to_access_state(struct rk3026_sdram_priv *priv)
{
	unsigned int state;
	struct rk3026_ddr_pctl *pctl = priv->pctl;

	while (1) {
		state = readl(&pctl->stat) & PCTL_STAT_MASK;
		switch (state) {
		case LOW_POWER:
			writel(WAKEUP_STATE, &pctl->sctl);
			while ((readl(&pctl->stat) & PCTL_STAT_MASK) != ACCESS)
				;
			break;
		case INIT_MEM:
			writel(CFG_STATE, &pctl->sctl);
			while ((readl(&pctl->stat) & PCTL_STAT_MASK) != CONFIG)
				;
			/* fallthrough */
		case CONFIG:
			writel(GO_STATE, &pctl->sctl);
			while ((readl(&pctl->stat) & PCTL_STAT_MASK) != ACCESS)
				;
			break;
		case ACCESS:
			return;
		default:
			break;
		}
	}
}

static void pctl_cfg(struct rk3026_sdram_priv *priv)
{
	struct rk3026_ddr_pctl *pctl = priv->pctl;
	u32 burst_len;
	u32 reg;

	writel(DFI_INIT_START | DFI_DATA_BYTE_DISABLE_EN, &pctl->dfistcfg0);
	writel(DFI_DRAM_CLK_SR_EN | DFI_DRAM_CLK_DPD_EN, &pctl->dfistcfg1);
	writel(DFI_PARITY_INTR_EN | DFI_PARITY_EN, &pctl->dfistcfg2);
	writel(7 << TLP_RESP_TIME_SHIFT | LP_SR_EN | LP_PD_EN,
	       &pctl->dfilpcfg0);

	writel(1, &pctl->dfitphyupdtype0);
	writel(0x0d, &pctl->dfitphyrdlat);

	/* cs0 and cs1 write odt enable */
	writel((RANK0_ODT_WRITE_SEL | RANK1_ODT_WRITE_SEL),
	       &pctl->dfiodtcfg);

	/* odt write length */
	writel(7 << ODT_LEN_BL8_W_SHIFT, &pctl->dfiodtcfg1);

	/* phyupd and ctrlupd disabled */
	writel(0, &pctl->dfiupdcfg);

	if ((ddr_timing.noc_timing.burstlen << 1) == 4)
		burst_len = MEM_BL4;
	else
		burst_len = MEM_BL8;

	copy_to_reg(&pctl->togcnt1u, &ddr_timing.pctl_timing.togcnt1u,
		    sizeof(struct rk3026_pctl_timing));
	reg = readl(&pctl->tcl);
	writel(reg - 3, &pctl->dfitrddataen);
	reg = readl(&pctl->tcwl);
	writel(reg - 1, &pctl->dfitphywrlat);

	writel(burst_len | MCFG_VENDOR_BIT5 |
			(1 & TFAW_CFG_MASK) << TFAW_CFG_SHIFT |
			PD_EXIT_SLOW_MODE | PD_ACTIVE_POWER_DOWN |
			(0 & PD_IDLE_MASK) << PD_IDLE_SHIFT,
			&pctl->mcfg);

	writel(RK_SETBITS(MSCH4_MAINDDR3), (void *)GRF_SOC_CON2_ADDR);
	setbits_le32(&pctl->scfg, HW_LOW_POWER_EN);
}

static void phy_cfg(struct rk3026_sdram_priv *priv)
{
	struct rk3026_ddr_phy *ddr_phy = priv->phy;
	struct rk3026_service_sys *axi_bus = priv->axi_bus;

	writel(ddr_timing.noc_timing.noc_timing, &axi_bus->ddrtiming);
	writel(0x3f, &axi_bus->readlatency);

	/* enable both byte lanes (0x2000a000 |= 0x30) before power-up */
	setbits_le32(&ddr_phy->ddrphy_reg1, BYTE_LANE_ENABLE);

	writel(MEMORY_SELECT_DDR3 | DQS_SQU_CAL_NORMAL_MODE,
	       &ddr_phy->ddrphy_reg2);

	clrsetbits_le32(&ddr_phy->ddrphy_reg3, 1, ddr_timing.phy_timing.bl);
	writel(ddr_timing.phy_timing.cl_al, &ddr_phy->ddrphy_reg4a);
	/*
	 * The CMD/CK drive registers at 0x2000a054 and 0x2000a070 are
	 * inherited from rk3036; the vendor blob never writes either, so
	 * leave them at reset.
	 */
	/*
	 * DQ drive: the vendor blob writes 0xbb to 0x2000a080/0x2000a084.
	 * Raw nibble, not an ohm value - the nibble-to-ohm table is only in
	 * third-party source and is not corroborated by any binary.
	 */
	writel(PHY_DRV_ODT_SET(0xb), &ddr_phy->ddrphy_reg25);
	writel(PHY_DRV_ODT_SET(0xb), &ddr_phy->ddrphy_reg26);
	writel(PHY_DRV_ODT_SET(PHY_RTT_216OHM), &ddr_phy->ddrphy_reg27);
	writel(PHY_DRV_ODT_SET(PHY_RTT_216OHM), &ddr_phy->ddrphy_reg28);
}

void dram_cfg_rbc(struct rk3026_sdram_priv *priv)
{
	char noc_config;
	int i = 0;
	struct rk3026_ddr_config config = priv->ddr_config;
	struct rk3026_service_sys *axi_bus = priv->axi_bus;

	move_to_config_state(priv);

	/* 2bit in BIT1, 2 */
	if (config.rank == 2) {
		noc_config = (config.cs0_row - 13) << 4 | config.bank << 1 |
			      1 << 3 | (config.col - 10);
		if (noc_config == ddr_cfg_2_rbc[9]) {
			i = 9;
			goto finish;
		} else if (noc_config == ddr_cfg_2_rbc[10]) {
			i = 10;
			goto finish;
		}
	}

	noc_config = (config.cs0_row - 13) << 4 | config.bank << 1 |
			(config.col - 10);

	for (i = 0; i < sizeof(ddr_cfg_2_rbc); i++) {
		if (noc_config == ddr_cfg_2_rbc[i])
			goto finish;
	}

	/* bank: 1 bit in BIT6,7, 1bit in BIT1, 2 */
	noc_config = 1 << 6 | (config.cs0_row - 13) << 4 |
			2 << 1 | (config.col - 10);
	if (noc_config == ddr_cfg_2_rbc[11]) {
		i = 11;
		goto finish;
	}

	/* bank: 2bit in BIT6,7 */
	noc_config = (config.bank << 6) | (config.cs0_row - 13) << 4 |
			(config.col - 10);

	if (noc_config == ddr_cfg_2_rbc[0])
		i = 0;
	else if (noc_config == ddr_cfg_2_rbc[12])
		i = 12;
	else if (noc_config == ddr_cfg_2_rbc[13])
		i = 13;
finish:
	writel(i, &axi_bus->ddrconf);
	move_to_access_state(priv);
}

static void sdram_all_config(struct rk3026_sdram_priv *priv)
{
	u32 os_reg = 0;
	u32 cs1_row = 0;
	struct rk3026_ddr_config config = priv->ddr_config;

	if (config.rank > 1)
		cs1_row = config.cs1_row - 13;

	os_reg = config.ddr_type << DDR_TYPE_SHIFT |
			0 << DDR_CHN_CNT_SHIFT |
			(config.rank - 1) << DDR_RANK_CNT_SHIFT |
			(config.col - 9) << DDR_COL_SHIFT |
			(config.bank == 3 ? 0 : 1) << DDR_BANK_SHIFT |
			(config.cs0_row - 13) << DDR_CS0_ROW_SHIFT |
			cs1_row << DDR_CS1_ROW_SHIFT |
			(2 >> config.bw) << DDR_DIE_BW_SHIFT;
	/* DDR_BW bits[3:2] stay 0 — the vendor blob never writes them */
	writel(os_reg, (void *)GRF_OS_REG1);
}

size_t sdram_size(void)
{
	u32 size, os_reg, cs0_row, cs1_row, col, bank, rank;

	os_reg = readl((void *)GRF_OS_REG1);

	cs0_row = 13 + ((os_reg >> DDR_CS0_ROW_SHIFT) & DDR_CS0_ROW_MASK);
	cs1_row = 13 + ((os_reg >> DDR_CS1_ROW_SHIFT) & DDR_CS1_ROW_MASK);
	col = 9 + ((os_reg >> DDR_COL_SHIFT) & DDR_COL_MASK);
	bank = 3 - ((os_reg >> DDR_BANK_SHIFT) & DDR_BANK_MASK);
	rank = 1 + ((os_reg >> DDR_RANK_CNT_SHIFT) & DDR_RANK_CNT_MASK);

	/* row + col + bank + bw(rk3026 only support 16bit, so fix in 1) */
	size = 1 << (cs0_row + col + bank + 1);

	if (rank > 1)
		size += size >> (cs0_row - cs1_row);

	return size;
}

void sdram_init(void)
{
	struct rk3026_sdram_priv sdram_priv;

	sdram_priv.cru = (void *)CRU_BASE;
	sdram_priv.grf = (void *)GRF_BASE;
	sdram_priv.phy = (void *)DDR_PHY_BASE;
	sdram_priv.pctl = (void *)DDR_PCTL_BASE;
	sdram_priv.axi_bus = (void *)CPU_AXI_BUS_BASE;

	get_ddr_config(&sdram_priv.ddr_config);
	sdram_all_config(&sdram_priv);
	rkdclk_init(&sdram_priv);
	phy_pctrl_reset(&sdram_priv);
	phy_dll_bypass_set(&sdram_priv, ddr_timing.freq);
	pctl_cfg(&sdram_priv);
	phy_cfg(&sdram_priv);
	writel(POWER_UP_START, &sdram_priv.pctl->powctl);
	while (!(readl(&sdram_priv.pctl->powstat) & POWER_UP_DONE))
		;
	memory_init(&sdram_priv);
	move_to_config_state(&sdram_priv);
	/* Blob order: DQS calibration runs in ACCESS state, not CONFIG. */
	move_to_access_state(&sdram_priv);
	data_training(&sdram_priv);
	dram_cfg_rbc(&sdram_priv);
}
