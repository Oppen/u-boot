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
#include <spl.h>

/*
 * Delay history, kept because each entry is a boot someone paid for.
 *
 * The 0x200440a0 path was REMOVED on 2026-07-31 and is not what runs now; the
 * A9 global timer below replaced it later the same day. Why the old one went,
 * and what it cost to keep:
 *
 *  1. The counter at 0x200440a0 DOES NOT ADVANCE. Settled 2026-07-28; the
 *     register reads zero (docs/HARDWARE-CONSTANTS.md). The old code probed it
 *     at init, found it dead every time, and fell back to the spin -- so the
 *     timer arithmetic never executed on this hardware.
 *
 *     Retracted with it: the old "24 MHz / 64 = 375 kHz, MEASURED" claim. The
 *     symptom behind it stands -- a delay written for 24 MHz took ~65 s for a
 *     requested 1 s, timed by hand -- but the cause was the spin, not a divided
 *     clock.
 *
 *     It was not free. `(u64)usec * TIMER_HZ / 1000000` was the SPL's only
 *     caller of __aeabi_uldivmod, which in turn pulled in lib1funcs.o:
 *     424 + 392 = 816 bytes, 21% of a 3880-byte binary against a 6144-byte
 *     SRAM budget, to compute ticks for a counter that does not tick.
 *     Measured with scripts/spl-size-report.py.
 *
 *  2. The spin is slow, and that is the reason delays here are glacial rather
 *     than wrong. sdram_init() runs far behind its intended timing. DDR delays
 *     are minimums, so slow is safe and fast is not -- which is why this has not
 *     been "optimised".
 *
 *     The SPL also runs before our PLL setup (that happens in the CRU probe, in
 *     U-Boot proper, from DRAM -- the wrong stage, see docs/UPSTREAM-ALIGNMENT.md),
 *     so the core is still near 24 MHz here. Behavioural corroboration: the
 *     pre-relocation LED rhythms look slower than the post-relocation ones.
 *
 * THE SPL TIMEBASE IS UNVERIFIED. `sleep 30` proved get_timer()/mdelay() in
 * U-Boot proper only (2026-07-30); nothing equivalent exists for this binary.
 *
 *
 * This file is the configuration that reached "DRAM trained" and handed off to
 * U-Boot proper over SD (2026-07-27, observed by the maintainer). A later switch
 * to lib/time.c's CFG_SYS_TIMER_* path, and a 2026-07-28 move to timer0 at
 * 0x20044000, each stopped it booting. Do not replace it again without a boot as
 * evidence -- and see the CONFIG_ROCKCHIP_STIMER_BASE help in rk3026/Kconfig,
 * which makes the same point about the address.
 */
/*
 * THE TIMEBASE, 2026-07-31: the Cortex-A9 MPCore global timer.
 *
 * Not the block this file used to point at. PERIPHBASE is 0x1013c000
 * (device tier: SCU Control reads 0x21, SCU Config 0x531), and the TRM's
 * private-memory map puts a 64-bit incrementing global timer at +0x200 with its
 * control register at +0x208. Unlike 0x200440a0 it is guaranteed by the CPU
 * rather than by the SoC integrator.
 *
 * Confirmed live at our own U-Boot prompt, device tier: the whole window reads
 * zero until bit 0 of +0x208 is written -- the vendor kernel never uses this
 * block -- and then the counter advances.
 *
 * RATE. PERIPHCLK = CPU/4, measured at two operating points (228.3 MHz at a
 * 912 MHz Android CPU; 159.9 MHz at our 600 MHz APLL). What the CPU runs at
 * HERE is genuinely open: the BootROM is documented to raise APLL to 192 MHz and
 * record 0xC0 at 0x10080000, while this file's own behavioural note says the core
 * looks near 24 MHz. That spread is 48 MHz vs 6 MHz of PERIPHCLK.
 *
 * So the constant OVERESTIMATES on purpose. Waiting `usec * TICKS_PER_US` ticks
 * takes usec * (TICKS_PER_US / real_rate_MHz) of real time, so too high a
 * constant errs LONG and too low errs SHORT -- and DDR delays are minimums,
 * where long is safe and short is not. At 48 the delay is exact if the ROM did
 * raise the clock and 8x too long if it did not. Task: measure it and set it
 * exactly (docs/BLOCKERS.md).
 *
 * Still 32-bit arithmetic only. `(u64)usec * rate / 1000000` is what pulled
 * __aeabi_uldivmod and lib1funcs.o into this binary -- 816 bytes, 21% of it --
 * for a counter that never ticked. A multiply by a small integer needs neither.
 * usec * 48 overflows u32 only past ~89 s.
 *
 * The spin fallback STAYS, and is not vestigial: if the counter does not advance
 * this SPL has no console to report that with, and a wait-for-ticks loop on a
 * dead counter is an unbreakable hang. Liveness is probed once, in
 * rockchip_stimer_init(), and only a counter observed moving is used.
 *
 * Constraints carried over, both still live:
 *  - do NOT route this through lib/time.c's CFG_SYS_TIMER_* path. That change
 *    stopped the SPL booting on 2026-07-27, from a state that had reached "DRAM
 *    trained" and handed off over SD.
 *  - the rate assumption breaks if PLL setup ever moves into the SPL, which is
 *    a planned change: the clock would then change mid-binary.
 */
