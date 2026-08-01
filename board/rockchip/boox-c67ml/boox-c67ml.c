// SPDX-License-Identifier: GPL-2.0+
/*
 * ONYX BOOX C67ML Magellan board support
 *
 * Copyright (c) 2024, Mario Rugiero
 *
 * Hardware: RK3026 dual-core Cortex-A9, 512 MB DDR3,
 *           4 GB raw NAND behind Rockchip's closed FTL (no mainline driver —
 *           boot medium is the microSD card), 6" E Ink display
 *           (EBC + TPS65185), RTL8188EU USB WiFi, ACT8931 PMIC.
 *
 * DRAM size/base come from the device tree; DRAM itself was brought up by the
 * SPL (see sdram_config.c for the die geometry) before the mask ROM loaded
 * U-Boot proper.
 */

#include <dm.h>
#include <env.h>
#include <event.h>
#include <fdtdec.h>
#include <init.h>
#include <iomux.h>
#include <stdio.h>
#include <time.h>
#include <asm/global_data.h>
#include <asm/io.h>
#include <boox_probe.h>
#include <cyclic.h>
#include <linux/bitops.h>
#include <linux/delay.h>

DECLARE_GLOBAL_DATA_PTR;

/*
 * Aliveness signal.  There is no serial console on this unit yet (the UART is
 * only reachable at the 0.4 mm LQFP176 leads), so first boot has to be observed
 * some other way.  gpio2 15 ("leds-ctl", GPIO2_B7) is the board's only LED
 * line; the vendor kernel drives it as an output, and GPIO2B's iomux reads back
 * as 0 (= GPIO function) on a live device, so no GRF write is needed here.
 *
 * Verified on hardware by toggling this pin from the live vendor Android: it
 * drives the BLUE LED, active-high.  (The nearby red LED is a separate charge
 * indicator and is not software-controlled.)
 *
 * Each milestone has its own repeating RHYTHM, not a blink count. A rhythm is
 * recognisable at a glance and from the corner of your eye; a count has to be
 * watched from its start and tallied, and two adjacent counts look alike.
 *
 * The patterns are chosen so that each differs from the others by something
 * QUALITATIVE, not by a count and not by phase. Two rhythms with the same
 * period and inverted duty (say 800/200 and 200/800) are the same waveform
 * shifted, and tell you nothing if you join mid-cycle; a pair that differs
 * only in how many pulses per group is just counting again. So:
 *
 *   SPL entered      1:1, one pulse    equal on and off
 *   DRAM trained     8:1, one pulse    looks lit, winks off
 *   early_init_f     1:8, one pulse    looks dark, ticks on
 *   dram_init        two equal taps    tap-tap ... pause
 *   misc_init_r      three equal taps  tap-tap-tap ... pause
 *   last_stage_init  dash-dot          long glow, short tap
 *
 * No two stages share a signature, where signature means (duty ratio, pulses
 * per group, whether those pulses are equal). That matters more than it looks:
 * if the timebase is wrong every pattern stretches by the same factor, so a
 * ratio still identifies a stage but an absolute rate does not. An earlier
 * revision gave the SPL 1 s / 1 s and board_early_init_f 120 ms / 120 ms --
 * both 1:1 -- and they were indistinguishable on hardware, because "fast" and
 * "slow" are not readable without knowing what the clock is doing.
 *
 * dash-dot is the only group with grossly unequal pulses, which is what makes
 * the terminal payload unmistakable next to the two tap patterns.
 *
 * Each runs in a loop for a dwell rather than once, so it is unmistakable, and
 * a hang inside a dwell leaves its rhythm repeating forever -- which is the
 * failure you most want to be able to read off the device. A hang between
 * dwells freezes the LED instead; the rhythm you last saw still localises it.
 */
#define CRU_BASE		0x20000000
#define CRU_CLKGATE_CON8	(CRU_BASE + 0xf0)
#define GATE_PCLK_GPIO2		11

#define GPIO2_BASE		0x20084000
#define GPIO_SWPORTA_DR		0x00
#define GPIO_SWPORTA_DDR	0x04
#define LED_BIT			BIT(15)

