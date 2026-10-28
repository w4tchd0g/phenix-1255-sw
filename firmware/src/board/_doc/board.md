# Board I/O and watchdog

`board_io.c` drives the board-level I/O of phenix-1255-hw: PA enable, the
three status LEDs, the `COMM_SEL` switch and the factory reset button (SW2).
`hw_id.c` detects the hardware variant from the HW_ID pins.
`watchdog.c` runs the hardware watchdog. Pins:
[hardware.md](../../../../_doc/hardware.md#pin-assignment).

## PA enable

PA_EN drives the gate of a P-MOSFET; the PA supply is on while PA_EN is low.

- The firmware drives PA_EN high (PA off) in the `EARLY` init level, before
  clock setup and before any driver is initialized.
- PA_EN is switched on only when a TX stream is started and switched off when
  it is stopped.
- PA_EN is switched off immediately on loss of the host link and on stream
  errors.

Between power-on and the first firmware instruction, and while the MCU is
held in reset (e.g. by the debugger), the pin is high impedance. A pull-up
from the gate to the MOSFET source (VBAT3 side) on phenix-1255-hw keeps the
PA off in this window.

## Status LEDs

| LED | Off | On | Slow blink (1 Hz) | Fast blink (5 Hz) |
|---|---|---|---|---|
| LED0 red (ERR) | No error | Error, PA emergency shutdown | | Factory reset acknowledged |
| LED1 yellow (WARN/INFO) | | Fallback IP address in use | Waiting for DHCP | Factory reset acknowledged |
| LED2 green (OK) | | Host connected | Ready, no host connected | Factory reset acknowledged |

A short flash of LED1 indicates a late TX packet or an RX overflow.
LED0 blinks slowly after a watchdog reset until the next successful stream
start. The red LED of the FRDM-MCXN947 itself (P0_10) is used only on
unsupported hardware ([Hardware variant](#hardware-variant)).

## COMM_SEL switch

A switch on phenix-1255-hw connects P2_7 to GND or leaves it open. The pin
is sampled once at boot; changing the switch takes effect after the next
reset. Open (pulled up) selects HS USB, closed to GND selects Ethernet for
the control channel and the sample stream. Ethernet and the web
interface are active in both modes.

## Factory reset

Hold SW2 while the board starts (power-on or reset button) for at least
50 ms after reset. The settings partition is erased and all LEDs blink fast
for two seconds. The network configuration returns to the factory defaults
(DHCP on, fallback address 192.168.2.10/24, control port 1255/TCP, sample
port 1256/UDP), the display settings to theirs. The front panel menu offers
the same reset at runtime ([display.md](../../display/_doc/display.md#menu)).

## Hardware variant

The hardware ID is read from HW_ID0 (P3_5), HW_ID1 (P3_4) and HW_ID2 (P4_4);
HW_ID*n* is bit *n* of the ID. The internal pull-downs of the pins are
enabled, so an open pin reads 0 and a pin tied to +3V3 reads 1.
phenix-1255-hw has no HW_ID jumpers; its pins stay open and read as ID 0
(base). The same applies to µCell and compatible boards
([hardware.md](../../../../_doc/hardware.md#µcell-and-compatible-boards)).
Later boards set their ID with solder jumpers from these pins to +3V3.
`hw_id.c` reads the pins as the first step of `main()`, before any other
pin is configured, with a majority vote over five samples 5 ms apart.

All possible IDs (1 = pin tied to +3V3, 0 = open):

| HW_ID2 | HW_ID1 | HW_ID0 | ID | Variant | Hardware | Firmware |
|---|---|---|---|---|---|---|
| 0 | 0 | 0 | 0 | base | phenix-1255-hw | Supported |
| 0 | 0 | 1 | 1 | | not assigned | Start-up halted |
| 0 | 1 | 0 | 2 | | not assigned | Start-up halted |
| 0 | 1 | 1 | 3 | | not assigned | Start-up halted |
| 1 | 0 | 0 | 4 | | not assigned | Start-up halted |
| 1 | 0 | 1 | 5 | | not assigned | Start-up halted |
| 1 | 1 | 0 | 6 | | not assigned | Start-up halted |
| 1 | 1 | 1 | 7 | | not assigned | Start-up halted |

The variant is shown in the boot log, by `phenix status`, in the web
interface (`hardware` and `hw_id` in `/status.json`) and on the HARDWARE
page of the front panel. A new variant is added to the `variants` table in
`hw_id.c`.

On any other ID, or if the pins cannot be read, the firmware does not
start:

- The error and the supported IDs are printed on the console and repeated
  every 5 s.
- The red LED of the FRDM-MCXN947 (P0_10) blinks fast (5 Hz). The LEDs of
  phenix-1255-hw are not driven, because it is unknown what their pins are
  connected to on such hardware.
- The front panel shows a static error screen with the ID read and the
  supported IDs. The OLED keeps its image across an MCU reset, so the
  screen is always overwritten.
- Nothing else is initialized: no SX1255, stream, settings, network, USB,
  front panel thread or watchdog. PA_EN stays off from the early init
  ([PA enable](#pa-enable)); the factory reset button is not evaluated.
- The console stays usable. `phenix status` shows the detected ID; commands
  that would access the SX1255 or the settings are refused
  ([shell.md](../../shell/_doc/shell.md#phenix)).

## Watchdog

The windowed watchdog WWDT0 resets the SoC if it is not fed for 2 s. It is fed
every 250 ms by a thread with a priority below all streaming and network
threads, so a hang or a thread that keeps the CPU busy leads to a reset. After
the reset PA_EN is driven inactive again in the earliest init level. The
reset cause is logged on the console and reported in the web interface
(`last_reset`). The console command `phenix wdt_test` stops feeding to verify
the watchdog.
