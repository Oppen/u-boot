// SPDX-License-Identifier: GPL-2.0+
/*
 * RK3026 SoC platform support
 *
 * Copyright (c) 2024, Mario Rugiero
 *
 * Based on rk3066.c / rk3036.c by the Rockchip U-Boot maintainers.
 *
 * The Rockchip boot ROM loads the SPL from LBA 64 of the boot medium (SD or
 * eMMC alike), the SPL initialises DRAM (sdram_rk3026.c) and returns to the
 * ROM, which then loads U-Boot proper into DRAM.
 */

#include <dm.h>
#include <syscon.h>
#include <asm/io.h>
#include <asm/arch-rockchip/bootrom.h>
#include <asm/arch-rockchip/clock.h>
#include <asm/arch-rockchip/cru_rk3026.h>
#include <asm/arch-rockchip/grf_rk3026.h>
#include <asm/arch-rockchip/hardware.h>
#include <asm/arch-rockchip/qos_rk3026.h>
#include <linux/err.h>

#define GRF_BASE	0x20008000

/*
 * Boot source nodes, indexed by the BROM boot-source code the mask ROM
 * leaves behind.  eMMC and SD share the dwmmc controller addresses from
 * the RK3026 DTS (rk3026.dtsi).
 */
const char * const boot_devices[BROM_LAST_BOOTSOURCE + 1] = {
	[BROM_BOOTSOURCE_EMMC] = "/mmc@1021c000",
	[BROM_BOOTSOURCE_SD] = "/mmc@10214000",
};

/*
 * Step 11 probe (-DUBOOT_CURRENT_STEP=11 via KCFLAGS), U-Boot proper only.
 *
 * This is the earliest board-owned C in U-Boot proper -- debug_uart_init() runs
 * near the top of board_init_f, well before board_early_init_f (step 6), which
 * only happens after fdtdec_setup() and arch_cpu_init(). If 11 blinks and 6 does
 * not, U-Boot proper got control from the ROM and died in between.
 *
 * Blinks forever off a raw spin: no timer, no mdelay(), and a positive signal so
 * it cannot be confused with a hang. Kept out of the SPL, which calls this same
 * function.
 */
#if defined(UBOOT_CURRENT_STEP) && !defined(CONFIG_XPL_BUILD)
#define STEP11_GPIO2_BASE	0x20084000
#define STEP11_LED_BIT		BIT(15)
#define STEP11_CRU_GATE8	0x200000f0
#define STEP11_SPIN		100000

static void step11_blink_forever(void)
{
	volatile u32 i;

	writel(BIT(11 + 16), (void __iomem *)STEP11_CRU_GATE8);
	setbits_le32((void __iomem *)(STEP11_GPIO2_BASE + 0x04), STEP11_LED_BIT);

	while (1) {
		setbits_le32((void __iomem *)STEP11_GPIO2_BASE, STEP11_LED_BIT);
		for (i = 0; i < STEP11_SPIN; i++)
			;
		clrbits_le32((void __iomem *)STEP11_GPIO2_BASE, STEP11_LED_BIT);
		for (i = 0; i < STEP11_SPIN; i++)
			;
	}
}
#endif

#ifndef CONFIG_XPL_BUILD
/*
 * After a power-on the CPU-side QoS priorities read CPU0 0x8, CPU1W/CPU1R
 * 0xf and PERI 0x4: the CPU cluster's DRAM traffic then outranks the
 * display fetch, and the EBC (E-Ink) controller underruns under CPU memory
 * load. The vendor kernel's cpu_axi_init() sets these four ports to the
 * lowest priority; a warm reset keeps whatever ran before, a power-on does
 * not, so set them on every boot. The QoS blocks from 0x1012d000 up (GPU,
 * VPU, VIO: LCDC/EBC) can be clock- or power-gated (reads hung under the
 * vendor kernel) and are left to the kernel.
 */
static void rk3026_qos_init(void)
{
	writel(0, (void __iomem *)(CPU0_QOS + CPU_AXI_QOS_PRIORITY));
	writel(0, (void __iomem *)(CPU1W_QOS + CPU_AXI_QOS_PRIORITY));
	writel(0, (void __iomem *)(CPU1R_QOS + CPU_AXI_QOS_PRIORITY));
	writel(0, (void __iomem *)(PERI_QOS + CPU_AXI_QOS_PRIORITY));
}

int arch_cpu_init(void)
{
	rk3026_qos_init();

	return 0;
}
#endif

void board_debug_uart_init(void)
{
#if defined(UBOOT_CURRENT_STEP) && !defined(CONFIG_XPL_BUILD)
	if (UBOOT_CURRENT_STEP == 11)
		step11_blink_forever();
#endif

	/*
	 * Route GPIO2_C6/C7 to UART2_SIN/SOUT (FUNC3) so the SPL has output
	 * before pinctrl is up.  GRF_GPIO2C_IOMUX is hiword-masked.
	 */
	writel(IOMUX_UART2, (void __iomem *)(GRF_BASE + GRF_GPIO2C_IOMUX));
}

int rockchip_get_clk(struct udevice **devp)
{
	return uclass_get_device_by_driver(UCLASS_CLK,
			DM_DRIVER_GET(rockchip_rk3026_cru), devp);
}

void *rockchip_get_cru(void)
{
	struct rk3026_clk_priv *priv;
	struct udevice *dev;
	int ret;

	ret = rockchip_get_clk(&dev);
	if (ret)
		return ERR_PTR(ret);

	priv = dev_get_priv(dev);

	return priv->cru;
}
