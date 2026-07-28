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
#include <asm/global_data.h>
#include <asm/io.h>
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
 *   last_stage_init  dash-dot          long glow, short tap  -- FOREVER
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
 * Empirically ~4 cycles per iteration of this loop, so 48000 iterations is
 * about 1 ms at 192 MHz. Slow by a factor of a few is harmless for a blink;
 * being 40x out either way is what made an earlier build blink invisibly.
 */
#define LED_SPIN_PER_MS	48000

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

static void led_stage(enum boox_stage stage, bool use_timer)
{
	led_rhythm_run(&boox_rhythms[stage], use_timer);
}

#ifdef CONFIG_BOARD_EARLY_INIT_F
int board_early_init_f(void)
{
	led_init();
	led_stage(BOOX_EARLY_INIT_F, false);
	return 0;
}
#endif

/* board_init() is provided by the shared arch/arm/mach-rockchip/board.c */

int dram_init(void)
{
	int ret = fdtdec_setup_mem_size_base();

	if (!ret)
		led_stage(BOOX_DRAM_INIT, false);

	return ret;
}

int dram_init_banksize(void)
{
	return fdtdec_setup_memory_banksize();
}

#ifdef CONFIG_MISC_INIT_R
int misc_init_r(void)
{
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
	/*
	 * Bring-up build: stop here and blink forever rather than booting.
	 * See led_rhythm_forever() for how to re-enable distroboot.
	 */
	led_rhythm_forever(&boox_rhythms[BOOX_LAST_STAGE], true);

	return 0;
}
EVENT_SPY_SIMPLE(EVT_LAST_STAGE_INIT, boox_last_stage);
#endif