static void led_init(void)
{
	/*
	 * CRU gate registers take a hiword write mask: the upper 16 bits select
	 * which bits of the lower half are written.  Setting the mask bit while
	 * leaving the value bit clear ungates PCLK_GPIO2 and touches nothing
	 * else.  (Reset default is gated; the vendor kernel shows con8 = 0x0003.)
	 */
	writel(BIT(GATE_PCLK_GPIO2 + 16), CRU_CLKGATE_CON8);
	setbits_le32(GPIO2_BASE + GPIO_SWPORTA_DDR, LED_BIT);
}

static void led_set(bool on)
{
	if (on)
		setbits_le32(GPIO2_BASE + GPIO_SWPORTA_DR, LED_BIT);
	else
		clrbits_le32(GPIO2_BASE + GPIO_SWPORTA_DR, LED_BIT);
}

/*
 * Spin delay for the pre-relocation stages, which run before the DM timer.
 *
 * The core clock here is 192 MHz, not the 24 MHz the earlier comment assumed:
 * the BootROM raises APLL to 24 x 128 / (4 x 2 x 2) before handing over, and
 * records 192 in a word at 0x10080000 (BROM 0x6a4, guarded on that word being
 * 0xC0 = 192). The vendor DDR blob's own "if (*(u32*)0x10080000 == 0xC0)" is
 * that same check, so the clocks are already up by the time we run.
 *
 * MEASURED, 2026-07-28, and it contradicts the 192 MHz figure above: a raw
 * volatile spin of 3,000,000 iterations in the SPL took 10-20 s on hardware,
 * i.e. ~200k iterations/second. So one millisecond is ~200 iterations, not
 * 48000 -- the old constant made every pattern ~240x too long, which is why
 * later boot stages looked like a dead LED (docs/PENDING.md H5). Whether the
 * cause is a core clock far below 192 MHz or very slow memory is open (H6);
 * either way this constant now comes from the stopwatch, not from arithmetic.
 */
#define LED_SPIN_PER_MS	200

static void led_delay_spin_ms(u32 ms)
{
	volatile u32 i;
	u32 n = ms * LED_SPIN_PER_MS;

	for (i = 0; i < n; i++)
		;
}

/*
 * A rhythm is a list of alternating on/off durations in ms, starting with on.
 */
struct led_rhythm {
	const u16	*ms;
	u8		n;
};

static const u16 rhy_mostly_off[]  = {  150, 1200 };
static const u16 rhy_double_tap[]  = {   90,  160,  90, 1200 };
static const u16 rhy_triple_tap[]  = {   90,  160,  90,  160,  90, 1200 };
static const u16 rhy_dash_dot[]    = { 1500,  300,  90,  900 };

#define RHYTHM(a)	{ .ms = (a), .n = ARRAY_SIZE(a) }

/* How long to hold each rhythm before moving on. */
#define LED_DWELL_MS	6000

static void led_delay_ms(u32 ms, bool use_timer)
{
	if (use_timer)
		mdelay(ms);
	else
		led_delay_spin_ms(ms);
}

/*
 * Run `r` on a loop for LED_DWELL_MS, then leave the LED dark. Ending dark is
 * deliberate: the vendor firmware also drives this line, so a lit LED at rest
 * proves nothing.
 *
 * `use_timer` picks the delay source -- false before relocation, true from
 * board_init_r onward where mdelay() exists and is accurate.
 */
static void led_rhythm_run(const struct led_rhythm *r, bool use_timer)
{
	u32 elapsed = 0;
	unsigned int i;

	while (elapsed < LED_DWELL_MS) {
		for (i = 0; i < r->n; i++) {
			led_set(!(i & 1));
			led_delay_ms(r->ms[i], use_timer);
			elapsed += r->ms[i];
		}
	}
	led_set(false);
}

static const struct led_rhythm boox_rhythms[] = {
	RHYTHM(rhy_mostly_off),		/* board_early_init_f -- 1:8, dark */
	RHYTHM(rhy_double_tap),		/* dram_init -- two equal taps */
	RHYTHM(rhy_triple_tap),		/* misc_init_r -- three equal taps */
	RHYTHM(rhy_dash_dot),		/* EVT_LAST_STAGE_INIT -- terminal */
};

