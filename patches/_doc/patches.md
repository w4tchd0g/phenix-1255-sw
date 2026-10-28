# Patches for TETRA base station software

These patches add the Phenix-1255 to the SDR auto-detection of
[TETRA-Bluestation](https://github.com/MidnightBlueLabs/tetra-bluestation) and
[FlowStation](https://github.com/razvanzeces/flowstation). Both projects open
SoapySDR devices, recognize supported devices by their SoapySDR driver key and
apply device specific defaults (sample rate, antennas, gains).

| Directory | Upstream | Base commit |
|---|---|---|
| `tetra-bluestation/` | MidnightBlueLabs/tetra-bluestation | `09d4e0d` (version 0.5.10) |
| `flowstation/` | razvanzeces/flowstation | `0f4faa9` |

## Changes

- `crates/tetra-entities/src/phy/components/soapy_settings.rs`: new device
  type `Phenix1255`, detected by the driver key `phenix-1255`, with these
  defaults:

  | Setting | Value |
  |---|---|
  | Sample rate | 600 kHz (`fs` in the configuration overrides it) |
  | RX antenna | `RX` |
  | TX antenna | `TX` |
  | RX gains | LNA 42 dB, PGA 16 dB |
  | TX gains | DAC 9 dB, MIXER 30 dB |
  | Hardware time | `getHardwareTime()` (estimated on the host, no device round trip) |

  The values follow the µCell, which uses the same SX1255 transceiver. No
  stream arguments are needed; the module ignores `period`.
- `example_config/config.toml`: example `device` entry for Ethernet.

## Test status

| Project | Tested |
|---|---|
| FlowStation `0f4faa9` | Built and run with the Phenix-1255 over Ethernet, over the air in the 70 cm amateur band: device detected, defaults applied, uplink bursts of a radio decoded, 2 x 60 s without RX/TX errors after startup |
| TETRA-Bluestation `09d4e0d` | Patch applies; `soapy_settings.rs` is identical apart from FlowStation's dashboard additions, not built separately |

At startup both stations log `Lost -1200 samples` and `Too late to produce
TX block 0` once; this comes from the station's start-up sequence (the first
RX block is expected one block ahead) and is followed by normal operation.

## Apply

```sh
cd tetra-bluestation        # or flowstation
git checkout 09d4e0d        # base commit, see above (0f4faa9 for flowstation)
git apply /path/to/phenix-1255-sw/patches/tetra-bluestation/0001-add-phenix-1255.patch
cargo build --release
```

The patches are small and usually also apply to newer upstream revisions;
`git apply --check` tells whether they do.

The Phenix-1255 SoapySDR module must be installed (see
[building.md](../../_doc/building.md#soapysdr-module)) or found through
`SOAPY_SDR_PLUGIN_PATH`.

## Configuration

**HS USB:** no `device` entry is needed. Without it, the station enumerates all
SoapySDR devices and opens the first supported one; the Phenix-1255 module
lists attached USB devices in every enumeration.

**Ethernet:** the module does not search the network during a generic
enumeration (mDNS timeouts would delay every SoapySDR application), so the
device must be given explicitly in `[phy_io.soapysdr]`:

```toml
device = "driver=phenix-1255,addr=192.168.2.10"
# or by mDNS name:
# device = "driver=phenix-1255,addr=phenix-1255.local"
```

Gains are overridden as for other devices, e.g. `rx_gain_lna = 30.0` or
`tx_gain_mixer = 24.0`. The available sample rates are 600, 300, 150, 75, 50
and 25 kHz; the default of 600 kHz is recommended.

RX and TX frequencies are set independently (separate SX1255 synthesizers),
so the usual 5 MHz duplex spacing in the 70 cm band works. The TX output must
be connected to a matching PA and antenna or a dummy load; the Phenix-1255
switches its PA enable output only while the station transmits.
