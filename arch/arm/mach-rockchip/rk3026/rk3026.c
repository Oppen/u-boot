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

void board_debug_uart_init(void)
{
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
