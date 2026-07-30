// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) 2026, Mario Rugiero
 *
 * Binds the GRF as a syscon so drivers can reach it by function rather than by
 * a hardcoded address. The DTS already advertises it (`compatible =
 * "rockchip,rk3026-grf", "syscon", "simple-mfd"`); without this driver
 * syscon_get_first_range(ROCKCHIP_SYSCON_GRF) has nothing to match.
 *
 * pinctrl does not need this -- it takes its own `rockchip,grf` phandle -- but
 * the i2c controller select does (docs/HARDWARE-CONSTANTS.md, GRF_SOC_CON1).
 */

#include <dm.h>
#include <syscon.h>
#include <asm/arch-rockchip/clock.h>

static const struct udevice_id rk3026_syscon_ids[] = {
	{ .compatible = "rockchip,rk3026-grf", .data = ROCKCHIP_SYSCON_GRF },
	{ }
};

U_BOOT_DRIVER(syscon_rk3026) = {
	.name = "rk3026_syscon",
	.id = UCLASS_SYSCON,
	.of_match = rk3026_syscon_ids,
	.bind = dm_scan_fdt_dev,
};
