// SPDX-License-Identifier: GPL-2.0+
/*
 * RK3026 SoC platform support
 *
 * Copyright (c) 2024, Mario Rugiero
 *
 * Based on rk3066.c / rk3036.c by the Rockchip U-Boot maintainers.
 *
 * The Rockchip boot ROM loads the SPL at eMMC sector 0x40 (or SD sector 0),
 * runs the DDR init code (ddrbin) via the back-to-BROM flow, then re-enters
 * the SPL which loads U-Boot proper.  No explicit DDR init lives here.
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
	 * Route GPIO1B[3:2] to UART2 (FUNC2) so the SPL/early console has
	 * output before pinctrl is up.  GRF_GPIO1B_IOMUX is hiword-masked.
	 */
	writel(IOMUX_UART2, (void __iomem *)(GRF_BASE + GRF_GPIO1B_IOMUX));
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
