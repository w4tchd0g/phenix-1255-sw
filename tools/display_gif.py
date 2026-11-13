#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Phenix-1255 contributors
"""Record the front panel over the console and render it as an animated GIF.

  capture  runs a fixed tour through the status pages and the menu with
           `phenix display key` and stores every `phenix display dump`
           (needs pyserial)
  render   turns the stored frames into a GIF in the look of the
           yellow/blue panel (needs Pillow)

  python3 tools/display_gif.py capture --port /dev/ttyACM0 --out frames.txt
  python3 tools/display_gif.py render frames.txt --out display.gif

The console is /dev/ttyACM* on Linux and /dev/cu.usbmodem* on macOS.

The stored frames contain the real MAC address, serial number and IP
addresses. `render` replaces them (MAC ff:ff:ff:ff:ff:ff, a dummy serial,
192.168.x.y -> 192.168.2.y) by reading the text with the firmware font and
drawing the replacement with the same font; `render --print` shows the
result as text for checking.
"""

import argparse
import pathlib
import re
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
FONT_SOURCE = ROOT / "firmware/src/display/display_fb.c"

WIDTH = 128
HEIGHT = 64
CHAR_WIDTH = 6
YELLOW_ROWS = 16

# `phenix display dump` prints two pixel rows per character
CELLS = {" ": (0, 0), "▀": (1, 0), "▄": (0, 1), "█": (1, 1)}

ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")
DUMP_LINE = re.compile(r"^\|(.{%d})\|$" % WIDTH)

# Same on Linux and macOS; see tools/_doc/display_gif.md
VENV_HINT = "python3 -m venv build/venv; build/venv/bin/pip install pyserial pillow"

DUMMY_MAC = "ff:ff:ff:ff:ff:ff"
DUMMY_SERIAL = ("0123456789abcdef", "fedcba9876543210")

# Tour steps: ("key", name) presses a key, ("dump", ms[, count]) stores
# count frames shown for ms each, ("ok_on", label) presses OK only while
# the menu entry with that label is selected. Only harmless entries are
# opened; confirmations (reboot, factory reset, host link) are never reached.
TOUR = [
    ("dump", 1200, 2),                    # STATUS 1/6
    ("key", "right"), ("dump", 2000),     # NETWORK
    ("key", "right"), ("dump", 2000),     # RF
    ("key", "right"), ("dump", 1000, 3),  # STREAM, live counters
    ("key", "right"), ("dump", 2000),     # SYSTEM
    ("key", "right"), ("dump", 1600),     # HARDWARE
    ("key", "ok"), ("dump", 1200),        # MENU, RF gain selected
    ("ok_on", "RF gain"), ("dump", 1200),
    ("key", "right"), ("dump", 600),
    ("key", "right"), ("dump", 600),
    ("key", "right"), ("dump", 1000),
    ("key", "back"), ("dump", 800),       # MENU
    ("key", "right"), ("dump", 800),      # Display
    ("ok_on", "Display"), ("dump", 1600),
    ("key", "back"), ("dump", 600),
    ("key", "right"), ("dump", 600),      # Network
    ("ok_on", "Network"), ("dump", 1800),
    ("key", "back"), ("dump", 600),
    ("key", "right"), ("dump", 600),      # Host link
    ("key", "right"), ("dump", 800),      # About
    ("ok_on", "About"), ("dump", 2500),
    ("key", "back"), ("dump", 600),       # any input returns to the menu
    ("key", "back"), ("dump", 1500),      # status pages
]

# Back from any screen: leaves editing, menus and confirmations without a
# change and ends on the first status page. The first input may only wake
# a dimmed display.
RESET_KEYS = 5

# The display redraws every 250 ms; wait for the frame after a key
KEY_SETTLE_S = 0.5


# Frames

def parse_dump(lines):
    """Pixel rows (lists of 0/1) from the lines of one dump, or None."""
    rows = []
    for line in lines:
        m = DUMP_LINE.match(ANSI.sub("", line).rstrip("\r"))
        if not m:
            continue
        upper, lower = [], []
        for ch in m.group(1):
            if ch not in CELLS:
                return None
            u, lo = CELLS[ch]
            upper.append(u)
            lower.append(lo)
        rows += [upper, lower]
    return rows if len(rows) == HEIGHT else None


def dump_lines(rows):
    """The frame in the format of `phenix display dump`."""
    return ["|%s|" % "".join(" \u2580\u2584\u2588"[rows[y][x] | (rows[y + 1][x] << 1)]
                             for x in range(WIDTH))
            for y in range(0, HEIGHT, 2)]


def write_frames(path, frames):
    with open(path, "w", encoding="utf-8") as f:
        for ms, rows in frames:
            f.write("# frame %d\n" % ms)
            f.write("\n".join(dump_lines(rows)) + "\n")


def read_frames(path):
    frames = []
    ms, block = None, []
    for line in open(path, encoding="utf-8").read().splitlines() + ["# frame 0"]:
        if line.startswith("# frame"):
            if ms is not None:
                rows = parse_dump(block)
                if rows is None:
                    sys.exit("%s: incomplete frame" % path)
                frames.append((ms, rows))
            ms, block = int(line.split()[2]), []
        else:
            block.append(line)
    return frames


