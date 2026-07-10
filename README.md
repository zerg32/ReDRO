# ReDRO

ESP32-based digital readout for Shahe BIN6 linear scales, running on a
Cheap Yellow Display (ESP32-2432S028R) with LovyanGFX-driven ST7789 panel,
XPT2046 resistive touch, and Bluetooth SPP for TouchDRO.

## Hardware

| Component | Connection |
|-----------|------------|
| **CYD** | ESP32-2432S028R Dual USB (USB-C + micro USB) |
| **Display** | ST7789 on HSPI: MISO=12, MOSI=13, SCLK=14, CS=15, DC=2, BL=21 |
| **Touch** | XPT2046 on software SPI: SCLK=25, MOSI=32, MISO=39, CS=33 |
| **Scale 1** | Shahe BIN6: CLK=27, DATA=22, no 74HC14, direct 1.5V to GPIO |
| **Scale 2** | Shahe BIN6: CLK=16, DATA=17 (RGB LED pins), direct 1.5V to GPIO |

The 74HC14 line driver was removed. The scale's 0–1.5 V signal is wired
straight to ESP32 GPIOs in INPUT mode (no pull-ups).

## Features

- Reads 1–2 Shahe BIN6 scales at the native 24-bit packet rate (~10 Hz)
- Sign-magnitude decode: bits 0–19 = magnitude, bit 20 = sign (1=positive)
- 100 counts/mm resolution (0.01 mm)
- Live mm display updates only on value change (no flicker)
- Tare / zero via touch button (right-edge "0") — works correctly with reversed axes
- Per-axis settings (Direction, Enabled, X Double) stored in NVS, accessed via touch menu
- Settings menu with touch navigation: gear button → axis select → per-axis config page
- Bluetooth SPP "ReDRO" sends raw (unmodified) counts to TouchDRO
- 15 µs timer-based polling filters touch-induced noise on scale lines

## Scale Protocol (BIN6)

The Shahe BIN6 uses a synchronous 1-wire-data + clock protocol:

- **Idle**: CLK HIGH, DATA HIGH (~1.5 V)
- **Bit read**: on the RISING edge of CLK
- **Bit order**: LSB-first (bit 0 transmitted first)
- **Packet**: 24 bits, repeated at ~10 Hz

| Bits | Field |
|------|-------|
| 0–19 | Magnitude (unsigned, 100 counts/mm) |
| 20   | Sign (1 = positive, 0 = negative) |
| 21–23| Flags (unused) |

Decoded as `counts = sign ? magnitude : -magnitude`.

## Software

Built with PlatformIO (`espressif32 @ 6.9.0`, Arduino framework).

### Core libraries

- **LovyanGFX** — display driver for ST7789 on HSPI
- **BluetoothSerial** — ESP32 built-in BT SPP for TouchDRO

### Timer-based scale polling

The scale is read by a hardware timer (prescaler 80 → 1 µs tick, alarm every
15 µs). The ISR samples `GPIO.in` directly for both CLK and DATA pins
simultaneously, detecting rising clock edges and accumulating bits LSB-first
into a 24-bit shift register.

This replaces a RISING-edge GPIO ISR, which suffered glitches when the
XPT2046 touch controller (software SPI on nearby pins) coupled fast edges
into the scale lines. By polling at a fixed 15 µs interval, scale reads are
decoupled from touch timing.

The polling timer is disabled during the settings menu to avoid noise while
drawing screens. Poll state (`prev`, `code`, `bits`) is stored in global
`PollState` structs so it can survive timer stop/start.

### Display init sequence

1. Configure HSPI bus (pins 13/12/14, DC=2) at 55 MHz write / 16 MHz read
2. Set up backlight PWM on GPIO 21
3. Attach ST7789 panel with CS=15
4. Attach XPT2046 touch on software SPI (pins 25/32/39/33, `spi_host = -1`)
5. Call `display.init()`, set rotation 1 (landscape)

### Display layout

- DRO mode: Z axis line at y=85, X axis (if enabled) at y=125
- Text drawn centered (middle_center datum) within a 250×30 px area
- Each line shows the label, colon, and value padded with `%6.2f` so
  positive/negative values never shift the text
- A rectangular "0" button (40 px wide) at the right edge for each axis
- A gear button in the top-right corner opens the settings menu
- Screen redraw only on value change, rate-limited to ~20 Hz

### Tare / Zero

Tapping the "0" button for an axis zeros it. The tare is stored as a raw
count offset and subtracted *before* axis configuration (direction reversal)
is applied. This ensures zeroing works identically regardless of whether the
axis is reversed.

BT always transmits raw (unmodified, pre-tare, pre-config) counts.

### Settings Menu

The gear button opens a per-axis settings system:

1. **Axis selection** — choose Z or X
2. **Per-axis config page**:
   - **Direction**: Normal / Reversed (inverts display sign)
   - **Enabled**: On / Off (hides disabled axis from display; counts still
     polled but not shown)
   - **X Double**: Off / On (when on, displayed X counts are ×2; no effect
     on BT output or Z axis)

Settings are saved to NVS (Preferences, namespace `"dro"`) on exit and
restored on boot. Keys: `z_rev`, `z_en`, `x_rev`, `x_en`, `x_x2`.

### Bluetooth SPP

- **BT name**: `ReDRO`
- **Frame format**: `x<scale1>;y0;z0;w0;\n` (one scale) or
  `x<scale1>;y<scale2>;z0;w0;\n` (two scales), sent on each scale update
- **Always raw**: BT packets contain unprocessed counts (no tare, no
  reversal, no doubling) so TouchDRO applies its own scaling
- **TouchDRO scale factor**: 100 counts/mm

## Building and Flashing

```bash
cd minimal_test
pio run -t upload --upload-port /dev/ttyUSB0
```

Monitor serial with:

```bash
pio device monitor
```

## Usage

1. Power the CYD via USB-C or micro USB
2. The display shows current positions as `Z:  12.34 mm` and `X:  56.78 mm`
3. Tap the "0" button next to an axis to zero it
4. Tap the gear (top-right) to open settings:
   - Choose an axis → toggle Direction, Enabled, or X Double
   - Changes take effect immediately; tap the back arrow to return
5. Pair with TouchDRO over Bluetooth ("ReDRO") — standard PIN 0000
6. Set TouchDRO scale factor to 100 counts/mm

## Pin Map

```
CYD GPIO    Function
─────────────────────────────
 2          TFT DC
 4          RGB LED Red
12          TFT MISO
13          TFT MOSI
14          TFT SCLK
15          TFT CS
16          Scale 2 DATA (optional) / RGB LED Green
17          RGB LED Blue
19          SD MISO (unused)
21          TFT Backlight
 22          Scale 1 DATA
 25          Touch SCLK (software SPI)
 26          Speaker / DAC (free)
 27          Scale 1 CLK
 16          Scale 2 CLK / RGB LED Green
 17          Scale 2 DATA / RGB LED Blue
32          Touch MOSI (software SPI)
33          Touch CS (software SPI)
39          Touch MISO (software SPI)
```

## Files

| File | Purpose |
|------|---------|
| `src/main.cpp` | Main firmware |
| `platformio.ini` | Build configuration |
| `README.md` | This file |

## References

- [LovyanGFX](https://github.com/lovyan03/LovyanGFX) — display library
- [FluidDial](https://github.com/bdring/FluidDial) — reference CYD config
- [TouchDRO](https://github.com/jacobschaer/TouchDRO) — Android DRO app
