# Host protocol

The Phenix-1255 firmware and the SoapySDR module communicate over two logical
channels. The message formats are identical on both transports and defined in
[`common/include/phenix1255/protocol.h`](../include/phenix1255/protocol.h).

| Transport | Control channel | Sample channel |
|---|---|---|
| Ethernet | TCP, device port `ctrl_port` (default 1255) | UDP, device port `sample_port` (default 1256) |
| HS USB | Bulk EP 0x01 OUT / 0x81 IN | Bulk EP 0x02 OUT / 0x82 IN |

All multi-byte fields are little endian, all structures are packed.
Protocol version: 1.

## Session

Only one host session exists at a time.

1. The host opens the control channel (TCP connect, or claims the USB
   interface) and sends `HELLO`. On Ethernet the request contains the host UDP
   port to which RX sample packets are sent; the destination address is the
   TCP peer address. On USB the session is opened by the `HELLO` itself; a
   `HELLO` within a USB session restarts the session (see [usb.md](../../firmware/src/usb/_doc/usb.md)).
2. The host keeps the session alive by sending a control message at least
   every `PHX_LINK_TIMEOUT_MS` (1000 ms). `KEEPALIVE` every 250 ms is
   recommended; every other control message also counts.
3. The session ends when the TCP connection closes, the USB device is
   reset/deconfigured, or the link timeout expires. The device then stops
   TX immediately (PA_EN off, TX driver off), stops RX streaming and accepts a
   new session.

On Ethernet, a second TCP connection while a session is active is closed
by the device. TX sample packets are accepted only from the session's peer
address.

## Control channel

Every message starts with an 8-byte header:

| Offset | Type | Field | Description |
|---|---|---|---|
| 0 | u16 | magic | `0x4350` ("PC") |
| 2 | u8 | version | 1 |
| 3 | u8 | opcode | Request opcode; responses have bit 7 set |
| 4 | u16 | seq | Chosen by the host, echoed in the response |
| 6 | u16 | length | Payload length (max. 256) |

Each request receives exactly one response with the same `seq`. A response
payload starts with an `int32` status (0 or a negative errno value:
`EIO` -5, `EAGAIN` -11, `EBUSY` -16, `ENODEV` -19, `EINVAL` -22, `ERANGE` -34,
`ENOTSUP` -134), followed by the response data if the status is 0.

On USB each control message is sent as one bulk transfer, terminated by a
short packet or, after a transfer of whole packets, by a zero-length packet
(see [usb.md](../../firmware/src/usb/_doc/usb.md)).

### Requests

| Opcode | Name | Request payload | Response data |
|---|---|---|---|
| 0x01 | HELLO | `proto_version` u16, `rx_udp_port` u16 | `phx_info` |
| 0x02 | KEEPALIVE | - | - |
| 0x03 | GET_INFO | - | `phx_info` |
| 0x04 | GET_STATUS | - | `phx_status` |
| 0x10 | SET_FREQ | `dir` u8, 3 x reserved, `freq_hz` u32 | `frf` u32, `freq_hz` u32 |
| 0x11 | SET_RATE | `rate` u32 | - |
| 0x12 | SET_GAIN | `element` u8, reserved, `gain_db` i16 | `gain_db` i16, reserved |
| 0x13 | SET_RX_INPUT | `value` u8 (0 antenna, 1 RF loopback) | - |
| 0x14 | SET_TX_OUTPUT | `value` u8 (TX driver enable while TX streams) | - |
| 0x20 | READ_REG | `addr` u8, `count` u8, 2 x reserved | `count` bytes |
| 0x21 | WRITE_REG | `addr` u8, `count` u8, 2 x reserved, `count` bytes | - |
| 0x30 | STREAM_RX | `value` u8 (1 enable, 0 disable) | `counter` i64 |
| 0x31 | STREAM_TX | `value` u8 (1 enable, 0 disable) | `counter` i64 |

Notes:

- **SET_FREQ**: 400 to 510 MHz. The actual frequency is
  `frf * xtal_hz / 2^20`; `freq_hz` in the response is rounded to 1 Hz.
- **SET_RATE**: one of the rates in `phx_info.rates`. Returns `EBUSY` while
  streaming.
- **SET_GAIN**: elements and ranges as in SoapySX: LNA 0..48 dB (6 dB steps),
  PGA 0..30 dB (2 dB), DAC 0..9 dB (3 dB), MIXER 0..30 dB (2 dB). The value is
  clamped and quantized; the response contains the value applied.
- **STREAM_RX / STREAM_TX**: the I2S interface and the sample counter start
  with the first stream that is enabled and stop when both are disabled. The
  counter restarts at 0 on every start. The response contains the counter
  value at the time of the command.
- **STREAM_TX enable** switches on PA_EN and, if enabled by SET_TX_OUTPUT
  (default on), the SX1255 TX driver. **Disable** switches both off and
  clears the TX buffer.

