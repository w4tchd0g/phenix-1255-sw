# Testing

All tests run against real hardware. Tests that transmit must only be used
with an attenuator or dummy load between TX and RX (or with antennas on a
frequency you are allowed to transmit on, e.g. as a licensed radio amateur in
the 70 cm band).

## Test setup

- FRDM-MCXN947 with the phenix-1255-hw board (or a µCell or compatible
  board, see [hardware.md](hardware.md#µcell-and-compatible-boards)),
  MCU-Link console connected.
- Host link: HS USB (`COMM_SEL` open) or Ethernet (`COMM_SEL` to GND).
- For the transmitting tests: SX1255 TX output connected to the RX input
  through an attenuator of about 20 dB.
- SoapySDR module built and found by SoapySDR:
  `export SOAPY_SDR_PLUGIN_PATH="$PWD/build/soapy"` (see
  [building.md](building.md)).

Device arguments in the examples: `driver=phenix-1255` (USB or mDNS) or
`driver=phenix-1255,addr=<address>` (Ethernet).

## phenix-1255-hw bring-up

Run these checks once on every new phenix-1255-hw board or board revision,
before the other tests. Steps marked *transmits* need the attenuator or a
dummy load on the TX output and on the PA output.

| Step | Procedure | Expected |
|---|---|---|
| Boot | Power on, watch the console | No errors in the boot log; `phenix status` shows `hardware: base (HW_ID2..0 = 000)`, `pa: disabled` and `comm_sel` matching the switch |
| Unsupported hardware | Connect P3_5 (HW_ID0) to +3V3, reset; remove the connection afterwards | Console repeats `Unsupported hardware: HW_ID2..0 = 001` every 5 s, red FRDM LED blinks fast, no LED of phenix-1255-hw lit, display shows `HARDWARE ERROR` and `HW ID 1 (001)`, no network or USB device; `phenix status` shows `start-up: halted`, `sx1255` commands are refused |
| SX1255 | `sx1255 info` | Version read back, reference 38400000 Hz, RX and TX PLL locked |
| PA off at idle | Measure P2_4 and the PA supply (VBAT3 after the MOSFET) after boot | P2_4 high, PA supply off |
| PA off in reset | Hold the reset button; halt the MCU with the debugger | PA supply stays off (gate pull-up) |
| PA on with TX (*transmits*) | Run `phx_soapy_test tone`, measure as above | P2_4 low and PA supply on during the test, `phenix status` shows `pa: enabled`; PA supply off after the test |
| PA shutdown paths (*transmits*) | Repeat the rows of [Link loss and safety](#link-loss-and-safety) that end with "PA off", measuring the PA supply | PA supply off at the time stated there |
| Status LEDs | `phenix led <err\|warn\|ok> on`, then `off`, for each LED; reboot afterwards | Each command lights the LED of the colour given in [board.md](../firmware/src/board/_doc/board.md#status-leds) |
| LED states | Boot without host, then connect a host | Green blinks slowly, then lights steadily |
| COMM_SEL | Boot with the switch open and closed | `phenix status` reports HS USB and Ethernet respectively |
| DIO0/1/3 | `sx1255 info` | `dio` line shows 0 or 1 for each pin (a negative value means the pin is not configured) |
| Encoder | Turn one detent clockwise, one counter-clockwise, press | One page step per detent in the expected direction (else see [display.md](../firmware/src/display/_doc/display.md#hardware)), press selects |
| Front panel | Rows of [Front panel](#front-panel) | As stated there |
| Signal integrity | `phx_soapy_test rx` at 600 kHz for 10 minutes, then `stream status` | `PASS`, 0 frame sync errors, 0 FIFO errors |
| Loop delay (*transmits*) | `phx_loopback_test "driver=phenix-1255" all 20 5` over USB and Ethernet | `PASS` with the delays of the table in [stream.md](../firmware/src/stream/_doc/stream.md#tx-to-rx-loop-delay); update the table if the board gives other, stable values |
| Current draw (*transmits*) | Measure the USB supply current during full duplex at 600 kHz | Set `USB_MAX_POWER` in `firmware/src/usb/transport_usb.c` and [usb.md](../firmware/src/usb/_doc/usb.md) accordingly |

## Test programs

| Program | Transport | Transmits | Checks |
|---|---|---|---|
| `SoapySDRUtil --find` / `--probe` | both | no | Discovery, device information |
| `SoapySDRUtil --rate=600e3 --direction=RX` | both | no | RX throughput |
| [`phx_soapy_test`](../soapy/tests/_doc/tests.md#phx_soapy_test) `rx` | both | no | RX timestamps present and contiguous, `getHardwareTime()` |
| [`phx_soapy_test`](../soapy/tests/_doc/tests.md#phx_soapy_test) `tone` | both | yes | Timed TX in 1.5 ms blocks, tone in the RX stream |
| [`phx_loopback_test`](../soapy/tests/_doc/tests.md#phx_loopback_test) | both | yes | Burst at sample *N* always at the same RX position |
| [`tools/phx_client.py`](../tools/_doc/phx_client.md) | Ethernet | `tone`, `burst`, `late`, `timeout` | Protocol: late/early TX, bursts, link timeout |

### Recommended run

```sh
SoapySDRUtil --probe="driver=phenix-1255"
build/soapy/phx_soapy_test rx "driver=phenix-1255" 10
for r in 600000 300000 150000 75000 50000 25000; do
  build/soapy/phx_soapy_test tone "driver=phenix-1255" 5 $r $((r / 10))
done
build/soapy/phx_loopback_test "driver=phenix-1255" all 20 5
```

Expected results:

- `phx_soapy_test`: `PASS`, 0 reads without `HAS_TIME`, 0 discontinuities,
  timestamp rate equal to the sample rate.
- `phx_loopback_test`: `PASS` with one delay per rate (see the table in
  [stream.md](../firmware/src/stream/_doc/stream.md#tx-to-rx-loop-delay)).
- `stream status` on the console afterwards: 0 FIFO errors, 0 frame sync
  errors, 0 dropped RX samples, 0 late/early TX packets.
- Full duplex over Ethernet with FlowStation or TETRA-Bluestation (transmits):
  0 dropped RX samples, including the stream start. Late TX packets occur
  only where the station itself reports `Too late to produce TX block`
  (host scheduling), mostly at the start.

## Link loss and safety

| Test | Procedure | Expected |
|---|---|---|
| Host process killed | Start a `tone` test, kill the process (`kill -9`) | Ethernet: PA off at once (TCP closed); USB: PA off after 1 s (link timeout) |
| USB cable removed | Remove the HS USB cable during a `tone` test | `USB deconfigured`, PA off at once; device re-enumerates when plugged in again |
| New host within 1 s (USB) | Restart a test immediately after killing it | Session restarts (`HELLO within a session`), test passes |
| Second host | Start a second SoapySDR application | Rejected (TCP connection closed, USB interface busy) |
| Link timeout | `phx_client.py timeout` | Session closed after about 1 s, PA off |
| Late TX packets | `phx_client.py late` | `tx_late` and `tx_early` incremented, packets discarded |
| Watchdog | `phenix wdt_test` on the console | Reset after the watchdog timeout, `last_reset` = watchdog |
| Factory reset | Hold SW2 during reset | All LEDs blink fast, network defaults restored |
| Factory reset (menu) | Menu, *Factory reset*, confirm twice | Session closed, PA off, defaults restored after the reboot |
| Flash write at runtime | `phenix config set sample_port 1257`, then `... 1256`, then `phx_soapy_test rx` over Ethernet | `PASS` at the full rate (flash caches released after the write) |

## Front panel

| Test | Procedure | Expected |
|---|---|---|
| Colour zones | `phenix display test 30` | Lit bar exactly in the yellow zone, frame exactly in the blue zone, text not mirrored |
| Pages | `phenix display key next` six times, `phenix display dump` after each | All six pages, values match `stream status` / `phenix config show` |
| Host state | Run `phx_soapy_test rx` | Link label highlighted, `RX` shown, counters running, test `PASS` |
| Menu | Navigate with `phenix display key ...` or the encoder | See [display.md](../firmware/src/display/_doc/display.md#operation); leaving the Network menu discards changes |
| Dimming | Set *Dim after* to 1 min, wait | `phenix display` reports `(dimmed)`; the first input only wakes the display |
| No display | Disconnect the display, reboot | `Display not ready, front panel disabled`, streaming unaffected |

Do not script the Reboot, Factory reset or Save+reboot dialogs with *Yes*
unless the reset is intended: a factory reset erases the network
configuration.

## Console diagnostics

All console commands are listed in
[shell.md](../firmware/src/shell/_doc/shell.md). `phenix status` shows the
board state, `stream status` the sample path counters, `phenix display` the
front panel.
