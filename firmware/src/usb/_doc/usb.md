# HS USB

`transport_usb.c` implements the USB vendor function with four bulk
endpoints on the device stack `device_next`. When the `COMM_SEL` switch is
open (see [board.md](../../board/_doc/board.md#comm_sel-switch)), the host
link runs over the high-speed USB port of the FRDM-MCXN947 (USB1, EHCI
controller with integrated HS PHY). Ethernet and the web interface stay
active in this mode, but host sessions are only accepted over USB.

## USB identification

| Field | Value |
|---|---|
| VID:PID | `1209:000A` (pid.codes test PID, see below) |
| Manufacturer string | `Phenix-1255` |
| Product string | `Phenix-1255 SX1255 SDR` |
| Serial number string | MCU unique ID, 32 hex digits |
| Device class | 0 (defined by the interface) |
| Power | Bus powered, `bMaxPower` 500 mA |

The serial number is the same unique ID that `GET_INFO` reports in
`phx_info.serial`. The USB string uses upper-case hex digits, `phx_info` and
the SoapySDR module use lower case; the module compares serial numbers
case-insensitively.

`bMaxPower` is a placeholder and must be adjusted to the measured current
draw of the FRDM-MCXN947 with phenix-1255-hw (see
[testing.md](../../../../_doc/testing.md#phenix-1255-hw-bring-up)).

### About the test PID 1209:000A

`1209` is the vendor ID of [pid.codes](https://pid.codes), which assigns
product IDs to open source hardware projects free of charge. The PIDs
`1209:0001` to `1209:000F` are reserved as **test PIDs**:

- They may be used by anybody for private testing and development.
- They are **not unique**: other devices on the same computer may use the
  same VID:PID. Host software therefore cannot rely on VID:PID alone to
  identify the device. The SoapySDR module additionally checks the
  interface layout (vendor class, the four bulk endpoints below).
- They **must not be used for devices that are distributed** to other people,
  sold or published as a finished product.

Before devices are built for others, apply for an own PID at pid.codes (the
process is a pull request against the pid.codes repository and requires an
open source license for hardware and firmware). Then change `PHX_USB_PID` in
[`common/include/phenix1255/protocol.h`](../../../../common/include/phenix1255/protocol.h)
(used by firmware and SoapySDR module) and the udev rule
[`soapy/udev/99-phenix-1255.rules`](../../../../soapy/udev/99-phenix-1255.rules).

## Interface and endpoints

One configuration with one vendor-specific interface (class `0xFF`,
interface 0, no alternate settings):

| Endpoint | Type | Direction | Channel |
|---|---|---|---|
| 0x01 | Bulk | Host to device | Control requests |
| 0x81 | Bulk | Device to host | Control responses and events |
| 0x02 | Bulk | Host to device | TX sample packets |
| 0x82 | Bulk | Device to host | RX sample packets |

The maximum packet size is 512 bytes at high speed and 64 bytes at full
speed. The firmware also enumerates at full speed, but 12 Mbit/s are not
sufficient for 600 kHz in both directions (2.4 MB/s each).

The message formats are the same as on Ethernet, see
[protocol.md](../../../../common/_doc/protocol.md).

### Transfer framing

Every control message and every sample packet is exactly one bulk transfer.
A transfer ends with a short packet; a transfer whose length is a multiple
of the maximum packet size (for example a TX packet of 120 or 248 samples,
512 or 1024 bytes) is terminated by a zero-length packet. Both sides follow
this rule:

- The firmware sets the ZLP flag on IN transfers of whole packets.
- The SoapySDR module sends a zero-length transfer after such an OUT
  transfer.

## Session handling

USB has no connection setup, so the session is bound to the protocol:

1. The host claims interface 0 and sends `HELLO` on endpoint 0x01. The device
   opens the session with this `HELLO`. Control messages outside a session
   are ignored.
2. A `HELLO` within a session comes from a new host process (the previous one
   ended without the device noticing). The device closes the old session
   (PA off, streams stopped) and opens a new one.
3. The session ends on:
   - USB bus reset, deconfiguration or suspend (host asleep, cable removed),
   - VBUS removal,
   - the link timeout (no control message for 1000 ms).

   As on Ethernet, the device then switches TX off immediately (PA_EN off,
   TX driver off) and stops all streaming.

A host that closes normally disables TX and RX first; the device closes the
session one second later by the link timeout. A new host may connect at any
time, including within this second (case 2 above).

Data that the old host did not read stays queued in the device (up to 8 RX
packets and 4 control messages) and is delivered to the next host. This is
harmless: RX packets that arrive before `STREAM_RX` are ignored, and the
SoapySDR module starts its control sequence numbers at a random value so that
old responses cannot match new requests. Cancelling these transfers on the
device would be possible only by disabling the endpoints.

## Device buffers

| Buffer | Count x size | Purpose |
|---|---|---|
| Control OUT | 4 x 512 B | One transfer queued, others waiting for the control thread |
| Control IN | 4 x 268 B | Responses and events |
| Sample OUT | 3 x 1536 B | TX packets, processed in the USB stack thread |
| Sample IN | 8 x 1472 B | RX packets, about 5 ms at 600 kHz |

If the host does not read RX packets in time, the device drops them and
reports the loss in `rx_dropped` and the discontinuity flag, exactly as on
Ethernet.

## Host access

Linux needs a udev rule for USB access by normal users; macOS needs no
driver; Windows is not supported. See
[soapy.md](../../../../soapy/_doc/soapy.md#host-access).

## Testing

```sh
export SOAPY_SDR_PLUGIN_PATH="$PWD/build/soapy"
SoapySDRUtil --find                                     # lists USB devices
SoapySDRUtil --probe="driver=phenix-1255,transport=usb"
SoapySDRUtil --args="driver=phenix-1255,transport=usb" --rate=600e3 --direction=RX
build/soapy/phx_soapy_test tone "driver=phenix-1255,transport=usb" 10 600000 60000
```

`tone` transmits; use an attenuator or dummy load between TX and RX.

The console shows `USB configured (high speed)` when the host has configured
the device and `Host connected (USB high speed)` when a session opens.