enum boox_stage {
	BOOX_EARLY_INIT_F,
	BOOX_DRAM_INIT,
	BOOX_MISC_INIT_R,
	BOOX_LAST_STAGE,
};

/*
 * Step probe builds (-DUBOOT_CURRENT_STEP=N via KCFLAGS); see the same macro in
 * arch/arm/mach-rockchip/rk3026/rk3026-board-spl.c. Steps 1-5 are the SPL,
 * 6-9 are here. The marked step turns the LED off and halts, so the result is a
 * single unambiguous state that keeps until you look at it.
 */
/*
 * Step probe builds (-DUBOOT_CURRENT_STEP=N via KCFLAGS); the SPL half lives in
 * arch/arm/mach-rockchip/rk3026/rk3026-board-spl.c. Steps 6-9 are here.
 *
 * Implementation is the shared one in common/boox_probe.c -- this file used to
 * carry its own inline copy with a 100,000 spin. Steps 6 and 7 run before
 * relocation with caches off, 8 and 9 after, hence the two variants.
 */
#define BOOX_STEP(n)	BOOX_STEP_AT(n)
#define BOOX_STEP_R(n)	BOOX_STEP_AT_R(n)

/*
 * T1: one switch for every rhythm here too. Off for step builds (T2), since a
 * step image must not depend on a delay primitive, and off via
 * BOOX_NO_LED_PROGRESS once the chain is trusted -- the rhythms cost real boot
 * time. led_rhythm_forever() for the terminal stage is deliberately NOT gated:
 * it is the result, not progress reporting.
 */
#if defined(UBOOT_CURRENT_STEP) || defined(BOOX_LED_PROGRESS_ON)
#define BOOX_LED_PROGRESS	0
#else
/*
 * OFF by default since 2026-08-01. The chain is trusted and there IS a console
 * now, so these dwells are pure cost: LED_DWELL_MS is 6000 and there are four
 * of them (board_early_init_f, dram_init, misc_init_r, last_stage), i.e. ~24
 * SECONDS of boot spent blinking. They became exact rather than merely slow
 * once the timebase started working, because led_rhythm_run() uses mdelay().
 *
 * Turn them back on with -DBOOX_LED_PROGRESS_ON when there is no console --
 * step builds still force them off, since a step image must not depend on a
 * delay primitive.
 */
#define BOOX_LED_PROGRESS	0
#endif

/*
 * Non-blocking heartbeat.
 *
 * This is what the four 6-second dwells should always have been. They blocked
 * because there was no cyclic infrastructure running -- which needed a live
 * get_timer_us(), which needed the timebase. All three exist now, so the LED
 * can carry the same stage information continuously while the boot proceeds,
 * at a cost of one toggle every few hundred ms instead of ~24 seconds of boot.
 *
 * The stage is a variable the callback reads, so each milestone changes the
 * pattern rather than stopping to perform it. Ratios, not rates, as before.
 */
static struct cyclic_info boox_led_cyclic;
static const struct led_rhythm *boox_cyclic_rhythm;
static unsigned int boox_cyclic_idx;

/*
 * Tick granularity. The shortest element in any rhythm is 90 ms, so 10 ms
 * resolves every pattern with room to spare and keeps the callback trivial --
 * cyclic polices per-call cost via CYCLIC_MAX_CPU_TIME_US (5000 us here).
 */
/*
 * Per-element timing from get_timer(), NOT from counting ticks.
 *
 * Measured on hardware: `cyclic list` reports 7.70 times/s against a requested
 * 10, because cyclic only runs when schedule() is called and the main loop sets
 * that cadence -- roughly 130 ms between entries. A callback that assumed each
 * entry was one tick of a known length would drift by that ratio and jitter with
 * whatever the loop is doing.
 *
 * The same measurement bounds what can be displayed: an element shorter than
 * the cadence cannot render, so the cyclic patterns below use elements of
 * >= 250 ms. They keep the qualitative signatures of the blocking rhythms --
 * duty ratio, pulses per group, equal vs unequal pulses -- which is what makes
 * a stage identifiable, rather than their exact millisecond values.
 */
