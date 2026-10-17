// SPDX-License-Identifier: GPL-2.0+
/*
 * ONYX BOOX Robinson Crusoe 2 (MC_Kepler_R2), RK3026.
 *
 * The SPL (spl.c) sets the power hold and brings up DDR, then the mask ROM
 * loads U-Boot proper from the microSD card.
 *
 * The OTG PHY bring-up and the gadget hand-off are the C67ML's
 * (board/rockchip/boox-c67ml/boox-c67ml.c), same SoC.
 */

#include <fdtdec.h>
#include <init.h>
#include <iomux.h>
#include <stdio.h>
#include <asm/global_data.h>
#include <asm/io.h>
#include <asm/arch-rockchip/hardware.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include "boox-rc2.h"

DECLARE_GLOBAL_DATA_PTR;

#define CRU_CLKGATE_CON1	(CRU_BASE + 0xd4)
#define CRU_CLKGATE_CON5	(CRU_BASE + 0xe4)
#define CRU_SOFTRST_CON4	(CRU_BASE + 0x120)

#define GRF_SOC_CON0		(GRF_BASE + 0x140)
#define GRF_UOC1_CON4		(GRF_BASE + 0x190)

/* E Ink waveform, ramoops and a loader log page live above this */
#define RC2_RAM_TOP		0x7ff00000

int board_early_init_f(void)
{
	/*
	 * IMEM remap off (GRF_SOC_CON0 bit 12), in case an earlier stage left
	 * it set: with it set, a reset restarts CPU0 in IMEM instead of the
	 * BootROM. A kernel that sets it to start CPU1 must clear it in its
	 * own restart path; this cannot repair that.
	 */
	writel(RK_CLRBITS(BIT(12)), (void __iomem *)GRF_SOC_CON0);

	rc2_power_hold();
	return 0;
}

int dram_init(void)
{
	return fdtdec_setup_mem_size_base();
}

int dram_init_banksize(void)
{
	return fdtdec_setup_memory_banksize();
}

phys_addr_t board_get_usable_ram_top(phys_size_t total_size)
{
	return min_t(phys_addr_t, gd->ram_top, RC2_RAM_TOP);
}

struct dwc2_udc;

#define GATE_HCLK_OTG0		BIT(29)		/* CLKGATE_CON5 bit 13 */
#define GATE_OTGPHY0		BIT(21)		/* CLKGATE_CON1 bit 5 */
/* UOC1_CON4 bits 10, 12 and 13; bits 12:13 are the OTG D+/D- mux */
#define UOC1_CON4_OTG_BITS	(BIT(10) | BIT(12) | BIT(13))
/* SOFTRST_CON4: the USBOTG0, USBPHY0 and OTGC0 resets */
#define SOFTRST_OTG0_BITS	GENMASK(7, 5)

/* Same sequence as boox-c67ml.c. */
void otg_phy_init(struct dwc2_udc *dev)
{
	writel(GATE_HCLK_OTG0, (void __iomem *)CRU_CLKGATE_CON5);
	writel(GATE_OTGPHY0, (void __iomem *)CRU_CLKGATE_CON1);
	writel(RK_CLRBITS(UOC1_CON4_OTG_BITS), (void __iomem *)GRF_UOC1_CON4);
	writel(RK_SETBITS(SOFTRST_OTG0_BITS), (void __iomem *)CRU_SOFTRST_CON4);
	udelay(3000);
	writel(RK_CLRBITS(SOFTRST_OTG0_BITS), (void __iomem *)CRU_SOFTRST_CON4);
	udelay(3000);
}

void otg_phy_off(struct dwc2_udc *dev)
{
	writel(RK_SETBITS(SOFTRST_OTG0_BITS), (void __iomem *)CRU_SOFTRST_CON4);
	udelay(3000);
	writel(RK_CLRBITS(SOFTRST_OTG0_BITS), (void __iomem *)CRU_SOFTRST_CON4);
}

/* bootm, right before the kernel jump: release the ACM gadget */
void board_quiesce_devices(void)
{
	iomux_doenv(stdin, "serial");
	iomux_doenv(stdout, "serial");
	iomux_doenv(stderr, "serial");
}
