# Board I/O and watchdog

`board_io.c` drives the board-level I/O of phenix-1255-hw: PA enable, the
three status LEDs, the `COMM_SEL` switch and the factory reset button (SW2).
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
start.

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

## Watchdog

The windowed watchdog WWDT0 resets the SoC if it is not fed for 2 s. It is fed
every 250 ms by a thread with a priority below all streaming and network
threads, so a hang or a thread that keeps the CPU busy leads to a reset. After
the reset PA_EN is driven inactive again in the earliest init level. The
reset cause is logged on the console and reported in the web interface
(`last_reset`). The console command `phenix wdt_test` stops feeding to verify
the watchdog.
