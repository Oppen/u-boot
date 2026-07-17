// SPDX-License-Identifier: GPL-2.0+
/*
 * RK3026 SPL board init (back-to-BROM)
 *
 * Copyright (c) 2024, Mario Rugiero
 *
 * Modeled on rk3036-board-spl.c.  The Rockchip mask ROM loads the SPL into
 * internal SRAM, the SPL initialises the debug UART + timers and returns to
 * the ROM (back-to-BROM), which then loads U-Boot proper.  DDR is trained by
 * the ROM's ddrbin before SPL re-entry, so there is no sdram_init() here.
 */

#include <debug_uart.h>
#include <init.h>
#include <asm/arch-rockchip/bootrom.h>
#include <asm/arch-rockchip/timer.h>

void board_init_f(ulong dummy)
{
#ifdef CONFIG_DEBUG_UART
	debug_uart_init();
#endif

	/* Secure timer + ARM arch timer (arch/arm/cpu/armv7/arch_timer.c) */
	rockchip_stimer_init();
	timer_init();

	/* DDR already initialised by the ROM ddrbin — return to the mask ROM. */
	back_to_bootrom(BROM_BOOT_NEXTSTAGE);
}

/* Placeholder: board_init_r never runs (back_to_bootrom does not return). */
void board_init_r(gd_t *id, ulong dest_addr)
{
	while (1)
		;
}
