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

The Rockchip BootROM tries SD before the internal NAND, so an SD with the SPL
at sector 64 boots without touching the vendor bootloader on NAND.

## Design notes / non-obvious choices

- **SPL runs from internal SRAM** (`SPL_TEXT_BASE=0x10081000`), not DRAM —
  RK292x family convention.
- **Back-to-BROM**: the mask ROM (not the SPL) loads U-Boot proper, so the SPL
  is deliberately tiny — it only inits the debug UART + timers and returns
  (`rk3026-board-spl.c`). No MMC/block/DM/partition code in SPL (those drag in
  malloc/bounce-buffer and bloat it past the SRAM budget). DDR is trained by
  the ROM's ddrbin before SPL re-entry.
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
