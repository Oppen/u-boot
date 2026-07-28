// SPDX-License-Identifier: GPL-2.0+
/*
 * RK3026 SPL board init (back-to-BROM)
 *
 * Copyright (c) 2024, Mario Rugiero
 *
 * Modeled on rk3036-board-spl.c.  The mask ROM loads the SPL into internal
 * SRAM; the SPL brings up the debug UART and DRAM, then returns to the ROM,
 * which loads U-Boot proper *into DRAM* (see tools/rkcommon.c) — so DRAM must
 * be up before back_to_bootrom().
 *
 * RK3026 is Cortex-A9: there is no ARM architected generic timer, so neither
 * the CNTFRQ write in the weak rockchip_stimer_init() nor timer_init() from
 * armv7/arch_timer.c can be used.  This file overrides the former to start the
 * Rockchip counter; everything else -- timer_read_counter(), get_ticks(),
 * __udelay() -- comes from lib/time.c's pre-driver-model path, configured by
 * CFG_SYS_TIMER_COUNTER / CFG_SYS_TIMER_RATE / CONFIG_SYS_TIMER_COUNTS_DOWN.
 */

#include <debug_uart.h>
#include <init.h>
#include <asm/io.h>
#include <linux/delay.h>
#include <linux/bitops.h>
#include <linux/types.h>
#include <asm/arch-rockchip/bootrom.h>
#include <asm/arch-rockchip/hardware.h>
#include <asm/arch-rockchip/sdram_rk3026.h>
#include <asm/arch-rockchip/timer.h>

/*
 * Timebase.
 *
 * lib/time.c does all the work once CFG_SYS_TIMER_COUNTER, CFG_SYS_TIMER_RATE
 * and CONFIG_SYS_TIMER_COUNTS_DOWN are set (see include/configs/boox_c67ml.h):
 * it provides timer_read_counter(), a get_ticks() that carries 32-bit rollover
 * into 64 bits, and a __udelay() whose usec_to_tick() is 64-bit and whose
 * "tmp + 1" floor means a sub-tick delay still waits one tick.
 *
 * All this file has to do is start the counter -- and not use the weak
 * rockchip_stimer_init(), which writes CNTFRQ (mcr p15,0,c14,c0,0), undefined
 * on Cortex-A9.
 */
#define STIMER		((struct rk_timer *)CONFIG_ROCKCHIP_STIMER_BASE)

void rockchip_stimer_init(void)
{
	if (readl(&STIMER->timer_ctrl_reg) & 1)
		return;

	writel(0, &STIMER->timer_ctrl_reg);
	writel(0xffffffff, &STIMER->timer_load_count0);
	writel(0xffffffff, &STIMER->timer_load_count1);
	writel(1, &STIMER->timer_ctrl_reg);
}

/*
 * Progress signal: LED LEVEL, not blinking.
 *
 * Each milestone toggles the line once. No delays, no patterns, so the signal
 * does not depend on the timebase -- which matters because the timebase is
 * exactly what has been unreliable here, and a blink that stretches by 64x is
 * indistinguishable from a hang.
 *
 * The starting state is ON: this line is a GPIO whose level survives a warm
 * reset, and the vendor kernel drives it high (machine_rk30_board_init does
 * gpio_request(207, "led_en") then gpio_direction_output(207, 1)). So after a
 * reboot from Android it is already lit, and the SPL turning it OFF is positive
 * proof our code ran -- nothing else on the board turns it off.
 *
 *   LED goes OFF   SPL entered
 *   LED goes ON    DRAM trained, handing back to the mask ROM
 *
 * If it stays on, the ROM never reached us. If it goes off and stays off,
 * sdram_init() did not return.
 *
 * gpio2 15 = "leds-ctl" = the blue LED, active-high, verified on hardware.
 * Kept DM-free: the SPL is a separate binary with no driver model.
 */
#define CRU_CLKGATE_CON8	0x200000f0
#define GATE_PCLK_GPIO2		11
#define GPIO2_BASE		0x20084000
#define GPIO_SWPORTA_DR		0x00
#define GPIO_SWPORTA_DDR	0x04
#define LED_BIT			BIT(15)

static void spl_led_init(void)
{
	/* CRU hiword mask: set mask bit, clear value bit -> ungate PCLK_GPIO2 */
	writel(BIT(GATE_PCLK_GPIO2 + 16), CRU_CLKGATE_CON8);
	setbits_le32(GPIO2_BASE + GPIO_SWPORTA_DDR, LED_BIT);
}

static void spl_led(bool on)
{
	if (on)
		setbits_le32(GPIO2_BASE + GPIO_SWPORTA_DR, LED_BIT);
	else
		clrbits_le32(GPIO2_BASE + GPIO_SWPORTA_DR, LED_BIT);
}

void board_init_f(ulong dummy)
{
	/*
	 * LED first, before any other peripheral is touched.  It is the only
	 * output we have, so anything that can hang -- a gated timer block, the
	 * debug UART -- must come after it, or a hang is indistinguishable from
	 * the ROM never branching into us.
	 *
	 * OFF = the mask ROM read the SD card and branched into our SPL.
	 */
#ifdef CONFIG_DEBUG_UART
	debug_uart_init();
#endif

	rockchip_stimer_init();

	/* OFF = the mask ROM read the SD card and branched into our SPL */
	spl_led_init();
	spl_led(false);

	sdram_init();

	/* ON = DRAM training returned; safe to hand back to the ROM */
	spl_led(true);

	/* return to the mask ROM, which loads U-Boot proper into DRAM */
	back_to_bootrom(BROM_BOOT_NEXTSTAGE);
}

/* Placeholder: board_init_r never runs (back_to_bootrom does not return). */
void board_init_r(gd_t *id, ulong dest_addr)
{
	while (1)
		;
}