#define BOOX_TICK_MS	50

static const u16 cyc_mostly_off[] = {  250, 2000 };		/* 1:8   */
static const u16 cyc_double_tap[] = {  250,  400, 250, 1600 };	/* tap tap  */
static const u16 cyc_triple_tap[] = {  250,  400, 250, 400, 250, 1600 };
static const u16 cyc_dash_dot[]   = { 1500,  500, 250, 1000 };	/* dash dot */

static const struct led_rhythm boox_cyclic_rhythms[] = {
	RHYTHM(cyc_mostly_off),		/* board_early_init_f */
	RHYTHM(cyc_double_tap),		/* dram_init */
	RHYTHM(cyc_triple_tap),		/* misc_init_r */
	RHYTHM(cyc_dash_dot),		/* last_stage_init */
};

static void boox_led_tick(struct cyclic_info *c)
{
	const struct led_rhythm *r = boox_cyclic_rhythm;
	static ulong due;

	if (due && get_timer(0) < due)
		return;

	boox_cyclic_idx = (boox_cyclic_idx + 1) % r->n;
	led_set(!(boox_cyclic_idx & 1));
	due = get_timer(0) + r->ms[boox_cyclic_idx];
}

static void boox_led_cyclic_start(void)
{
	static bool started;

	if (started || !boox_cyclic_rhythm)
		return;
	started = true;

	cyclic_register(&boox_led_cyclic, boox_led_tick,
			BOOX_TICK_MS * 1000, "boox-led");
}

static void led_stage(enum boox_stage stage, bool use_timer)
{
	/* Blocking dwell: only for step builds, or when explicitly asked for. */
	if (BOOX_LED_PROGRESS) {
		led_rhythm_run(&boox_rhythms[stage], use_timer);
		return;
	}

	/*
	 * Otherwise just retarget the heartbeat. use_timer tells us whether a
	 * timebase exists yet: before relocation it does not, so the pattern is
	 * recorded and the cyclic function starts at the first stage that has one.
	 */
	boox_cyclic_rhythm = &boox_cyclic_rhythms[stage];
	boox_cyclic_idx = 0;

	if (use_timer)
		boox_led_cyclic_start();
}

#ifdef CONFIG_BOARD_EARLY_INIT_F
int board_early_init_f(void)
{
	led_init();

	/* 6: U-Boot proper is executing from DRAM. */
	BOOX_STEP(6);

	led_stage(BOOX_EARLY_INIT_F, false);
	return 0;
}
#endif

/* board_init() is provided by the shared arch/arm/mach-rockchip/board.c */

#if CONFIG_IS_ENABLED(USB_GADGET_DWC2_OTG)
/*
 * OTG PHY bring-up, overriding the __weak no-op in
 * drivers/usb/gadget/dwc2_udc_otg.c:166. UNPROVEN on hardware.
 *
 * Nothing in our build touched the PHY before this: CONFIG_ROCKCHIP_USB2_PHY is
 * off, and enabling it would hang() -- its match table has no rk3026 entry.
 *
 * Every value below is read out of Rockchip's own rk30usbplug device-mode blob
 * (docs/re/USBPLUG-RE.md) -- EVIDENCE TIER: vendor blob, not measurement. The
 * vendor kernel's PHY path (docs/re/VENDOR-KERNEL-USB.md) writes a DIFFERENT
 * register set (GRF+0x280/0x288 in hw_init, GRF+0x17c/+0x194 in soft_reset).
 * That conflict is unresolved, so only the device-mode blob's sequence is
 * replicated here -- it is the one that demonstrably enumerates this SoC as a
 * USB device.
 *
 * Clock gates first, and that ordering is not cosmetic: reading a clock-gated
 * block hangs this bus.
 */
#define CRU_CLKGATE_CON1	(CRU_BASE + 0xd4)
#define CRU_CLKGATE_CON5	(CRU_BASE + 0xe4)
#define CRU_SOFTRST_CON4	(CRU_BASE + 0x120)
#define GRF_UOC1_CON4		0x20008190

