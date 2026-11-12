# Logo converter

[`tools/logo_to_c.py`](../logo_to_c.py) converts the project logo into the
bitmaps of the front panel start screen
([display.md](../../firmware/src/display/_doc/display.md#start-screen)) and
writes them to `firmware/src/display/splash_logo.c`. The generated file is
checked in, so the firmware build does not need the script; run it only
after the logo changed.

It needs Python 3 and Pillow:

```sh
sudo apt install python3-pil       # Debian/Ubuntu
brew install pillow                # macOS (Homebrew Python)

python3 tools/logo_to_c.py --preview   # print the bitmaps as text
python3 tools/logo_to_c.py             # write splash_logo.c
```

Options: `--logo` (default `_doc/images/phenix-1255_logo_transparent.png`),
`--out` (default `firmware/src/display/splash_logo.c`), `--preview`.

The logo must have a transparent background. The lowest band of opaque rows
is taken as the lettering and scaled to 14 pixels height for the yellow
zone; everything above it is the graphic, scaled to 48 pixels height for the
blue zone. Every opaque pixel is lit regardless of its colour, so the gaps
between the logo parts keep them apart on the monochrome display. The alpha
thresholds at the top of the script were chosen by eye; check the result
with `--preview` and `phenix display dump` after a change.
