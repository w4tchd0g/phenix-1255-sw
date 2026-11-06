# Hardware

## Overview

The phenix-1255-hw board carries a Semtech SX1255 transceiver with a
38.4 MHz reference oscillator, three status LEDs, a P-MOSFET with gate
pull-up that switches the PA supply (VBAT3), the front panel OLED display
and three buttons Left, OK and Right
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
| HS USB | Host link when *Host link* is set to USB (default) |
| Ethernet | Host link when *Host link* is set to Ethernet, web interface with both settings |

The host link is selected in the front panel menu, on the console or in the
web interface and takes effect after a reboot
([config.md](../firmware/src/config/_doc/config.md#host-link)).

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
| PA_EN | P1_23 (J9-23) | GPIO output, active low | P-MOSFET gate, switches VBAT3 |
| LED0 (red, ERR) | P0_14 | GPIO output, active high | |
| LED1 (yellow, WARN/INFO) | P0_22 | GPIO output, active high | |
| LED2 (green, OK) | P0_15 | GPIO output, active high | |
| FACTORY_RST (SW2) | P0_23 | GPIO input, pull-up, active low | Held during boot = factory reset |
| HW_ID0 | P3_5 | GPIO input, internal pull-down | Not connected on phenix-1255-hw (reads 0); reserved for a solder jumper to +3V3 on later boards, bit 0 of the hardware ID |
| HW_ID1 | P3_4 | GPIO input, internal pull-down | Not connected on phenix-1255-hw (reads 0); reserved for a solder jumper to +3V3 on later boards, bit 1 of the hardware ID |
| HW_ID2 | P1_19 (J9-5) | GPIO input, internal pull-down | Not connected on phenix-1255-hw (reads 0); reserved for a solder jumper to +3V3 on later boards, bit 2 of the hardware ID |
| FRDM red LED | P0_10 | GPIO output, active low | On the FRDM-MCXN947; blinks fast on unsupported hardware |
| OLED SDA | P1_0 | LPI2C3 SDA (FC3_P0) | Front panel display, see [display.md](../firmware/src/display/_doc/display.md) |
| OLED SCL | P1_1 | LPI2C3 SCL (FC3_P1) | |
| BTN0 (Left) | P2_0 (J3-1) | GPIO input, internal pull-down, active high | Button to +3V3 |
| BTN1 (OK) | P1_22 (J3-3) | GPIO input, internal pull-down, active high | Button to +3V3; long press = back |
| BTN2 (Right) | P4_12 (J4-1) | GPIO input, internal pull-down, active high | Button to +3V3 |

The devicetree description is in
[`firmware/boards/frdm_mcxn947_mcxn947_cpu0.overlay`](../firmware/boards/frdm_mcxn947_mcxn947_cpu0.overlay).

## Board functions

PA enable, status LEDs, the factory reset button, the hardware variant
(HW_ID jumpers) and the watchdog: [board.md](../firmware/src/board/_doc/board.md). Front panel
display and buttons: [display.md](../firmware/src/display/_doc/display.md).
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
- The OLED display and the three buttons can be added with jumper wires
  to the FRDM-MCXN947 headers (buttons to +3V3).
- Indicator LEDs on the SX1255 board (e.g. the µCell RX/TX LEDs) are not
  driven.

At 600 kHz sample rate the I2S bit clock is 38.4 MHz. Signal integrity over
jumper wires is a known limitation of this setup.