/* CRU gate/reset registers take a hiword write mask: mask bit set, value bit
 * clear = ungate. */
#define GATE_HCLK_OTG0		BIT(29)		/* CLKGATE_CON5 bit 13 */
#define GATE_OTGPHY0		BIT(21)		/* CLKGATE_CON1 bit 5 */

void otg_phy_init(struct dwc2_udc *dev)
{
	/* AHB clock to 0x10180000 first, then the PHY clock. Vendor order. */
	writel(GATE_HCLK_OTG0, (void __iomem *)CRU_CLKGATE_CON5);
	writel(GATE_OTGPHY0, (void __iomem *)CRU_CLKGATE_CON1);

	/*
	 * Hiword-masked clear of UOC1_CON4 bits 10/12/13. Bits 12:13 are the OTG
	 * D+/D- mux the vendor kernel labels "usb otg dp/dm switch to usb phy".
	 */
	writel(0x34000000, (void __iomem *)GRF_UOC1_CON4);

	/* Assert USBOTG0 + USBPHY0 + OTGC0 together, then release. The vendor
	 * waits 1100/1000/100 us; these spins are longer, and delays are minimums. */
	writel(0x00e000e0, (void __iomem *)CRU_SOFTRST_CON4);
	led_delay_spin_ms(3);
	writel(0x00e00000, (void __iomem *)CRU_SOFTRST_CON4);
	led_delay_spin_ms(3);

	/*
	 * Step 76 reads the controller's ID register back through the LED, which
	 * is the only console this board has. Three OUTCOMES, by duty ratio:
	 *
	 *   4:1, long on, brief dark   GSNPSID matched -> the core is clocked and
	 *                              readable, so the gates above are open
	 *   1:4, long dark, brief on   the read returned something else
	 *   fixed level                the read never came back: bus hang, still gated
	 *
	 * Expected value from the vendor blob (docs/re/USBPLUG-RE.md):
	 * GSNPSID & ~0xfff == 0x4f542000.
	 */
#if defined(UBOOT_CURRENT_STEP) && UBOOT_CURRENT_STEP == 76
	{
		u32 id = readl((void __iomem *)(0x10180000 + 0x40));

		BOOX_REPORT(76, (id & ~0xfffU) == 0x4f542000);
	}
#endif
	/*
	 * 79: GOTGCTL.CONID_B, the bit dwc2_core_reset() uses to decide whether
	 * this is a host. If it reads 0 the driver sets host_mode = true and then
	 * waits for GINTSTS_CURMODE_HOST, which cannot happen in device mode.
	 *
	 * match (long on) = CONID_B set, i.e. correctly seen as a B-device
	 * mismatch (long dark) = clear, i.e. the driver will take the host path
	 */
#if defined(UBOOT_CURRENT_STEP) && UBOOT_CURRENT_STEP == 79
	BOOX_REPORT(79, readl((void __iomem *)0x10180000) & BIT(16));
#endif
}

/*
 * PULSE the reset, do not park in it.
 *
 * This is reached from udc_disable() (dwc2_udc_otg.c:186), which is on the
 * g_dnl_unregister() path -- so it now runs at every kernel handoff, not just
 * on an explicit shutdown. Asserting SOFTRST_CON4 and leaving it asserted
 * handed Linux USBOTG0 + USBPHY0 + OTGC0 held in reset, and touching a block
 * in reset hangs this bus: observed 2026-07-31 as a dark LED with the U-Boot
 * console correctly gone and no new gadget appearing.
 *
 * Same assert/release shape as otg_phy_init() above.
 */
void otg_phy_off(struct dwc2_udc *dev)
{
	writel(0x00e000e0, (void __iomem *)CRU_SOFTRST_CON4);
	led_delay_spin_ms(3);
	writel(0x00e00000, (void __iomem *)CRU_SOFTRST_CON4);
}

