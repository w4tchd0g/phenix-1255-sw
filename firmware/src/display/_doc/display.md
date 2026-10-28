# Front panel display

`display_ui.c` (pages, menu, input) and `display_fb.c` (frame buffer, font)
run the front panel. A 0.96" 128x64 OLED (SSD1306, I2C) shows the device
state on five status pages and offers a settings menu. It is operated with a
rotary encoder with push button; both are part of phenix-1255-hw. SW3 on the
FRDM-MCXN947 also steps through the pages, which is useful on a µCell or
compatible board setup without encoder. The firmware runs unchanged without a
display.

## Hardware

| Signal | MCU pin | Function | Notes |
|---|---|---|---|
| OLED SDA | P1_0 | LPI2C3 SDA (FC3_P0) | mikroBUS I2C pins, 400 kHz |
| OLED SCL | P1_1 | LPI2C3 SCL (FC3_P1) | Pull-ups on the display module |
| Encoder A | P1_19 | GPIO input, internal pull-up | `gpio-qdec` |
| Encoder B | P1_18 | GPIO input, internal pull-up | `gpio-qdec` |
| Encoder push button | P0_11 | GPIO input, pull-up, active low | `gpio-keys`, `INPUT_KEY_ENTER` |
| SW3 | P0_6 | GPIO input, pull-up, active low | `gpio-keys`, `INPUT_KEY_DOWN`; ISP mode select only during reset |

**I2C address:** display modules print the address in 8-bit form, `0x78`
(`0x79` read) or `0x7A` (`0x7B` read), selected by a resistor on the back.
The 7-bit address used in the devicetree is `0x3C` or `0x3D`. The module in
use answers at `0x3C`; change `reg` and the node name in
[`frdm_mcxn947_mcxn947_cpu0.overlay`](../../../boards/frdm_mcxn947_mcxn947_cpu0.overlay)
for a module set to `0x3D`.

**Colour zones:** the two-colour panels have fixed colours in the glass:
pixel rows 0..15 (text rows 0 and 1) are yellow, rows 16..63 (text rows 2..7)
are blue, with a narrow visible gap between them. The firmware puts the
header into the yellow zone. `phenix display test` shows a pattern that
marks both zones.

**Encoder:** one input event per detent of an encoder with four steps per
detent (`steps-per-period = <4>`). If turning clockwise moves backwards, add
`invert-direction;` to the `encoder` node.

## Screen layout

```
 +--------------------------+
 | PHENIX-1255        [ETH] |  yellow: host link, highlighted while a host session is open
 | STATUS 1/5 [ERR][RX][TX] |  yellow: page or menu title, active stream, error
 +--------------------------+
 | RX 438.250000 MHz        |
 | TX 430.650000 MHz        |  blue: six lines of content
 | ...                      |
 +--------------------------+
```

`ERR` is shown while the red error LED is on or blinking (stream fault,
watchdog reset, initialization error) or the SX1255 is not ready. `RX` is
shown while the sample stream runs, `TX` while TX is enabled.

### Status pages

| Page | Content |
|---|---|
| STATUS | RX and TX frequency (exact synthesizer value), sample rate, PLL lock, host session, late TX packets and dropped RX samples |
| NETWORK | Current IP address and its source, MAC address, control and sample port, host link selected by `COMM_SEL` |
| RF | LNA, PGA, DAC and mixer gain, RX input, TX driver, PA, SX1255 state |
| STREAM | Sample counter, RX/TX packet counters, late/early TX packets, SAI FIFO and frame sync errors |
| SYSTEM | Firmware and Zephyr version, uptime, last reset cause, serial number |

The pages change every 5 s ("Auto pages"). A manual page change pauses the
automatic change for 60 s.

## Operation

| Input | Status pages | Menu | While editing a value |
|---|---|---|---|
| Turn clockwise, SW3 | Next page | Next entry | Increase |
| Turn counter-clockwise | Previous page | Previous entry | Decrease |
| Short push | Open the menu | Select or edit the entry | Confirm (next octet for addresses) |
| Long push (>= 0.8 s) | First page | Back | Stop editing |

The menu closes after 60 s without input.

### Menu

```
MENU
 ├─ Display
 │   ├─ Brightness   1..10 (default 8)
 │   ├─ Dim after    never, 1, 2, 5 (default), 10, 30, 60 min
 │   └─ Auto pages   on (default) / off
 ├─ Network
 │   ├─ Mode         DHCP / static
 │   ├─ IP, Mask, GW, DNS   static configuration (prefix length for the mask)
 │   └─ Save+reboot  validates, asks for confirmation, saves and reboots
 ├─ Reboot           asks for confirmation
 └─ Factory reset    asks twice, erases all settings and reboots
```

- Display settings take effect at once and are saved when leaving the
  Display menu (or when the menu times out). While a stream runs, saving is
  deferred until it stops, because flash writes lock interrupts and would
  interrupt the sample path.
- Network changes are kept in the menu until *Save+reboot*; leaving the
  menu discards them. The same validation as on the web interface and the
  console applies; a rejected configuration shows the reason. Turning the
  encoder quickly steps address octets by 10.
- Reboot and factory reset switch the PA off and close the host session
  first, like a reboot from the web interface. The confirmation shows a
  warning while a host session is open.

### Dimming

OLEDs age with the time a pixel is lit, and static content burns in. After
the configured time without operator input the display is dimmed to the
lowest contrast. It returns to full brightness on any input (the first input
only wakes the display), when a host session opens or closes, when TX is
switched and when an error is signalled.

## Implementation

- `display_ui.c` runs in its own thread at preemptive priority 9, below the
  web interface and above the watchdog feeder. It redraws every 250 ms and
  sends a frame only when its content changed; a full frame takes about
  23 ms on the bus.
- `display_fb.c` holds the frame buffer in the SSD1306 page layout and a
  5x7 font. Frame buffers and the thread stack are in SRAMX (CPU only, the
  LPI2C driver does not use DMA).
- The LPI2C3 and GPIO port 0/1 interrupts run at priority 3, below the eDMA
  (0) and SAI (2) interrupts, so the front panel cannot delay the sample
  path.
- The LPI2C controller needs a bus idle timeout (`bus-idle-timeout`, 10 us):
  without it the bus busy flag stays set after boot and the NXP HAL rejects
  every transfer, so the display stays dark and an I2C scan finds nothing.
- Input callbacks run synchronously in the context of the reporting driver
  (`CONFIG_INPUT_MODE_SYNCHRONOUS`) and only queue the event.
- Settings are stored as `phenix/display` in the settings partition, see
  [config.md](../../config/_doc/config.md).

## Console commands

| Command | Purpose |
|---|---|
| `phenix display` | Display state and settings |
| `phenix display key <prev\|next\|select\|back>` | Simulate an input |
| `phenix display test [s]` | Colour zone test pattern (default 10 s) |
| `phenix display dump` | Print the content last sent to the display |