# Capture

class Console:
    def __init__(self, port, baud, prompt):
        try:
            import serial
        except ImportError:
            sys.exit("pyserial is required: " + VENV_HINT)
        self.ser = serial.Serial(port, baud, timeout=0.05)
        self.prompt = prompt

    def run(self, cmd, timeout=5.0):
        """Run a shell command, return its output lines."""
        self.ser.reset_input_buffer()
        self.ser.write(cmd.encode() + b"\r")
        buf = b""
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            buf += self.ser.read(4096)
            text = ANSI.sub("", buf.decode("utf-8", "replace"))
            # The echo of the command line comes first, the prompt last
            pos = text.find(cmd)
            if pos >= 0 and self.prompt in text[pos + len(cmd):]:
                body = text[pos + len(cmd):]
                return body[:body.rfind(self.prompt)].splitlines()
        raise TimeoutError("no prompt after '%s'" % cmd)

    def dump(self):
        rows = parse_dump(self.run("phenix display dump"))
        if rows is None:
            raise RuntimeError("no display frame (display connected?)")
        return rows

    def key(self, name):
        self.run("phenix display key %s" % name)
        time.sleep(KEY_SETTLE_S)


def capture_splash(con):
    """Reboot and catch the start screen, shown for 3 s after boot."""
    con.ser.reset_input_buffer()
    con.ser.write(b"kernel reboot cold\r")
    end = time.monotonic() + 15.0
    while time.monotonic() < end:
        time.sleep(0.2)
        try:
            lines = con.run("phenix display dump", timeout=3.0)
        except TimeoutError:
            continue
        rows = parse_dump(lines)
        if rows is not None:
            return rows
    raise RuntimeError("no frame after the reboot")


def cmd_capture(args):
    con = Console(args.port, args.baud, args.prompt)
    frames = []

    if args.splash:
        frames.append((3000, capture_splash(con)))
        print("start screen captured, waiting %d s" % args.delay)
        time.sleep(args.delay)

    for _ in range(RESET_KEYS):
        con.key("back")

    for step in TOUR:
        if step[0] == "key":
            con.key(step[1])
        elif step[0] == "ok_on":
            selected = selected_label(con.dump())
            if selected != step[1]:
                for _ in range(RESET_KEYS):
                    con.key("back")
                sys.exit("expected menu entry '%s', found '%s'; tour aborted" %
                         (step[1], selected))
            con.key("ok")
        else:
            for _ in range(step[2] if len(step) > 2 else 1):
                t0 = time.monotonic()
                frames.append((step[1], con.dump()))
                # Live counters: keep the dumps the frame time apart
                if len(step) > 2:
                    time.sleep(max(0.0, step[1] / 1000.0 - (time.monotonic() - t0)))
        print("\r%d frames" % len(frames), end="", flush=True)

    print()
    write_frames(args.out, frames)
    print("wrote %s (contains the real serial number and addresses)" % args.out)


# Text recognition with the firmware font

def load_font():
    src = FONT_SOURCE.read_text(encoding="utf-8")
    font = {}
    for m in re.finditer(r"\{((?:0x[0-9A-Fa-f]{2},?\s*){5})\},\s*/\* '(.)' \*/", src):
        cols = tuple(int(v, 16) for v in m.group(1).replace(",", " ").split())
        font[m.group(2)] = cols + (0,)
    if len(font) != 0x7F - 0x20:
        sys.exit("%s: font table not found" % FONT_SOURCE)
    return font


FONT = None
GLYPHS = None


def font():
    global FONT, GLYPHS
    if FONT is None:
        FONT = load_font()
        GLYPHS = {}
        for ch, cols in FONT.items():
            GLYPHS.setdefault(cols, (ch, False))
            GLYPHS.setdefault(tuple(c ^ 0xFF for c in cols), (ch, True))
    return FONT, GLYPHS


def column(rows, row, x):
    return sum(rows[row * 8 + b][x] << b for b in range(8))


def read_cells(rows, row, phase):
    """Characters of one text row at x = phase + 6 k: list of (x, ch, invert)."""
    _, glyphs = font()
    cells = []
    for x in range(phase, WIDTH - CHAR_WIDTH + 1, CHAR_WIDTH):
        cols = tuple(column(rows, row, x + i) for i in range(CHAR_WIDTH))
        ch, inv = glyphs.get(cols, ("\x7f", False))
        cells.append((x, ch, inv))
    return cells


def draw_char(rows, row, x, ch, invert):
    fnt, _ = font()
    for i, col in enumerate(fnt.get(ch, fnt["?"])):
        if invert:
            col ^= 0xFF
        for b in range(8):
            rows[row * 8 + b][x + i] = (col >> b) & 1


