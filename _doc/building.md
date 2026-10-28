# Building

This document describes how to build and flash the Phenix-1255 firmware and
how to build the SoapySDR module, on Linux and macOS.

## Firmware

### Requirements

| Component | Version |
|---|---|
| Zephyr | Pinned in [`west.yml`](../west.yml) |
| Zephyr SDK | 1.0.1 or newer |
| Board | NXP FRDM-MCXN947, target `frdm_mcxn947/mcxn947/cpu0` |
| Flash tool | NXP LinkServer (default) or SEGGER J-Link |

### Host packages

Debian/Ubuntu:

```sh
sudo apt install --no-install-recommends git cmake ninja-build gperf ccache \
    dfu-util device-tree-compiler wget python3-dev python3-venv python3-tk \
    xz-utils file make gcc libsdl2-dev libmagic1
```

macOS (Homebrew):

```sh
brew install cmake ninja gperf python3 ccache dtc wget libmagic
```

Install the Zephyr SDK as described in the
[Zephyr Getting Started Guide](https://docs.zephyrproject.org/latest/develop/getting_started/index.html).

### Option A: new west workspace

The repository is a west manifest repository. The workspace is the directory
that contains the clone; Zephyr and the required modules are fetched next to it.

```sh
mkdir phenix-1255-ws && cd phenix-1255-ws
git clone <repository-url> phenix-1255-sw
python3 -m venv .venv
source .venv/bin/activate
pip install west
west init -l phenix-1255-sw
west update
west zephyr-export
west packages pip --install
```

Resulting layout:

```
phenix-1255-ws/
├── .west/
├── phenix-1255-sw/      (this repository)
├── zephyr/
└── modules/hal/{cmsis,cmsis_6,nxp}
```

### Option B: existing Zephyr workspace

An existing workspace can be used if it contains the Zephyr revision pinned in
`west.yml` (or a compatible one) and the modules `cmsis`, `cmsis_6` and
`hal_nxp`. Activate its Python environment and point `ZEPHYR_BASE` to it:

```sh
source <workspace>/.venv/bin/activate
export ZEPHYR_BASE=<workspace>/zephyr
```

### Build

From the repository root:

```sh
west build -b frdm_mcxn947/mcxn947/cpu0 -d build/firmware firmware
```

Add `-p always` for a pristine build.

### Flash

The FRDM-MCXN947 has an on-board MCU-Link debug probe (USB connector J17,
labelled "MCU-Link"). With the default CMSIS-DAP probe firmware, use
NXP LinkServer:

```sh
west flash -d build/firmware
```

If the MCU-Link has been reprogrammed with SEGGER J-Link firmware:

```sh
west flash -d build/firmware --runner jlink
```

LinkServer must be in `PATH`. On macOS it is installed under
`/Applications/LinkServer_<version>/`.

### Console

The firmware log is printed on the MCU-Link virtual COM port
(115200 baud, 8N1).

| Host | Device name |
|---|---|
| Linux | `/dev/ttyACM*` |
| macOS | `/dev/cu.usbmodem*` |

Example:

```sh
python3 -m serial.tools.miniterm /dev/ttyACM0 115200
```

## SoapySDR module

The module `phenix1255Support` registers the SoapySDR driver `phenix-1255`.

### Requirements

Debian/Ubuntu:

```sh
sudo apt install --no-install-recommends libsoapysdr-dev soapysdr-tools cmake g++ \
  pkg-config libusb-1.0-0-dev
```

macOS (Homebrew):

```sh
brew install soapysdr cmake pkg-config libusb
```

libusb-1.0 (1.0.21 or newer) is needed for the HS USB transport. Without it
the module is built for Ethernet only (CMake prints a warning); the option
`-DPHX_ENABLE_USB=OFF` disables USB explicitly. On Linux, install the udev
rule for USB access as described in [soapy.md](../soapy/_doc/soapy.md#linux).

### Build and install

```sh
cmake -S soapy -B build/soapy                                          # Linux
cmake -S soapy -B build/soapy -DCMAKE_PREFIX_PATH="$(brew --prefix)"   # macOS
cmake --build build/soapy -j
sudo cmake --install build/soapy     # Linux; on macOS without sudo
```

To use the module without installing it, point SoapySDR to the build
directory:

```sh
export SOAPY_SDR_PLUGIN_PATH="$PWD/build/soapy"
SoapySDRUtil --find="driver=phenix-1255"
SoapySDRUtil --probe="driver=phenix-1255"
```

### Usage

Device arguments, discovery and USB access rights:
[soapy.md](../soapy/_doc/soapy.md). Test programs:
[tests.md](../soapy/tests/_doc/tests.md).

## Continuous integration

The GitHub Actions workflow
[`.github/workflows/build.yml`](../.github/workflows/build.yml) runs on every
push to `develop` and on pull requests from `develop` to `main`:

| Job | Runner | Checks |
|---|---|---|
| Firmware | Ubuntu | Pristine build for `frdm_mcxn947/mcxn947/cpu0` with `CONFIG_COMPILER_WARNINGS_AS_ERRORS=y`; memory report in the job summary |
| SoapySDR module | Ubuntu | Build with `CMAKE_COMPILE_WARNING_AS_ERROR=ON` and USB support, `SoapySDRUtil --info` must list the `phenix-1255` factory; syntax check of `tools/phx_client.py` |

Compiler warnings fail the build. The firmware (`zephyr.bin`, `zephyr.elf`,
`zephyr.map`) is kept as a workflow artifact for 30 days and is not
published as a release. The hardware tests in [testing.md](testing.md) are
not part of the workflow. The workflow builds on Linux only; macOS builds
must be checked locally before pushing (see the commands above).

