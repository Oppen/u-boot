# U-Boot for the ONYX BOOX Robinson Crusoe 2 (RK3026)

Build and card layout are the C67ML's (`../boox-c67ml/README.md`):

```bash
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- boox_rc2_defconfig
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf-
sudo dd if=u-boot-rockchip.bin of=/dev/sdX seek=64 conv=fsync
```

On a cold start the mask ROM boots the microSD card before the internal
eMMC. The eMMC keeps the vendor firmware: this U-Boot neither describes
nor probes it, and nothing here writes it. Without the card the book
boots the vendor firmware as before.

The console is the USB CDC-ACM gadget; UART2 has no known connector.
