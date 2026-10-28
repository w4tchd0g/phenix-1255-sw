# Hardware

## Overview

The phenix-1255-hw board carries a Semtech SX1255 transceiver with a
38.4 MHz reference oscillator, three status LEDs, the `COMM_SEL` switch,
three HW_ID solder jumpers for the hardware variant, a P-MOSFET with gate pull-up that switches the PA supply (VBAT3), the front
panel OLED display and a rotary encoder with push button
([display.md](../firmware/src/display/_doc/display.md)). It is connected
to an NXP FRDM-MCXN947. A µCell board or another SX1255 board built the same
way can be used instead with a reduced feature set, see
[µCell and compatible boards](#µcell-and-compatible-boards).

phenix-1255-hw was developed by Z32IT as part of the joint Phenix-1255
project. Schematic, board images, Gerber files and BOM are in the hardware
repository [Phenix-1255-HW](https://github.com/labdvm/Phenix-1255-HW).

| FRDM-MCXN947 connector | Use |
|---|---|
| J17 (MCU-Link) | Flashing, debug, console |
| HS USB | Host link when `COMM_SEL` is high |
| Ethernet | Host link when `COMM_SEL` is low, web interface in both modes |

## Pin assignment

| Signal | MCU pin | Function | Notes |
|---|---|---|---|
| CK_OUT | P3_18 | SAI1_RX_BCLK | I2S bit clock from the SX1255 |
| DIO2 / WS | P3_19 | SAI1_RX_FS | I2S frame sync from the SX1255 |
| I_OUT | P3_21 | SAI1_RXD0 | RX samples |
| I_IN | P3_20 | SAI1_TXD0 | TX samples |
| SCK | P0_25 | LPSPI1 SCK (FC1_P1) | |
| SDI (MOSI) | P0_24 | LPSPI1 SDO (FC1_P0) | |
| SDO (MISO) | P0_26 | LPSPI1 SDI (FC1_P2) | |
| NSS | P0_31 | GPIO chip select, active low | Hardware PCS on P0_27 is not used (on-board green LED) |
| RESET | P0_29 | GPIO | Idle high impedance, pulse high >= 100 us |
| DIO0 | P0_28 | GPIO input | |
| DIO1 | P0_30 | GPIO input | |
| DIO3 | P0_19 | GPIO input | |
| PA_EN | P2_4 | GPIO output, active low | P-MOSFET gate, switches VBAT3 |
| LED0 (red, ERR) | P0_14 | GPIO output, active high | |
| LED1 (yellow, WARN/INFO) | P0_22 | GPIO output, active high | |
| LED2 (green, OK) | P0_15 | GPIO output, active high | |
| COMM_SEL | P2_7 | GPIO input, internal pull-up | High (open) = HS USB, low = Ethernet |
| FACTORY_RST (SW2) | P0_23 | GPIO input, pull-up, active low | Held during boot = factory reset |
| HW_ID0 | P3_5 | GPIO input, internal pull-down | Solder jumper to +3V3, bit 0 of the hardware ID |
| HW_ID1 | P3_4 | GPIO input, internal pull-down | Solder jumper to +3V3, bit 1 of the hardware ID |
| HW_ID2 | P4_4 | GPIO input, internal pull-down | Solder jumper to +3V3, bit 2 of the hardware ID |
| FRDM red LED | P0_10 | GPIO output, active low | On the FRDM-MCXN947; blinks fast on unsupported hardware |
| OLED SDA | P1_0 | LPI2C3 SDA (FC3_P0) | Front panel display, see [display.md](../firmware/src/display/_doc/display.md) |
| OLED SCL | P1_1 | LPI2C3 SCL (FC3_P1) | |
| Encoder A | P1_19 | GPIO input, internal pull-up | |
| Encoder B | P1_18 | GPIO input, internal pull-up | |
| Encoder push button | P0_11 | GPIO input, pull-up, active low | |
| SW3 | P0_6 | GPIO input, pull-up, active low | Steps the display pages; ISP mode select only during reset |

The devicetree description is in
[`firmware/boards/frdm_mcxn947_mcxn947_cpu0.overlay`](../firmware/boards/frdm_mcxn947_mcxn947_cpu0.overlay).

## Board functions

PA enable, status LEDs, the `COMM_SEL` switch, the factory reset button, the
hardware variant (HW_ID jumpers) and the watchdog: [board.md](../firmware/src/board/_doc/board.md). Front panel
display and encoder: [display.md](../firmware/src/display/_doc/display.md).
Memory layout: [firmware.md](../firmware/_doc/firmware.md#memory).

## µCell and compatible boards

The first tests were made with a µCell (MU-Cell) board wired to the
FRDM-MCXN947 with jumper wires of about 10 cm length. Streaming over HS USB
and Ethernet, timed TX, the loopback test and over-the-air operation with
FlowStation work on this setup.

Other SX1255 boards built the same way can be used like the µCell. They
need:

- a Semtech SX1255 with its I2S interface (CK_OUT, DIO2/WS, I_OUT, I_IN),
  SPI (SCK, SDI, SDO, NSS) and RESET accessible, wired as in the
  [pin assignment](#pin-assignment);
- a 38.4 MHz reference oscillator. The sample rates are integer fractions of
  the reference, so another frequency changes them and must be set as
  `clock-frequency` of the `sx1255` node in the devicetree overlay.

Compared to phenix-1255-hw the feature set of such a setup is reduced:

- DIO0, DIO1, DIO3, PA_EN and LED0 to LED2 are usually not connected. An
  external PA is then not switched by the firmware.
- The OLED display and the rotary encoder can be added with jumper wires;
  without an encoder, SW3 steps the display pages.
- Indicator LEDs on the SX1255 board (e.g. the µCell RX/TX LEDs) are not
  driven.
- `COMM_SEL` is set with a wire bridge from P2_7 to GND.
- The HW_ID pins stay unconnected; the internal pull-downs make them read
  as the base variant ([board.md](../firmware/src/board/_doc/board.md#hardware-variant)).

At 600 kHz sample rate the I2S bit clock is 38.4 MHz. Signal integrity over
jumper wires is a known limitation of this setup.
