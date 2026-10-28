# CLAUDE.md

Guidance for AI coding assistants working in this repository. Keep it short;
details live in the `_doc/` folders and are linked below.

## Project

Phenix-1255: SX1255 SDR transceiver (38.4 MHz TCXO) on the NXP FRDM-MCXN947.
Target hardware is the phenix-1255-hw board (schematic, Gerber, BOM:
[Phenix-1255-HW](https://github.com/labdvm/Phenix-1255-HW)). The first tests
ran on a µCell board; µCell and compatible SX1255 boards work with a reduced
feature set, see [hardware.md](_doc/hardware.md#µcell-and-compatible-boards).
Zephyr firmware streams timestamped I/Q samples over HS USB or Ethernet
(selected by the `COMM_SEL` switch at boot); a SoapySDR module
(`driver=phenix-1255`) serves TETRA base station software
(TETRA-Bluestation, FlowStation). Supported hosts: **Linux and macOS**
(Windows is not supported). Status and features: [README.md](README.md).

| Path | Content |
|---|---|
| `firmware/` | Zephyr application, target `frdm_mcxn947/mcxn947/cpu0` (C); one folder per module in `firmware/src/` |
| `common/include/phenix1255/protocol.h` | Wire protocol shared by firmware and host |
| `soapy/` | SoapySDR module, test programs in `soapy/tests/` (C++17, libusb optional) |
| `tools/phx_client.py` | Protocol test client, Ethernet only (Python stdlib only) |
| `patches/` | Patches for TETRA-Bluestation and FlowStation (`git apply` format) |
| `_doc/` | System documents: architecture, hardware, building, testing, roadmap |

Documentation lives in a `_doc/` folder next to the code it describes: the
root `_doc/` for the system, `firmware/_doc/` for the firmware structure,
`firmware/src/<module>/_doc/` per firmware module, and `common/`, `soapy/`,
`soapy/tests/`, `tools/`, `patches/` each with their own `_doc/`. Read
[architecture.md](_doc/architecture.md) and
[firmware.md](firmware/_doc/firmware.md) before larger changes.

## Build

Firmware (west workspace or existing one with `ZEPHYR_BASE` set, see
[building.md](_doc/building.md)):

```sh
west build -b frdm_mcxn947/mcxn947/cpu0 -d build/firmware firmware   # add -p always for pristine
west flash -d build/firmware                                         # LinkServer in PATH
```

SoapySDR module:

```sh
cmake -S soapy -B build/soapy                                          # Linux
cmake -S soapy -B build/soapy -DCMAKE_PREFIX_PATH="$(brew --prefix)"   # macOS
cmake --build build/soapy -j
export SOAPY_SDR_PLUGIN_PATH="$PWD/build/soapy"   # use without installing
```

On Linux, USB access for normal users needs the udev rule
`soapy/udev/99-phenix-1255.rules` ([soapy.md](soapy/_doc/soapy.md#linux)); without
it the device is listed as "no access".

Builds must stay free of warnings on Linux and macOS. All build output goes to
`build/` (ignored by git).

## Test

All tests need real hardware; there is no unit test suite. Procedure and
expected results: [testing.md](_doc/testing.md).

```sh
SoapySDRUtil --probe="driver=phenix-1255"
build/soapy/phx_soapy_test rx   "driver=phenix-1255" 10
build/soapy/phx_soapy_test tone "driver=phenix-1255" 5 600000 60000   # transmits
build/soapy/phx_loopback_test   "driver=phenix-1255" all 20 5          # transmits
```

- Console: MCU-Link VCOM, 115200 8N1, `/dev/ttyACM*` on Linux,
  `/dev/cu.usbmodem*` on macOS. Diagnostics: `phenix status`,
  `stream status`, `sx1255 dump` (see [shell.md](firmware/src/shell/_doc/shell.md#stream)),
  `phenix display dump` (front panel content as text, [display.md](firmware/src/display/_doc/display.md)).
- Always cap redirected test output (`| tail`, `| head -c`): a test spinning
  on stream errors once produced a multi-GB log.
- Loopback through a ~20 dB attenuator needs low gains (LNA 0, PGA 0, DAC 0,
  MIXER 30); the module defaults clip the RX path and fake random delays.
  Check the RX envelope (`PHX_VERBOSE=1`) before trusting correlations.

## RF and PA safety

- Tests that transmit require an attenuator or dummy load, or antennas on a
  frequency the operator is licensed for. Never start a transmitting test
  without confirming the RF setup with the user.
- PA_EN is driven off as early as possible at boot and switched on only by
  `STREAM_TX`. Every new error or link-loss path must switch it off
  immediately. Do not weaken this; see
  [firmware.md](firmware/_doc/firmware.md#pa-safety).

## Conventions

- **Public repository:** no personal data, user names, home paths, private IP
  addresses, device serial numbers or local tool paths in code, docs or
  patches. Example addresses use `192.168.2.x` and `phenix-1255.local`.
  Exception: the credit for the hardware developer (Z32IT) in README and
  `_doc/hardware.md`.
- **Linux and macOS are equal targets.** Every host-side change (`soapy/`,
  `tools/`, build files) must build and work on both; guard platform-specific
  APIs with feature macros (e.g. `SO_NOSIGPIPE` / `MSG_NOSIGNAL` in
  `TransportNet.cpp`) instead of OS checks where possible. Docs give commands
  for both (Debian/Ubuntu `apt` and Homebrew), never Homebrew paths or
  `/dev/cu.*` alone.
- English only; comments are strictly technical and explain *why*, not *what*.
- Every source file starts with an SPDX header:
  `SPDX-License-Identifier: MIT` and `Copyright (c) 2026 Phenix-1255 contributors`.
- Style is defined by `.clang-format` / `.editorconfig`: firmware and `common/`
  use Zephyr style (tabs, 100 columns, Linux braces); `soapy/` has its own
  `soapy/.clang-format` (4 spaces, 100 columns). Firmware symbols are
  `snake_case` C, Soapy code lives in `namespace phx`.
- Docs are part of every change: update the `_doc/` folder of the module
  that changes (and README status/features when behaviour visible to users
  changes). A new firmware module gets its own folder in `firmware/src/`
  with a `_doc/` and a row in [firmware.md](firmware/_doc/firmware.md).
- Firmware headers are included with their module folder
  (`#include "stream/stream.h"`).
- Docs describe the product, not the development history: no work stages,
  dates or plans outside [roadmap.md](_doc/roadmap.md).

### Protocol changes

`protocol.h` is the single source of truth. A change must be applied together
in the firmware, `soapy/src/`, `tools/phx_client.py` (which mirrors the
constants) and [protocol.md](common/_doc/protocol.md). Incompatible changes bump
`PHX_PROTO_VERSION` in both `protocol.h` and `phx_client.py`.

### USB identification

The firmware uses the pid.codes **test PID 1209:000A**. It is for private
testing only, not unique, and must not be used in distributed devices. Any
code or docs touching the USB IDs must keep this explanation
([usb.md](firmware/src/usb/_doc/usb.md#about-the-test-pid-1209000a)).

## Hardware-verified constraints

These were found on hardware and look like they could be "simplified". They
must not be changed without a hardware test.

- **Zephyr is pinned to v4.5.0-rc3** in `west.yml`; older ENET-QOS drivers
  drop TX frames while the transmitter is busy. Move to v4.5.0 final when
  released, nothing older.
- **`CONFIG_SPI_NXP_LPSPI_DMA=n`:** LPSPI DMA fails on multi-buffer transfers.
- **eDMA position:** `EDMA_GetRemainingMajorLoopCount()` returns 0 while DONE
  is set in circular mode; the sample counter reads CITER directly
  (`stream/stream.c`).
- **SAI:** standard I2S (BCP=1, FSE=1); TX is synchronous to RX and TE is set
  before RE so both rings start on the same frame. The TX-to-RX loop delay is
  deterministic per rate ([stream.md](firmware/src/stream/_doc/stream.md#tx-to-rx-loop-delay));
  any change in this path must re-run `phx_loopback_test`.
- **SX1255 RF loopback** needs the TX driver on; the digital loopback yields
  nothing. STAT 0x11: bit 0 TX PLL lock, bit 1 RX PLL lock, bit 2 oscillator.
- **Ethernet:** `NET_BUF_DATA_SIZE=1536` with 32 RX descriptors (one net_buf
  per descriptor, else UDP bursts are lost); `NET_MAX_CONN` above the default.
  `zsock_shutdown()` does not wake a blocked `recv`, so control threads poll
  with a short timeout and a close flag.
- **USB (`udc_mcux_ehci`):** no `VBUS_READY` event when the cable is already
  plugged in at boot, so `usbd_enable()` is called unconditionally. Never call
  `usbd_ep_dequeue()` on an enabled endpoint with primed IN transfers (stale
  dTD, endpoint dead); stale IN data after a session change is tolerated by
  the host instead.
- **RAM:** SRAM A..H (416 KB) is ~93 % used. DMA buffers must stay there;
  CPU-only buffers and stacks go to SRAMX via `zephyr_code_relocate()` in
  `firmware/CMakeLists.txt`. Files listed there must never contain DMA
  buffers, and their names must not exist in Zephyr (the relocation script
  matched `shell/shell_cmds.c` to Zephyr's `subsys/shell/shell_cmds.c` and
  left 17 KB in SRAM A..H). Check the memory report after every firmware
  change.
- **Settings:** NVS in its own 64 KB internal-flash partition at 0x1F0000;
  there are no MCUboot partitions.
- **Network RX thread preemptive below the RX pump** (`prj.conf`,
  `CONFIG_NET_TC_THREAD_PREEMPTIVE`, base priority 5): the default cooperative
  `rx_q` thread processes a burst of incoming TX packets in one go and starves
  the pump past the 5.6 ms RX ring (RX drops in full duplex over Ethernet).
- **Flash caches:** the Zephyr MCXN flash driver leaves `NVM_CTRL[CLR_FLASH_CACHE]`
  and `LPCAC_CTRL[CLR_LPCAC]` set after every program/erase, which keeps both
  caches off until reset (Ethernet streaming then loses ~60 % of RX samples).
  `config/config_store.c` clears them after every flash operation; any new flash
  write must do the same. Flash operations lock interrupts, so runtime saves
  (display settings) are deferred while a stream runs.
- **LPI2C3 (OLED):** needs `bus-idle-timeout` in the devicetree, otherwise the
  bus busy flag stays set after boot and every transfer fails (scan finds
  nothing). Module labels give 8-bit addresses (0x78/0x7A); the panel answers
  at 7-bit 0x3C. LPI2C3 and GPIO0/1 interrupts run at priority 3, below eDMA
  and SAI.