### Device information (`phx_info`, 120 bytes)

| Offset | Type | Field |
|---|---|---|
| 0 | u16 | proto_version |
| 2 | u8 | transport (0 Ethernet, 1 USB) |
| 3 | u8 | num_rates |
| 4 | char[32] | fw_version |
| 36 | char[32] | serial (MCU unique ID, 32 hex digits, NUL terminated only if shorter) |
| 68 | u32 | xtal_hz |
| 72 | u32 | freq_min_hz |
| 76 | u32 | freq_max_hz |
| 80 | u16 | max_packet_samples |
| 82 | u16 | reserved |
| 84 | u32 | tx_buffer_samples |
| 88 | u32[8] | rates (highest first) |

### Status (`phx_status`, 32 bytes)

| Offset | Type | Field |
|---|---|---|
| 0 | i64 | counter |
| 8 | u32 | rate |
| 12 | u32 | state (bit 0 RX streaming, 1 TX streaming, 2 PA enabled, 3 RX PLL lock, 4 TX PLL lock) |
| 16 | u32 | rx_dropped |
| 20 | u32 | tx_late |
| 24 | u32 | tx_early |
| 28 | u32 | fifo_errors |

### Events

The device may send `EVENT` (opcode 0x7F, seq 0) at any time; it is not
answered. Payload: `code` u16, reserved u16, `detail` i32.

| Code | Meaning |
|---|---|
| 1 | Stream stopped by an internal error (`detail`: errno) |
| 2 | PA switched off by a safety condition |

## Sample channel

Every sample packet has a 32-byte header followed by `count` CS16 samples
(I then Q, int16, full scale ±32767):

| Offset | Type | Field | Description |
|---|---|---|---|
| 0 | u16 | magic | `0x5350` ("PS") |
| 2 | u8 | version | 1 |
| 3 | u8 | type | 1 RX (device to host), 2 TX (host to device) |
| 4 | u16 | flags | RX: bit 0 = discontinuity |
| 6 | u16 | count | 1..360 samples |
| 8 | u32 | seq | Packet sequence number per direction |
| 12 | u32 | rx_dropped | RX: cumulative RX samples dropped in the device |
| 16 | u32 | tx_late | RX: cumulative TX packets discarded as too late |
| 20 | u32 | tx_early | RX: cumulative TX packets discarded as too early |
| 24 | i64 | timestamp | Sample counter of the first sample |
| 32 | cs16[count] | samples | |

The maximum packet size is 32 + 360 × 4 = 1472 bytes, the largest UDP payload
without IP fragmentation on a 1500-byte MTU. On USB each sample packet is one
bulk transfer with the same termination rule as control messages.

### RX

While RX streaming is enabled the device sends contiguous packets. The
number of samples per packet depends on the sample rate, so that every packet
covers about 0.6 ms (360 samples at 600 kHz, 180 at 300 kHz, 90 at 150 kHz,
45 at 75 kHz, 30 at 50 kHz, 16 at 25 kHz). `timestamp` is the counter value of the first sample, derived from
the SAI DMA position (see [stream.md](../../firmware/src/stream/_doc/stream.md)). If the device had to drop
samples (host or link too slow), the next packet has the discontinuity flag set
and its timestamp reflects the gap; `rx_dropped` counts the lost samples.

The left-aligned 32-bit I2S sample words are converted to CS16 by rounding
to the upper 16 bits with saturation; in TX direction CS16 is shifted into the
upper 16 bits.

### TX

TX runs continuously in full duplex while the RX/TX interface is active; when
no data is present for a sample, zero is transmitted. Every TX packet carries
the counter value at which its first sample is to be sent:

- The device keeps a TX buffer of `tx_buffer_samples` samples indexed by the
  sample counter. A packet is copied to its exact position; overlapping
  packets overwrite earlier data.
- A packet whose first sample is not later than the part of the buffer the
  device has already committed to the DMA (one to two refill blocks ahead
  of the current counter, 16 to 256 samples depending on the rate) is
  **late**: it is discarded completely and `tx_late` is incremented.
- A packet that ends beyond the buffer horizon (current counter +
  `tx_buffer_samples`) is **early**: it is discarded and `tx_early` is
  incremented.
- Packets are only accepted while TX streaming is enabled.

The host is responsible for scheduling: it sends a packet shortly before its
time (lead time configurable in the SoapySDR module), so that the packet
arrives after the earliest acceptable time and before the horizon.

## Time base on the host

The SoapySDR module converts between the sample counter and nanoseconds with
`SoapySDR::ticksToTimeNs` / `timeNsToTicks` at the current sample rate.
`readStream` returns the timestamp of every RX packet with
`SOAPY_SDR_HAS_TIME`. `getHardwareTime()` is estimated from the most recent
RX timestamp plus the host time elapsed since it was received; the device is
not queried.
