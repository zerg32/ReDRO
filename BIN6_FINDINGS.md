# BIN6 Protocol Findings — Shahe Scale (Direct-Wire, No 74HC14)

## Hardware
- **Scale:** Shahe BIN6 linear scale (capacitive, ~300mm travel)
- **Connection:** CLK→GPIO27, DATA→GPIO22, **no 74HC14** (74HC14 removed)
- **Signal levels:** 0–1.5V, ESP32 GPIOs configured as `INPUT` (no pull-up)
- **Board:** ESP32-2432S028R (CYD), PlatformIO espressif32 6.9.0, framework arduino

## Protocol

### Electrical
- **Idle:** CLK HIGH, DATA HIGH (~1.5V)
- **Bit read:** on **RISING edge** of CLK
- **Bit order:** **LSB-first** (bit 0 transmitted first)
- **Packet size:** 24 bits
- **Packet rate:** ~10 Hz (~100ms inter-packet gap)
- **Per-packet time:** ~2.4ms

### Bit Map (Sign-Magnitude)

| Bits | Field | Description |
|------|-------|-------------|
| 0–19 | Magnitude | Unsigned 20-bit counter, **100 counts/mm** (0.01mm resolution). 2540 counts/inch in metric mode. |
| 20 | Sign | **1 = positive**, 0 = negative (direction from scale body) |
| 21–23 | Flags/status | Typically unused in normal operation |

### Formula (Sign-Magnitude + Tare)

```c
int32_t lower20 = raw & 0xFFFFF;               // magnitude
int32_t sign   = ((raw >> 20) & 1) ? 1 : -1;   // sign bit
int32_t counts = sign * lower20;                // signed count
float    mm     = (counts - tare_counts) / 100.0f;  // position relative to tare
```

- **`tare_counts`**: captured at the user's zero position (via `z` command or button)
- **Resolution:** 100 counts/mm (= 2540 counts/inch)

### Key Points
- The scale uses **sign-magnitude** encoding, NOT unsigned + page counter as we initially assumed.
- Bit 20 is the **sign bit**: 1 = positive, 0 = negative.
- Bits 21–23 are flags/status (ignored).
- The apparent "page transition" was actually the **sign bit flipping** when the scale's internal counter crosses zero.
- The `PAGE_OFFSET = 266` from our earlier model was a consequence of `2 × zero_lower20` — the difference between the sign-magnitude representation and unsigned representation when crossing zero.
- **Tare** (not zero-offset subtraction) is the correct way to set the reference point, matching published Arduino example code.

### Verified Readings

| Display | Raw | lower20 | bit20 | Counts | Tare | Result | Match |
|---------|-----|---------|-------|--------|------|--------|-------|
| 0.00mm (tare set) | 133 | 133 | 0 | -133 | -133 | 0mm | ✓ |
| 50.03mm | 1,053,446 | 4,870 | 1 | +4,870 | -133 | 50.03mm | ✓ |
| 100.00mm | 1,058,443 | 9,867 | 1 | +9,867 | -133 | 100.00mm | ✓ |

### Behavior at Zero Crossing
When the scale passes through its internal zero point:
- Raw value has a discontinuity (e.g., `543` → `1,049,852`)
- Magnitude wraps from 543 to 1,276
- Sign bit flips from 0 to 1
- Effective counts are continuous: `-543`→`+1,276` (1,819 count = 18.19mm step → actual travel)
- Tare subtraction makes the displayed position continuous

## Reader Implementation
- **Polling** (blocking `readScale()`): waits for 24 RISING edges, LSB-first, returns 24-bit raw value.
- **Sign-magnitude decode**: extract lower20 + bit20, compute `sign * lower20 - tare`.
- **Serial zero**: send `z` over UART (115200 baud) to tare at current position.

## TouchDRO Bluetooth SPP
- **BT name:** "CYD Lathe DRO"
- **Phone pairing:** standard Bluetooth pairing (PIN 0000 or 1234)
- **Protocol:** `x<counts>;y0;z0;w0;\n` at 25 Hz
- **TouchDRO Scale Factor:** **100 counts/mm**
- **Library:** `BluetoothSerial` (bundled with ESP32 Arduino core)

## Comparison with `shahe_5403_protocol.md`

| Aspect | `shahe_5403_protocol.md` | This scale | |
|--------|--------------------------|------------|---|
| Bit order | LSB-first | LSB-first | ✓ matches |
| Edge | FALLING | RISING | ✗ (may be due to 74HC14 inversion) |
| Bits 0–19 | Magnitude (unsigned) | Magnitude (unsigned) | ✓ matches |
| Bit 20 | Sign (0=positive) | Sign (1=positive) | ✗ polarity flipped |
| Bits 21–23 | Status flags | Status flags | ✓ matches |
| Resolution | 10µm (0.01mm) | 100 counts/mm (0.01mm) | ✓ equivalent |
| Sign format | Sign-magnitude | Sign-magnitude | ✓ confirmed |
| Reading edge | FALLING | RISING | ✗ (hardware inversion) |

The `shahe_5403_protocol.md` flip-flop on bit 20 polarity (0=positive vs 1=positive) is likely due to the 74HC14 signal inversion — with the 74HC14 removed, we see the **native format**: sign=1 = positive.

## File Locations
- **Working reader:** `/home/serg/cyddro/minimal_test/src/main.cpp` (sign-magnitude + tare + BT)
- **Old spec:** `/home/serg/cyddro/shahe_5403_protocol.md` (bit 20 polarity reversed)
- **DRO-fork ISR reader:** `/home/serg/cyddro/DRO-fork/src/bin6_reader.h` (needs update)
- **TouchDRO BT plan:** `/home/serg/cyddro/FORK_PLAN.md` (BT protocol documented)