/*
 * Hand Linux a USB core that is no longer enumerated.
 *
 * Nothing released the gadget before this: board_quiesce_devices() is a weak
 * no-op (boot/bootm.c:50), and acm_stdio_stop() -- which does the real work,
 * g_dnl_unregister() (f_acm.c:676) -- only runs when the ACM device stops being
 * a console, which booting a kernel never did. Observed consequence
 * (2026-07-31): the host kept a live CDC-ACM device right across the handoff,
 * so Linux inherited a configured dwc2 core.
 *
 * Reassigning the consoles is what triggers that teardown, and the ordering is
 * load-bearing: acm_stdio_stop() sets stdio_dev->priv to NULL and the unbind
 * frees the f_acm, so anything printing to usbacm afterwards would dereference
 * freed memory. bootm prints "Starting kernel ..." BEFORE calling us and prints
 * nothing after, so this is the last safe point (boot/bootm.c:1200-1219).
 *
 * We do not call otg_phy_off() ourselves, but g_dnl_unregister() reaches it via
 * udc_disable() (dwc2_udc_otg.c:186) -- which is why that function had to be
 * fixed to release SOFTRST_CON4 instead of parking in it. See it below.
 */
void board_quiesce_devices(void)
{
	iomux_doenv(stdin, "serial");
	iomux_doenv(stdout, "serial");
	iomux_doenv(stderr, "serial");
}
#endif
int dram_init(void)
{
	int ret = fdtdec_setup_mem_size_base();

	/* 7: the device tree parsed and DRAM was sized. */
	BOOX_STEP(7);


	if (!ret)
		led_stage(BOOX_DRAM_INIT, false);

	return ret;
}

int dram_init_banksize(void)
{
	return fdtdec_setup_memory_banksize();
}

#ifdef CONFIG_MISC_INIT_R
/*
 * Where the SPL left its global-timer calibration: GRF scratch registers, which
 * survive SPL -> BROM -> here and need no DRAM. MUST match GRF_OS_REG2..4 and
 * SPL_GT_STASH_MAGIC16 in arch/arm/mach-rockchip/rk3026/rk3026-board-spl.c.
 *
 * A DRAM stash at 0x60f00000 was tried first and the magic never arrived. The
 * reason is now known: that address is CONFIG_PRE_CON_BUF_ADDR, so the
 * pre-console buffer overwrote the stash. It was never evidence that DRAM is
 * unusable as a cross-stage carrier.
 *
 * ⚠️ OS_REG2..4 are OURS, and OS_REG0/1/5 belong to the DDR blob, the DRAM
 * geometry word and the boot-mode register respectively. Only OS_REG6/7 are
 * free. Anything else that wants cross-stage scratch must check that first: a
 * later user picked OS_REG2..4 and silently read back this calibration.
 */
#define GRF_OS_REG2		0x200081d0
#define GRF_OS_REG3		0x200081d4
#define GRF_OS_REG4		0x200081d8
#define SPL_GT_STASH_MAGIC16	0x6774		/* "gt" */

/*
 * Print what the SPL measured. The SPL has no console, and its rate was until
 * now an assumed constant (CPU 192 MHz / 4) with the real CPU clock in that
 * window unresolved -- so this is the readout that turns it into a measurement.
 *
 * dgt/dt0 are the raw deltas over one spin: timer0 is xin24m at a fixed 24 MHz
 * and does not move with the PLLs, so MHz = 24 * dgt / dt0 and the spin's own
 * rate cancels.
 */
static void boox_report_spl_timer(void)
{
	u32 w = readl((void __iomem *)GRF_OS_REG2);

	if ((w >> 16) != SPL_GT_STASH_MAGIC16) {
		printf("SPL timer: no calibration stashed (OS_REG2 %08x)\n", w);
		return;
	}

	printf("SPL timer: A9 global timer %u ticks/us; dgt %u / dt0 %u (ref 24 MHz)\n",
	       w & 0xffff, readl((void __iomem *)GRF_OS_REG3),
	       readl((void __iomem *)GRF_OS_REG4));
}

