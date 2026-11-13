# Front panel GIF

[`tools/display_gif.py`](../display_gif.py) records the front panel
([display.md](../../firmware/src/display/_doc/display.md)) over the console
and renders it as an animated GIF in the look of the yellow/blue panel. It
produces `_doc/images/front_panel.gif` shown in the README.

The frames are the exact content sent to the display, read with
`phenix display dump`; the buttons are simulated with
`phenix display key`. No camera is involved.

It needs Python 3, pyserial (`capture`) and Pillow (`render`). A virtual
environment in `build/` provides both in the same way on Linux and macOS
(Debian/Ubuntu need `sudo apt install python3-venv` once):

```sh
python3 -m venv build/venv
build/venv/bin/pip install pyserial pillow

# Console: /dev/ttyACM0 on Linux, /dev/cu.usbmodem* on macOS
build/venv/bin/python3 tools/display_gif.py capture --port /dev/ttyACM0 --splash --out frames.txt
build/venv/bin/python3 tools/display_gif.py render frames.txt --print   # check the text
build/venv/bin/python3 tools/display_gif.py render frames.txt --out _doc/images/front_panel.gif
```

## capture

Runs a fixed tour and stores every frame with its display time:

1. With `--splash`: `kernel reboot cold`, then the start screen, then a
   pause of `--delay` seconds (default 5). The reboot ends a running stream,
   so a stream for live values is started during the pause, for example
   `phx_soapy_test rx "driver=phenix-1255" 120` (receive only).
2. Long presses of OK return to the first status page from any screen.
3. The six status pages, the STREAM page three times one second apart
   (live counters).
4. The menu: RF gain, Display and Network submenus, About.

The tour only opens harmless entries and never reaches a confirmation
(reboot, factory reset, host link). Before every OK in the menu it checks
the highlighted entry and aborts with long presses of OK if it is not the
expected one. Nothing is changed or saved on the device.

Options: `--port` (required), `--baud` (115200), `--prompt` (`uart:~$`),
`--splash`, `--delay`, `--out` (default `display_frames.txt`).

The frames file holds the dumps as text and contains the real MAC address,
serial number and IP addresses; keep it out of the repository.

## render

Replaces device data before drawing, so the GIF can be published:

| Data | Replacement |
|---|---|
| MAC address | `ff:ff:ff:ff:ff:ff` |
| Serial number (SYSTEM page) | `0123456789abcdef` / `fedcba9876543210` |
| IPv4 `192.168.x.y` | `192.168.2.y` (documentation network) |

The text is read with the 5x7 font from
`firmware/src/display/display_fb.c` at every column offset, so
right-aligned and inverted text is found as well; the replacement is drawn
with the same font and keeps the inversion. `--print` shows the result as
text.

Every display pixel becomes a `--scale` (default 4) pixel square; rows
0..15 are yellow and rows 16..63 blue, with a one-pixel gap between the
zones, as on the panel. Frames with the same content in a row are merged.

Options: `--out` (default `display.gif`), `--scale`, `--print`.