#define A9_PERIPHBASE		0x1013c000
#define A9_GT_COUNTER_LO	(A9_PERIPHBASE + 0x200)
#define A9_GT_CONTROL		(A9_PERIPHBASE + 0x208)
#define A9_GT_CTRL_ENABLE	BIT(0)

/* CPU 192 MHz / 4. See the rate discussion above: high errs long, which is safe. */
#define SPL_GT_TICKS_PER_US	48

/*
 * Deliberately generous, where slow beats hung. Used only when the global timer
 * is not advancing.
 */
#define SPL_FALLBACK_LOOPS_PER_US	12

/* Zero until proven otherwise, so a failed probe cannot select the timer path. */
static u32 spl_gt_ticks_per_us;

static void spl_udelay_spin(unsigned long usec)
{
	volatile u32 i;
	u32 loops = usec * SPL_FALLBACK_LOOPS_PER_US;

	for (i = 0; i < loops; i++)
		;
}

void __udelay(unsigned long usec)
{
	u32 start, ticks;

	if (!spl_gt_ticks_per_us) {
		spl_udelay_spin(usec);
		return;
	}

	ticks = usec * spl_gt_ticks_per_us;
	start = readl((void __iomem *)A9_GT_COUNTER_LO);

	/* Unsigned difference, so the 32-bit wrap needs no special case. */
	while ((readl((void __iomem *)A9_GT_COUNTER_LO) - start) < ticks)
		;
}

/*
 * MUST remain an override, and now it also does the work.
 *
 * The weak generic version (arch/arm/mach-rockchip/spl_common.c) writes CNTFRQ
 * with `mcr p15, 0, %0, c14, c0, 0`. That is the ARM Generic Timer, which
 * Cortex-A9 does not implement (DDI0407I has no CNTFRQ/CNTPCT at all), so the
 * instruction is UNDEFINED here. Deleting this function does not remove code --
 * it substitutes an undefined-instruction trap.
 *
 * Enable the global timer, then PROVE it moves before letting __udelay() depend
 * on it. Writing bit 0 alone leaves the prescaler at divide-by-1 and both
 * comparator and IRQ disabled, so this cannot raise an interrupt.
 *
 * The probe is a raw spin, not a delay: there is no timebase yet, and it needs
 * only to be long enough that a live counter certainly moves. Called after
 * debug_uart_init() and before sdram_init(), so it is in place for every delay
 * DDR training takes.
 */