def selected_label(rows):
    """Text of the highlighted menu entry, or None."""
    for row in range(2, 8):
        for phase in range(CHAR_WIDTH):
            cells = read_cells(rows, row, phase)
            text = "".join(ch for _, ch, inv in cells if inv).strip()
            if len(text) >= 2 and "\x7f" not in text:
                return text
    return None


def ipv4_doc(m):
    parts = [int(p) for p in m.group(0).split(".")]
    if parts[:2] == [192, 168] and parts[2] != 2:
        return "192.168.2.%d" % parts[3]
    return m.group(0)


# (pattern, replacement); a replacement must keep the length of the match
RULES = [
    (re.compile(r"[0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5}"),
     lambda m: DUMMY_MAC.upper() if m.group(0).isupper() else DUMMY_MAC),
    (re.compile(r"(?<=SN )[0-9a-f]{16}"), lambda m: DUMMY_SERIAL[0]),
    (re.compile(r"^   [0-9a-f]{16}(?![0-9a-f])"), lambda m: "   " + DUMMY_SERIAL[1]),
    (re.compile(r"(?<![\d.])\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}(?![\d.])"), ipv4_doc),
]


def anonymize(rows):
    rows = [list(r) for r in rows]
    for row in range(HEIGHT // 8):
        for phase in range(CHAR_WIDTH):
            cells = read_cells(rows, row, phase)
            text = "".join(ch for _, ch, _ in cells)
            for pattern, repl in RULES:
                for m in pattern.finditer(text):
                    new = repl(m)
                    if new == m.group(0):
                        continue
                    if len(new) != len(m.group(0)):
                        sys.exit("replacement '%s' -> '%s' changes the length" %
                                 (m.group(0), new))
                    for i, ch in enumerate(new):
                        x, _, inv = cells[m.start() + i]
                        draw_char(rows, row, x, ch, inv)
                    cells = read_cells(rows, row, phase)
                    text = "".join(ch for _, ch, _ in cells)
    return rows


def print_frame(ms, rows):
    print("# %d ms" % ms)
    print("\n".join(dump_lines(rows)))


# Rendering

def render_frame(rows, scale, Image, ImageDraw):
    border = scale * 4
    gap = scale  # dark gap between the colour zones, about one pixel row
    width = WIDTH * scale + 2 * border
    height = HEIGHT * scale + gap + 2 * border
    yellow = (255, 205, 40)
    blue = (60, 175, 255)

    img = Image.new("RGB", (width, height), (10, 12, 16))
    draw = ImageDraw.Draw(img)
    # The pixel grid is a dimmed line, not a black one: black lines leave
    # the dark glyphs of inverted text unreadable once the GIF is scaled down
    dot = max(1, scale - 1)
    for y in range(HEIGHT):
        colour = yellow if y < YELLOW_ROWS else blue
        grid = tuple(c * 3 // 4 for c in colour)
        py = border + y * scale + (gap if y >= YELLOW_ROWS else 0)
        for x in range(WIDTH):
            if rows[y][x]:
                px = border + x * scale
                draw.rectangle([px, py, px + scale - 1, py + scale - 1], fill=grid)
                draw.rectangle([px, py, px + dot - 1, py + dot - 1], fill=colour)
    return img


def cmd_render(args):
    frames = [(ms, anonymize(rows)) for ms, rows in read_frames(args.frames)]
    if args.print:
        for ms, rows in frames:
            print_frame(ms, rows)
        return

    try:
        from PIL import Image, ImageDraw
    except ImportError:
        sys.exit("Pillow is required: " + VENV_HINT)

    images = [render_frame(rows, args.scale, Image, ImageDraw)
              for _, rows in frames]
    palette = images[0].quantize(colors=64, method=Image.Quantize.MEDIANCUT)
    images = [im.quantize(palette=palette, dither=Image.Dither.NONE) for im in images]
    images[0].save(args.out, save_all=True, append_images=images[1:],
                   duration=[ms for ms, _ in frames], loop=0, optimize=True, disposal=1)
    print("wrote %s (%d frames)" % (args.out, len(images)))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)

    cap = sub.add_parser("capture", help="record the front panel over the console")
    cap.add_argument("--port", required=True,
                     help="MCU-Link console, /dev/ttyACM* (Linux), /dev/cu.usbmodem* (macOS)")
    cap.add_argument("--baud", type=int, default=115200)
    cap.add_argument("--prompt", default="uart:~$")
    cap.add_argument("--splash", action="store_true",
                     help="reboot first and record the start screen (ends a running stream)")
    cap.add_argument("--delay", type=int, default=5,
                     help="seconds between start screen and tour, e.g. to start a stream")
    cap.add_argument("--out", default="display_frames.txt")
    cap.set_defaults(func=cmd_capture)

    ren = sub.add_parser("render", help="render recorded frames as a GIF")
    ren.add_argument("frames")
    ren.add_argument("--out", default="display.gif")
    ren.add_argument("--scale", type=int, default=4, help="screen pixels per display pixel")
    ren.add_argument("--print", action="store_true",
                     help="print the anonymized frames as text instead")
    ren.set_defaults(func=cmd_render)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
