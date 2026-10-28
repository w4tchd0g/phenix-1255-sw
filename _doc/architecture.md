# Architecture

## Overview

The SX1255 is the I2S controller. All sample timing derives from its bit clock:
the firmware counts I2S frames and timestamps every sample with a 64-bit
counter that is shared by RX and TX. The host link (HS USB or Ethernet,
selected by `COMM_SEL` at boot) carries a control channel and a sample
channel with the same message formats on both transports.

## Software modules

Every module documents itself in its own `_doc` folder.

| Module | Path | Documentation |
|---|---|---|
| Firmware | `firmware/` | [firmware.md](../firmware/_doc/firmware.md): structure, threads, memory, PA safety, with links to the firmware modules |
| Wire protocol | `common/include/phenix1255/protocol.h` | [protocol.md](../common/_doc/protocol.md) |
| SoapySDR module | `soapy/` | [soapy.md](../soapy/_doc/soapy.md) |
| Test programs | `soapy/tests/` | [tests.md](../soapy/tests/_doc/tests.md) |
| Protocol test client | `tools/` | [phx_client.md](../tools/_doc/phx_client.md) |
| Logo converter for the start screen | `tools/` | [logo_to_c.md](../tools/_doc/logo_to_c.md) |
| Station patches | `patches/` | [patches.md](../patches/_doc/patches.md) |

System-wide documents in this folder:

| Document | Content |
|---|---|
| [hardware.md](hardware.md) | phenix-1255-hw board, pin assignment, µCell and compatible boards |
| [building.md](building.md) | Building and flashing the firmware, building the SoapySDR module |
| [testing.md](testing.md) | Test procedure and expected results |
| [roadmap.md](roadmap.md) | Open points |

## Data flow

**RX:** the SX1255 sends I/Q samples over I2S; the eDMA writes them into a
ring buffer and the sample counter is derived from the DMA position
([stream.md](../firmware/src/stream/_doc/stream.md)). The RX pump packs the
samples into packets with the counter value of their first sample and hands
them to the active transport ([ethernet.md](../firmware/src/ethernet/_doc/ethernet.md),
[usb.md](../firmware/src/usb/_doc/usb.md)). The SoapySDR module returns every
packet with its timestamp from `readStream`.

**TX:** the SoapySDR module splits timed `writeStream` calls into packets
with exact counter values and sends them shortly before their time. The
firmware copies each packet into a TX buffer indexed by the counter; the TX
DMA sends it in exactly that I2S frame.

**Control:** the SoapySDR module opens a session, configures the SX1255
(frequencies, rate, gains) and starts the streams over the control channel
([control.md](../firmware/src/control/_doc/control.md),
[protocol.md](../common/_doc/protocol.md)). Losing the session switches TX
and the PA off at once.
