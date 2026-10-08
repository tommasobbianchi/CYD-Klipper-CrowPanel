# BTT Panda Touch / K-Touch port

ESP32-S3, 800x480 RGB panel (DE mode), GT911 touch. Build env: `esp32-PANDATOUCH`.

```
pio run -e esp32-PANDATOUCH
esptool --chip esp32s3 -b 460800 write-flash --flash-mode dio --flash-size 16MB --flash-freq 80m \
  0x0 bootloader.bin 0x8000 partitions.bin 0xe000 boot_app0.bin 0x10000 firmware.bin
```

The first flash needs all four images (the stock partition table differs); later ones only `0x10000 firmware.bin`.
USB is a CH340 behind the USB-C port, so auto-reset works. A weak USB port browns out at full backlight.

## Seeding WiFi + Moonraker (optional)

The board has no usable on-screen input until WiFi is set, and the serial `ssid` command cannot take names with spaces.
Create an untracked `src/core/device/pt_secrets.h`:

```c
#define PT_WIFI_SSID "my network"
#define PT_WIFI_PASS "my password"
```

On first boot (blank NVS: `esptool erase-region 0x9000 0x5000`) the firmware seeds WiFi and a Klipper printer at
`192.168.0.100:7125` with brightness 80. Edit those defaults in `src/conf/global_config.cpp`. The file is read with
`__has_include`, so builds without it are unaffected.

## Hardware notes (measured)

| What | Value |
|---|---|
| RGB data d0..d15 | B[17,18,48,47,39] G[11..16] R[6..10], pclk 5, DE 38, HSYNC 42, VSYNC 45 (unrouted) |
| LCD reset | GPIO46, pulse low then high before `tft.begin()` |
| Backlight | GPIO21 (PWM) |
| Touch | GT911 at 0x5D, SDA 2 / SCL 1 / INT 40 / RST 41. LovyanGFX's GT911 path gave no events; read over `Wire` instead |
| Pixel clock | 16 MHz clean. 18 MHz and above shifts, 14 MHz loses sync |
| Framebuffer | `use_psram = 2` (PSRAM only); 1 is half/half and shifts |
| Orientation | panel is mounted 180 deg: `setRotation(rotate_screen ? 0 : 2)`; GT911 coordinates already match |
| Build flags | `-DBOARD_HAS_PSRAM` is required or the PSRAM framebuffer cannot be allocated |
| Fonts | 32 px body / 24 px small, enabled for this driver in `lv_conf.h` |
