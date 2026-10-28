# SoapySDR module

The module `phenix1255Support` registers the SoapySDR driver `phenix-1255`
for Linux and macOS. It talks to the firmware over the wire protocol
([protocol.md](../../common/_doc/protocol.md)) on Ethernet or HS USB.
Building and installing: [building.md](../../_doc/building.md#soapysdr-module).
Test programs: [tests.md](../tests/_doc/tests.md).

## Source files

| File | Purpose |
|---|---|
| `Registration.cpp` | Device discovery (USB enumeration, network probe) and device arguments |
| `Phenix1255Device.cpp` | SoapySDR device API: frequencies, gains, antennas, rates, streams |
| `ControlClient.cpp` | Control requests and responses, keepalive thread |
| `SampleStream.cpp` | RX packet ring, TX scheduler with lead time, host time estimate |
| `Transport.hpp` | Transport interface shared by Ethernet and USB |
| `TransportNet.cpp` | TCP control and UDP sample sockets |
| `TransportUsb.cpp` | libusb bulk transfers (asynchronous for samples) |

libusb-1.0 (1.0.21 or newer) is optional: without it only `TransportNet` is
built. Platform-specific socket options are selected by feature macros
(`SO_NOSIGPIPE` / `MSG_NOSIGNAL`), not by OS checks.

## Streams and time

`readStream` returns every packet with `SOAPY_SDR_HAS_TIME`. Timed
`writeStream` calls are split into packets with exact counter values; the TX
scheduler sends each packet `lead_ms` (default 20 ms) before its time, so the
device holds about 20 ms of TX data. `getHardwareTime()` is estimated from
the most recent RX timestamp and the host time since its arrival; the device
is not queried. Counter values and nanoseconds are converted with
`SoapySDR::ticksToTimeNs` / `timeNsToTicks` at the current sample rate.

The control client sends `KEEPALIVE` every 250 ms and starts its sequence
numbers at a random value, so that responses queued for a previous host
cannot match new requests.

## Device arguments

| Argument | Default | Description |
|---|---|---|
| `driver` | | `phenix-1255` |
| `transport` | see below | `usb` or `eth` |
| `addr` | `phenix-1255.local` | Host name or IPv4 address (Ethernet) |
| `ctrl_port` | 1255 | TCP control port |
| `sample_port` | 1256 | UDP sample port |
| `serial` | | Select a device by serial number |
| `lead_ms` | 20 | TX lead time (also setting `lead_ms`) |

Without `transport`, an attached USB device is used if there is one and no
`addr` is given, otherwise Ethernet. With several USB devices, `serial`
selects one; serial numbers are compared case-insensitively.

A generic `SoapySDRUtil --find` without arguments lists attached USB devices
but does not search the network (mDNS timeouts would delay every SoapySDR
application); use `driver=phenix-1255` or `addr=...` for Ethernet devices.

Because the USB test PID is not unique, USB devices are identified by
VID:PID and the interface layout (vendor class, four bulk endpoints), see
[usb.md](../../firmware/src/usb/_doc/usb.md#about-the-test-pid-1209000a).

## Host access

### Linux

Normal users need access rights to the USB device node. Install the udev
rule `soapy/udev/99-phenix-1255.rules`:

```sh
sudo cp soapy/udev/99-phenix-1255.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger
```

The rule grants access to the logged-in user (`uaccess`) and to members of
the `plugdev` group. Without it, `SoapySDRUtil --find` lists the device with
the label "no access" and opening it fails.

### macOS

No driver or configuration is needed; libusb accesses the device directly.

### Windows

Not supported: the firmware provides no Microsoft OS descriptors, so Windows
does not bind WinUSB automatically.
