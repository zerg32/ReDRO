# Technical Specification: Shahe 5403 Digital Scale Protocol

This document provides a comprehensive technical breakdown of the hardware interface, electrical behavior, and communication protocol for the **Shahe 5403** (and 5403F) Digital Linear Readout Scale. The scale utilizes a variant of the synchronous serial communication standard frequently designated as the **BIN6 (24-bit) protocol** in digital readout (DRO) reverse-engineering communities.

---

## 1. Physical Interface & Pinout

The Shahe 5403 scale head uses a standard **Mini-USB 5-pin** receptacle. However, **this port is not USB-compliant**. Connecting this port directly to a standard USB host (such as a PC or charger) provides 5V to a 3V max circuit and will permanently destroy the reading head's encoder ASIC.

### Connector Layout
```
      _______
     / 1 2 3     |  4   5  |
    +---------+
 (Front view of female 
  Mini-USB receptacle)
```

### Pin Assignment Matrix

| Mini-USB Pin | Wire Color (Typical) | Signal Name | Type | Description |
| :---: | :--- | :--- | :---: | :--- |
| **1** | Red | **VCC** | Power | +3.0V DC Input (Stable power rail) |
| **2** | Black | **GND / Shield** | Power | Ground Reference & Casing Shield |
| **3** | White or Yellow | **Clock (CLK)** | Output | Synchronous Clock output driven by the scale head |
| **4** | Green | **Data (DAT)** | Output | Synchronous Serial Data output driven by the scale head |
| **5** | N/C | **No Connect** | — | Internally isolated (sometimes bonded to shield) |

---

## 2. Electrical Specifications

Unlike older 1.5V button-cell calipers, the Shahe 5403 reading head runs off a 3V source (typically derived from two external AAA batteries or a stable linear regulator inside an attached display console).

* **Operating Voltage ($V_{CC}$):** 3.0V DC nominal ($2.8	ext{V} - 3.3	ext{V}$ range).
* **Signal Logic Levels:** Although powered by 3.0V, the internal ASIC drives the Clock and Data signals with an output voltage swing of approximately **1.5V to 1.8V peak-to-peak ($V_{PP}$)**.
* **Microcontroller Interfacing Requirement:** Because the high logic state peaks at only $\sim1.5	ext{V}$, connecting these lines directly to a standard 5V Arduino (e.g., Uno/Mega) will fail to trigger the internal logic thresholds reliably (which require $>0.6 	imes V_{CC}$, or $3.0	ext{V}$). A logic-level shifter, a comparator, or a Schmidt-trigger buffer configured for low-threshold detection must be placed between the scale and the master microcontroller.
* **Communication Type:** Simplex / Uni-directional. The scale acts exclusively as a master transmitter, continuously streaming its current position. It cannot receive commands from a connected device.

---

## 3. The BIN6 Protocol Structure

The protocol updates continuously at a refresh frequency of roughly **9.5 Hz to 9.6 Hz** (one frame transmitted approximately every 104 ms). Each transmission frame consists of a single **24-bit stream** chunked into six 4-bit nibbles. 

### Data Framing
* **Idle State:** When not transmitting, both `Clock` and `Data` lines default to a `HIGH` logic level ($\sim1.5	ext{V}$).
* **Bit Synchronization:** Data bits are shifted out sequentially and must be sampled on the **falling edge** of the Clock signal.
* **Bit Order:** Least Significant Bit (LSB) first. Bit 0 is the first bit shifted out; Bit 23 is the final bit.

```
       ___     ___     ___           ___
CLK  _|   |___|   |___|   |___...___|   |_____ (Idle High)
          ^       ^       ^             ^
        Bit 0   Bit 1   Bit 2         Bit 23
         LSB                           MSB
```

### Bitstream Field Mapping

The 24 bits are allocated across the following structure:

$$egin{array}{|c|c|c|c|c|c|}
\hline
	extbf{Nibble 1} & 	extbf{Nibble 2} & 	extbf{Nibble 3} & 	extbf{Nibble 4} & 	extbf{Nibble 5} & 	extbf{Nibble 6} \
	ext{(Bits 0–3)} & 	ext{(Bits 4–7)} & 	ext{(Bits 8–11)} & 	ext{(Bits 12–15)} & 	ext{(Bits 16–19)} & 	ext{(Bits 20–23)} \ \hline
	ext{Data [0:3]} & 	ext{Data [4:7]} & 	ext{Data [8:11]} & 	ext{Data [12:15]} & 	ext{Data [16:19]} & 	ext{Flags / Sign} \ \hline
\end{array}$$

#### 1. Magnitude Fields (Bits 0 to 19)
The first 20 bits are interpreted directly as a raw, unsigned binary integer representing the linear position value. 
* **Native Scale Resolution:** 10 microns ($0.01	ext{ mm}$ per unit step). 
* **Metric Invariance:** The raw bitstream output from the Shahe 5403 scale head **always** remains in metric units ($0.01	ext{ mm}$ integers), regardless of whether the physical buttons on the reading head are toggled to display inches or millimeters. Unit conversion is entirely handled downstream by the original LCD display unit.

#### 2. Flag & Sign Field (Bits 20 to 23 / Nibble 6)
* **Bit 20 (Sign Bit):** Indicates the directional polarity of the scale offset.
  * `0` = Positive displacement ($+$).
  * `1` = Negative displacement ($-$).
  * *Implementation Note:* The scale uses a **Sign-Magnitude** format, not Two's Complement. If the position is $-15.42	ext{ mm}$, the raw magnitude bits (0-19) represent the exact positive integer `1542`, while Bit 20 is toggled to `1`.
* **Bit 21, 22, 23 (Status Flags):** Typically fixed or used for rapid internal status diagnostics. In standard operating states on the 5403, these bits remain static.

---

## 4. Mathematical Conversion Formula

To parse the 24-bit frame into a human-readable format inside your firmware, follow this logical flow:

1. Extract the raw integer from the lower 20 bits:
   $$X_{	ext{raw}} = \sum_{i=0}^{19} 	ext{Bit}(i) \cdot 2^i$$

2. Calculate the baseline measurement in millimeters:
   $$	ext{Measurement (mm)} = X_{	ext{raw}} 	imes 0.01$$

3. Apply structural sign correction checking Bit 20:
   $$	ext{Final Value} = egin{cases} 
   	ext{Measurement (mm)}, & 	ext{if Bit 20} = 0 \
   -	ext{Measurement (mm)}, & 	ext{if Bit 20} = 1 
   \end{cases}$$

4. Optional Imperial Conversion:
   $$	ext{Final Value (inches)} = 	ext{Final Value (mm)} 	imes 0.0393700787$$

---

## 5. Timing Parameters
For precise state-machine tracking or interrupt-driven capture on external microcontrollers, use the following timing baselines:

| Parameter | Symbol | Min | Typ | Max | Unit |
| :--- | :---: | :---: | :---: | :---: | :---: |
| **Clock Pulse Width (Low / High)** | $T_{	ext{clk}}$ | 40 | 50 | 60 | $\mu	ext{s}$ |
| **Bit-to-Bit Period** | $T_{	ext{bit}}$ | 80 | 100 | 120 | $\mu	ext{s}$ |
| **Total Packet Duration** | $T_{	ext{frame}}$ | 2.0 | 2.4 | 2.8 | $	ext{ms}$ |
| **Inter-packet Quiet Interval (Sleep)** | $T_{	ext{idle}}$ | 95 | 100 | 110 | $	ext{ms}$ |
