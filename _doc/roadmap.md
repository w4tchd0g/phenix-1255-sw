# Roadmap

What is planned next for Phenix-1255 and which points are still open.
Completed items are removed; their results are documented with the module
they belong to.

## Next steps

- **phenix-1255-hw bring-up** – run the checks in
  [testing.md](testing.md#phenix-1255-hw-bring-up) on the first boards: PA
  enable, status LEDs, DIO0/1/3, encoder, signal integrity, loop delay and
  current draw.
- **USB power** – set `bMaxPower` from the measured current draw
  ([usb.md](../firmware/src/usb/_doc/usb.md#usb-identification)).
- **Zephyr v4.5.0** – move `west.yml` from v4.5.0-rc3 to the final
  release once it is out, then run the full test procedure in
  [testing.md](testing.md).
- **Front panel** – test operation without a display and in HS USB mode
  with a display.
- **DHCP** – test the DHCP client against a real DHCP server; so far only
  static addresses were used.
- **FlowStation** – rebase the patch in `patches/flowstation/` onto the
  current FlowStation version (base is `0f4faa9`).

## Before boards are given to others

- **USB product ID** – apply for a dedicated PID. The test PID 1209:000A
  must not be used in distributed devices
  ([usb.md](../firmware/src/usb/_doc/usb.md#about-the-test-pid-1209000a)).

## Improvements

- **Front panel on the second core** – move the display and the rotary
  encoder to the second Cortex-M33 core (cpu1), which is unused so far. The
  front panel then no longer shares CPU time and memory with the sample path
  and the network stack on cpu0; status and settings are exchanged between
  the cores.
- **More free RAM** – SRAM A..H is about 94 % used
  ([firmware.md](../firmware/_doc/firmware.md#memory)). Review buffer sizes
  and thread stacks (`kernel thread stacks`) and move further CPU-only data
  to SRAMX, so that more RAM is available for new features. The
  hardware-verified network buffer settings stay unchanged.

## Ideas

- **Zephyr shield** – describe phenix-1255-hw as a Zephyr shield
  (`--shield phenix_1255_hw`) instead of the board overlay, so that a µCell
  or compatible setup can use its own overlay without the board-specific
  pins.

## Under observation

Seen once, not reproduced since. Report it if it shows up again, together
with the output of `stream status`.

- **SAI frame sync errors** – a FlowStation over-the-air run reported
  2 RX and 1 TX frame sync errors at an unknown time.
- **RX drop at stream start over Ethernet** – a FlowStation run lost
  3356 RX samples at startup, most likely because the initial burst of TX
  packets delayed the RX pump. The preemptive network RX thread
  ([firmware.md](../firmware/_doc/firmware.md#threads-and-interrupts)) may
  have fixed it.