int misc_init_r(void)
{
	/* 8: relocation and the driver model survived. */
	BOOX_STEP_R(8);

	boox_report_spl_timer();

	/*
	 * 78: is the timebase alive AFTER relocation? Everything in dwc2 bounds
	 * its waits with get_timer(), so a dead counter turns each
	 * wait_for_bit_le32(..., 1000, false) into an infinite loop rather than a
	 * timeout (include/wait_bit.h:45). BLOCKERS.md records the SPL counter at
	 * 0x200440a0 as dead; the DM timer here is a different base.
	 *
	 * match (long on) = the counter moved across a spin, timeouts work
	 * mismatch (long dark) = it never moved, every dwc2 wait hangs forever
	 */
	/*
	 * 81: read timer0's counter DIRECTLY, bypassing the driver model, after
	 * ungating it ourselves. Splits "the hardware counter is dead" from "our DM
	 * timer path is broken". Gate bits from docs/re/CRU-GATES.md: timer0 =
	 * CLKGATE_CON1 bit 0, pclk_timer0 = CLKGATE_CON7 bit 7 (hiword mask: mask
	 * bit set + value bit clear = ungate). Register layout is the vendor blob's
	 * (docs/re/USBPLUG-RE.md): LOAD0 +0x00, CURR +0x08, CTRL +0x10.
	 *
	 *   dark forever  the counter read wedged the bus
	 *   long on       it advances -- hardware fine, our DM path is not
	 *   long dark     static even when ungated and started by hand
	 */
	env_set("board", "boox-c67ml");
	led_stage(BOOX_MISC_INIT_R, true);
	return 0;
}
#endif

#if CONFIG_IS_ENABLED(LAST_STAGE_INIT)
/*
 * Terminal payload: run a rhythm and never return.
 *
 * The intermediate milestones dwell and move on, because boot has to continue.
 * The last one does not: reaching it is the whole result we are testing for, so
 * it becomes the program. An indefinitely repeating rhythm is unambiguous --
 * you can pick the device up at any moment, from across the room, and read
 * "U-Boot got all the way to the end" off it, with no need to have been
 * watching when it started.
 *
 * This deliberately never hands off to bootcmd. Delete the call in
 * boox_last_stage() to let distroboot ("bootflow scan") take over and load the
 * kernel from the FAT partition instead.
 */
static void __noreturn led_rhythm_forever(const struct led_rhythm *r,
					  bool use_timer)
{
	unsigned int i;

	for (;;) {
		for (i = 0; i < r->n; i++) {
			led_set(!(i & 1));
			led_delay_ms(r->ms[i], use_timer);
		}
	}
}

/*
 * board_late_init() is already taken by arch/arm/mach-rockchip/board.c, so the
 * final stage hooks the event instead — it fires at the end of board_init_r,
 * immediately before the main loop hands off to bootcmd.
 */
static int boox_last_stage(void)
{
	/* 9: end of U-Boot init, about to hand to bootcmd. */
	BOOX_STEP_R(9);

	/*
	 * One dwell, then RETURN, so board_init_r reaches run_main_loop().
	 * That matters beyond tidiness: the USB ACM gadget only enumerates while
	 * something polls the console, and nothing polls before the main loop --
	 * so halting here made a USB console impossible to observe.
	 *
	 * mdelay(), not a spin: get_timer() has been verified on hardware since
	 * 2026-07-29 (card 78) and misc_init_r above already runs its rhythm off the
	 * timer on the same boot. The spin here dated from the era of a dead
	 * timebase, when a blocked get_timer() left the LED at a fixed level and
	 * looked exactly like a crash.
	 *
	 * The two PRE-RELOCATION rhythms (board_early_init_f, dram_init) stay on
	 * spins deliberately: they run before timer_init() in board_f's sequence, so
	 * there is no timebase to use yet. Their spin constant is calibrated for a
	 * ~24 MHz CPU and the core now runs at 600 MHz, so those two rhythms are
	 * roughly 25x faster than their nominal ms -- the ratios still identify the
	 * stage, which is the property the scheme was designed around.
	 */
	led_rhythm_run(&boox_rhythms[BOOX_LAST_STAGE], true);

	return 0;
}
EVENT_SPY_SIMPLE(EVT_LAST_STAGE_INIT, boox_last_stage);
#endif