void rockchip_stimer_init(void)
{
	volatile u32 i;
	u32 a, b;

	writel(A9_GT_CTRL_ENABLE, (void __iomem *)A9_GT_CONTROL);

	a = readl((void __iomem *)A9_GT_COUNTER_LO);
	for (i = 0; i < 1000; i++)
		;
	b = readl((void __iomem *)A9_GT_COUNTER_LO);

	if (b != a)
		spl_gt_ticks_per_us = SPL_GT_TICKS_PER_US;
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

/*
 * Rhythms, not counts. Each milestone gets a shape recognisable at a glance: a
 * count has to be watched from its start and tallied, and adjacent counts look
 * alike -- which matters most here, where the milestones are "the ROM read the
 * card" and "DRAM works", and confusing them sends you debugging the wrong
 * thing.
 *
 *   SPL entered    symmetric   1:1   equal on and off
 *   DRAM trained   mostly on   8:1   looks lit, winks off
 *
 * Ratios, not rates: if the timebase is wrong every pattern stretches by the
 * same factor, so a ratio still identifies the stage where an absolute rate
 * does not.
 *
 * This is the signalling that was confirmed on hardware on 2026-07-27.
 */
/*
 * T1: one switch for every rhythm. They cost real boot time on a stretched
 * timebase, so they come out once the chain is trusted -- and come back with a
 * one-line change. Step builds (T2) never run them: a step image must not
 * depend on __udelay() at all, which is the point of having steps.
 */
#if defined(UBOOT_CURRENT_STEP) || defined(BOOX_NO_LED_PROGRESS)
#define BOOX_LED_PROGRESS	0
#else
#define BOOX_LED_PROGRESS	1
#endif

static const u16 spl_rhy_slow[] = { 100, 100 };
static const u16 spl_rhy_fast[] = { 200,  25 };

#define SPL_LED_DWELL_MS	600

static void spl_led_rhythm(const u16 *ms, unsigned int n)
{
	u32 elapsed = 0;
	unsigned int i;

	while (elapsed < SPL_LED_DWELL_MS) {
		for (i = 0; i < n; i++) {
			spl_led(!(i & 1));
			mdelay(ms[i]);
			elapsed += ms[i];
		}
	}
	spl_led(false);
}

/*
 * Step probe builds (-DUBOOT_CURRENT_STEP=N via KCFLAGS).
 *
 * One image per step. The marked step turns the LED off and halts; nothing
 * else runs. No rhythm to catch mid-cycle, no duty ratio to judge, no timer
 * involved -- the only question per image is "did it go dark", and the answer
 * keeps until you look. Burn the next number to advance.
 *
 * Steps 1-5 are the SPL; 6+ are U-Boot proper, in
 * board/rockchip/boox-c67ml/boox-c67ml.c.
 */
/*
 * Raw spin, for signalling only. Not a delay primitive: no timer, no udelay().
 *
 * Count is set from a MEASUREMENT, not an assumption: 3,000,000 iterations took
 * 10-20 s on hardware (2026-07-28), i.e. ~200k iterations/second, so 100,000 is
 * about half a second per half-cycle. An order of magnitude either way still
 * reads as blinking, which is all this has to survive.
 */
static void spl_spin(u32 n)
{
	volatile u32 i;

	for (i = 0; i < n; i++)
		;
}

/*
 * Step probe builds (-DUBOOT_CURRENT_STEP=N via KCFLAGS).
 *
 * The marked step blinks FOREVER and never returns. Positive signal on purpose:
 * "dark" is also what a hang looks like, so a step that ended dark could not be
 * told apart from a crash just before it. Nothing else in this firmware ends in
 * a repeating blink.
 *
 * Steps 1-5 are the SPL; 6+ are U-Boot proper, in
 * board/rockchip/boox-c67ml/boox-c67ml.c.
 */
#define SPL_STEP_SPIN	100000

#ifdef UBOOT_CURRENT_STEP
#define BOOX_STEP(n)						\
	do {							\
		if ((n) == UBOOT_CURRENT_STEP) {		\
			spl_led_init();				\
			while (1) {				\
				spl_led(true);			\
				spl_spin(SPL_STEP_SPIN);	\
				spl_led(false);			\
				spl_spin(SPL_STEP_SPIN);	\
			}					\
		}						\
	} while (0)
#else
#define BOOX_STEP(n)	do { } while (0)
#endif

/*
 * Counter probes, steps 24-27 (T9/T10/T11). Run before sdram_init so the answer
 * arrives in seconds.
 *
 * Every reading we have of timer0/timer1 was taken under a running Android
 * kernel, which says nothing about decode or clock gating at ROM handoff. These
 * sample the counter directly at the point the SPL actually uses it.
 *
 *   24  0x200440a0 (the base the SPL uses today) -- expected dead
 *   25  timer0 at 0x20044000, programmed first
 *   26  timer1 at 0x20044020, programmed first
 *   27  timer0 again, but bucketing the RATE rather than liveness
 *
 * Signals: fast flicker = advancing (for 27: ~MHz), medium = advancing slowly
 * (27 only), slow 1:1 = static. NO BLINK AT ALL is a third outcome: the access
 * hung the bus, which a read of an undecoded or gated address can do.
 */
#if defined(UBOOT_CURRENT_STEP) && UBOOT_CURRENT_STEP >= 24 && UBOOT_CURRENT_STEP <= 27
#define PROBE_SPIN	100000

static void boox_counter_probe(void)
{
	u32 base, a, b, delta, period;

#if UBOOT_CURRENT_STEP == 24
	base = 0x200440a0;
	const bool program = false;
#elif UBOOT_CURRENT_STEP == 26
	base = 0x20044020;
	const bool program = true;
#else
	base = 0x20044000;
	const bool program = true;
#endif

	spl_led_init();

	if (program) {
		/* MiniLoader's recipe, from FlashBoot: load0 = 0xffffffff at
		 * +0x00, ctrl = 1 at +0x10, counter read at +0x08. */
		writel(0, (void __iomem *)(base + 0x10));
		writel(0xffffffff, (void __iomem *)(base + 0x00));
		writel(0xffffffff, (void __iomem *)(base + 0x04));
		writel(1, (void __iomem *)(base + 0x10));
	}

	a = readl((void __iomem *)(base + 0x08));
	spl_spin(PROBE_SPIN);
	b = readl((void __iomem *)(base + 0x08));
	delta = a - b;			/* counts down; unsigned wrap is fine */

#if UBOOT_CURRENT_STEP == 27
	/*
	 * PROBE_SPIN iterations is ~1.9 s at the measured ~54k iterations/s, so
	 * a 24 MHz counter should move by tens of millions.
	 */
	if (!delta)
		period = SPL_STEP_SPIN * 4;		/* static */
	else if (delta < 1000000)
		period = SPL_STEP_SPIN;			/* slow counter */
	else
		period = SPL_STEP_SPIN / 8;		/* ~MHz */
#else
	period = delta ? SPL_STEP_SPIN / 8 : SPL_STEP_SPIN * 4;
#endif

	while (1) {
		spl_led(true);
		spl_spin(period);
		spl_led(false);
		spl_spin(period);
	}
}
#endif

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
	/* 1: the ROM branched into us. First point at which anything is safe. */
	BOOX_STEP(1);

#ifdef CONFIG_DEBUG_UART
	debug_uart_init();
#endif

	/* 2: the debug UART did not hang. */
	BOOX_STEP(2);

	rockchip_stimer_init();

	/* 3: the timer block answered and the liveness probe finished. */
	BOOX_STEP(3);

#if defined(UBOOT_CURRENT_STEP) && UBOOT_CURRENT_STEP >= 24 && UBOOT_CURRENT_STEP <= 27
	boox_counter_probe();
#endif

	/*
	 * Step 10 is RETIRED (2026-07-31). It blinked at one of two rates to
	 * report what the runtime liveness probe had decided about the counter at
	 * CONFIG_ROCKCHIP_STIMER_BASE. That question is closed -- the counter
	 * never advances, the probe was removed with the timer path, and there is
	 * no longer a `stimer_ticks` to report. The answer is recorded in
	 * docs/HARDWARE-CONSTANTS.md and in HOW-TO's step table.
	 *
	 * The number is deliberately not reused: step numbers appear in session
	 * notes and card filenames, so recycling 10 would make old logs lie.
	 */

	/* slow pulse = the mask ROM read the SD card and branched into our SPL */
	spl_led_init();
	if (BOOX_LED_PROGRESS)
		spl_led_rhythm(spl_rhy_slow, ARRAY_SIZE(spl_rhy_slow));

	sdram_init();

	/* 4: sdram_init() returned. */
	BOOX_STEP(4);

	/*
	 * 12: does DRAM actually WORK?
	 *
	 * sdram_init() returning proves only that our code finished, not that
	 * the controller is configured correctly. If DRAM is dead, the ROM's
	 * copy of U-Boot proper to 0x60000000 goes into the void and its branch
	 * there executes garbage -- with the LED never touched, which looks
	 * exactly like "U-Boot proper never ran".
	 *
	 * Write/read-back across the whole 512 MiB, including the far end and a
	 * cross-bank address, with patterns that catch stuck bits and aliasing
	 * (address-in-address catches a controller that wraps).
	 *
	 *   fast flicker  every location read back correctly
	 *   slow 1:1      at least one failed
	 */
#ifdef UBOOT_CURRENT_STEP
	if (UBOOT_CURRENT_STEP == 12) {
		static const u32 offs[] = {
			0x00000000, 0x00000004, 0x00001000, 0x00100000,
			0x01000000, 0x08000000, 0x10000000, 0x1ffffff0,
		};
		bool ok = true;
		unsigned int k;

		for (k = 0; k < ARRAY_SIZE(offs); k++) {
			u32 a = 0x60000000 + offs[k];

			writel(0xa5a5a5a5, (void __iomem *)a);
			if (readl((void __iomem *)a) != 0xa5a5a5a5)
				ok = false;
			writel(a, (void __iomem *)a);		/* addr-in-addr */
			if (readl((void __iomem *)a) != a)
				ok = false;
		}
		/* re-read all: catches aliasing, where a later write clobbered an
		 * earlier address that the controller maps to the same cell */
		for (k = 0; k < ARRAY_SIZE(offs); k++) {
			u32 a = 0x60000000 + offs[k];

			if (readl((void __iomem *)a) != a)
				ok = false;
		}

		spl_led_init();
		while (1) {
			u32 period = ok ? SPL_STEP_SPIN / 8 : SPL_STEP_SPIN * 4;

			spl_led(true);
			spl_spin(period);
			spl_led(false);
			spl_spin(period);
		}
	}
#endif

	/* mostly-on = DRAM trained; safe to hand back to the ROM */
	if (BOOX_LED_PROGRESS)
		spl_led_rhythm(spl_rhy_fast, ARRAY_SIZE(spl_rhy_fast));

	/* 5: about to leave the SPL. */
	BOOX_STEP(5);

	/* return to the mask ROM, which loads U-Boot proper into DRAM */
	back_to_bootrom(BROM_BOOT_NEXTSTAGE);
}

/*
 * Placeholder: board_init_r never runs (back_to_bootrom does not return).
 *
 * Omitted when SPL_FRAMEWORK is on, which supplies its own in common/spl/spl.c
 * -- otherwise the two collide at link time. Enabling the framework is what
 * unlocks SPL_SYS_MALLOC_F, hence calloc, hence a device tree and driver model
 * in the SPL.
 */
#if !CONFIG_IS_ENABLED(FRAMEWORK)
void board_init_r(gd_t *id, ulong dest_addr)
{
	while (1)
		;
}
#else
/*
 * The framework asks the board where it booted from. Ours never loads anything:
 * back_to_bootrom() hands control to the mask ROM, which streams stage 2 into
 * DRAM itself. mach-rockchip/spl.c has an implementation of this, but it is
 * gated on SPL_ROCKCHIP_COMMON_BOARD, which would also bring its own
 * board_init_f and collide with ours.
 */
u32 spl_boot_device(void)
{
	return BOOT_DEVICE_BOOTROM;
}
#endif
