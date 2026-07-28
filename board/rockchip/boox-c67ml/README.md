# U-Boot for ONYX BOOX C67ML Magellan (RK3026)

Mainline U-Boot board port for the ONYX BOOX C67ML Magellan e-reader
(Rockchip RK3026, dual Cortex-A9). Modeled on the in-tree evb-rk3036 A9 board.

## Status: builds clean (verified)

`make boox_c67ml_defconfig && make` produces, with **zero errors**:

| Artifact | Size | Purpose |
|---|---|---|
| `u-boot-rockchip.bin` | ~8.7 MB | complete flashable image (idbloader + u-boot), `dd` to SD sector 64 |
| `idbloader.img` | 4 KB | SPL with the rksd header |
| `spl/u-boot-spl.bin` | ~1.2 KB | minimal back-to-BROM SPL |
| `u-boot.bin` | ~419 KB | U-Boot proper |

Built with Debian Trixie + `gcc-arm-linux-gnueabihf` 14.2 (see
`../../../../../docker/uboot.Dockerfile`):

```bash
docker build -f docker/uboot.Dockerfile -t rk3026-uboot:trixie .
docker run --rm -v "$PWD/mainline_u-boot:/build/u-boot" rk3026-uboot:trixie \
  bash -lc 'cd /build/u-boot &&
    make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- boox_c67ml_defconfig &&
    make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- -j$(nproc)'
```

## Flash to SD

```bash
sudo dd if=u-boot-rockchip.bin of=/dev/sdX seek=64 conv=fsync   # SPL+u-boot at sector 64
```

Sector 64 = LBA 64 on both SD and eMMC (the BootROM ignores the partition
table). Nothing here writes the internal NAND.

Rockchip BootROMs are generally reported as trying eMMC/SD before NAND, but the
order is **not verified for RK3026** — no public BROM disassembly exists and this
board boots vendor firmware from NAND today. Confirm on hardware first; if the
NAND loader wins, force maskrom (short the NAND CLK line at power-on) and push
the SPL over USB with `rkflashtool L`.

## Design notes / non-obvious choices

- **SPL runs from internal SRAM** (`SPL_TEXT_BASE=0x10081000`), not DRAM —
  RK292x family convention.
- **Back-to-BROM**: the mask ROM (not the SPL) loads U-Boot proper *into DRAM*
  (see `tools/rkcommon.c`), so the SPL must init DRAM before returning, and is
  otherwise deliberately tiny: debug UART, timer, `sdram_init()`, return
  (`rk3026-board-spl.c`). No MMC/block/DM/partition code in SPL (those drag in
  malloc/bounce-buffer and bloat it past the SRAM budget).
- **DDR init** (`arch/arm/mach-rockchip/rk3026/sdram_rk3026.c`) is ported from
  rk3036: same RK2928-generation die, same PCTL (0x20004000) / PHY (0x2000a000)
  / CRU / GRF / CPU_AXI addresses (vendor kernel `mach-rk2928/.../io.h`). The
  timing table is rk3036's 396 MHz DDR3 one and the die geometry in
  `sdram_config.c` is an assumption (512 MB = one 4 Gbit x16 die) — **neither is
  hardware-verified**; the stock kernel prints the real geometry over UART
  ("DDR DEBUG: %d CS, ROW=%d, Bank=%d, COL=%d, Total Capability=%dMB").
- **No ARM generic timer**: RK3026 is Cortex-A9, so `CONFIG_SYS_ARCH_TIMER` is
  off, the weak `rockchip_stimer_init()` is overridden (it writes CNTFRQ via
  `mcr p15, 0, c14, c0, 0`, undefined on A9), and the SPL carries a local
  `__udelay()` over the Rockchip timer. U-Boot proper uses `ROCKCHIP_TIMER`.
- **UART2 iomux** is GPIO2_C6/C7 **FUNC3** (vendor
  `mach-rk3026/include/mach/iomux.h`), which `board_debug_uart_init()` sets up
  because the raw debug-UART driver does no pinmux. FUNC2 on those same pins is
  `EBC_BORDER0/1` — the serial console and the EBC border pins are mutually
  exclusive.
- **`rk3026` was added to `tools/rkcommon.c`** (RK30 family, 0x1000 SPL load,
  header V1 — identical to rk3036) so `mkimage -T rksd -n rk3026` works.
- **clk driver**: PLL get/set switch is keyed on `PLL_APLL..PLL_GPLL` (DT ids
  0-3); the RK3026 MMC clock DT ids (31-36) would collide with a `0..63` range.
  The MMC mux is the RK3026-specific 1-bit cpll/gpll select (eMMC CON12 bit7
  div[14:8], SDMMC CON11) — see `cru_rk3026.h`. **Not hardware-verified**;
  validate with `mmc dev 0; mmc info` on the U-Boot prompt.
- **Storage**: the internal 4 GB flash is raw NAND behind a closed FTL — boot
  media is the SD card. See `../../../../../docs/HARDWARE-CONSTANTS.md`.

## Bring-up gate (real hardware)

UART2 (ttyS2, 115200 8N1). A U-Boot prompt confirms clocks + SPL + UART. Then
`mmc dev 0; mmc info; mmc read 0x60800000 0 1` validates the dwmmc/SD path and
the clk driver's MMC mux before booting a kernel.
