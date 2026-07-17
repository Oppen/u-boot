// SPDX-License-Identifier: GPL-2.0+
/*
 * ONYX BOOX C67ML Magellan board support
 *
 * Copyright (c) 2024, Mario Rugiero
 *
 * Hardware: RK3026 dual-core Cortex-A9, 512 MB DDR3,
 *           4 GB eMMC, microSD, 6" E Ink display (EBC + TPS65185),
 *           RTL8188EU USB WiFi, ACT8931 PMIC.
 *
 * DRAM size/base come from the device tree (the mask ROM has already
 * trained DDR via the back-to-BROM flow before U-Boot proper runs).
 */

#include <dm.h>
#include <env.h>
#include <fdtdec.h>
#include <init.h>
#include <asm/global_data.h>

DECLARE_GLOBAL_DATA_PTR;

/* board_init() is provided by the shared arch/arm/mach-rockchip/board.c */

int dram_init(void)
{
	return fdtdec_setup_mem_size_base();
}

int dram_init_banksize(void)
{
	return fdtdec_setup_memory_banksize();
}

#ifdef CONFIG_MISC_INIT_R
int misc_init_r(void)
{
	env_set("board", "boox-c67ml");
	return 0;
}
#endif
