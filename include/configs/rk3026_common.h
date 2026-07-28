/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * RK3026 SoC common U-Boot config
 *
 * Copyright (c) 2024, Mario Rugiero
 *
 * RK3026 shares the RK3036 memory map: DRAM at 0x60000000, 24 MHz timer.
 */
#ifndef __CONFIG_RK3026_COMMON_H
#define __CONFIG_RK3026_COMMON_H

#include "rockchip-common.h"

/* Internal SRAM base — same as the rest of the RK292x/A9 family (rk3066,
 * rk3128, rk322x).  The BootROM leaves the boot-source id at +0x10 here. */
#define CFG_IRAM_BASE			0x10080000

#define CFG_SYS_HZ_CLOCK		24000000

#define CFG_SYS_SDRAM_BASE		0x60000000
#define SDRAM_BANK_SIZE			(512UL << 20UL)
#define SDRAM_MAX_SIZE			(CONFIG_NR_DRAM_BANKS * SDRAM_BANK_SIZE)

#define ENV_MEM_LAYOUT_SETTINGS \
	"scriptaddr=0x60000000\0" \
	"pxefile_addr_r=0x60100000\0" \
	"fdt_addr_r=0x61f00000\0" \
	"kernel_addr_r=0x62000000\0" \
	"ramdisk_addr_r=0x64000000\0"

/*
 * stdout/stderr/stdin are muxed onto both the UART and the USB CDC-ACM gadget.
 * The UART is unreachable on this board without soldering to the LQFP176 leads,
 * so in practice the ACM device -- /dev/cu.usbmodem* on the host -- is the
 * console. CONFIG_PRE_CONSOLE_BUFFER holds everything printed before USB
 * enumerates and replays it once the port opens, so nothing early is lost.
 */
#define CFG_EXTRA_ENV_SETTINGS \
	"stdout=serial,usbacm\0" \
	"stderr=serial,usbacm\0" \
	"stdin=serial,usbacm\0" \
	"fdt_file=" CONFIG_DEFAULT_FDT_FILE "\0" \
	"fdt_high=0x7fffffff\0" \
	"partitions=" PARTS_DEFAULT \
	ENV_MEM_LAYOUT_SETTINGS \
	"boot_targets=" BOOT_TARGETS "\0"

#endif
