# SoapySDR test programs

Two programs check the device through the SoapySDR API. They are built with
the module into `build/soapy/` ([building.md](../../../_doc/building.md#soapysdr-module)).
The complete test procedure is in [testing.md](../../../_doc/testing.md).

Tests that transmit must only be used with an attenuator or dummy load
between TX and RX, or with antennas on a frequency you are allowed to
transmit on.

## phx_soapy_test

```sh
build/soapy/phx_soapy_test rx   "driver=phenix-1255" 10
build/soapy/phx_soapy_test tone "driver=phenix-1255" 10 600000 50000
```

Arguments: mode (`rx` or `tone`), device arguments, duration in seconds,
sample rate, tone offset in Hz.

- `rx` verifies that every read carries a timestamp, that timestamps are
  contiguous and that `getHardwareTime()` follows the stream.
- `tone` (transmits) sends a tone in timed 1.5 ms blocks, as
  TETRA-Bluestation does, and checks it in the RX stream. Choose the tone
  offset within the RX bandwidth, e.g. a tenth of the sample rate.

The program aborts on stream errors instead of retrying, so a lost device
cannot fill a redirected log; cap redirected output anyway (`| tail`).

## phx_loopback_test

Verifies that a burst transmitted at sample counter *N* always appears at
the same RX position (transmits). The measured delays are listed in
[stream.md](../../../firmware/src/stream/_doc/stream.md#tx-to-rx-loop-delay).

```sh
build/soapy/phx_loopback_test "driver=phenix-1255" all 20 5
```

Arguments: device arguments, rates (`all` or a comma separated list), bursts
per stream start, stream starts per rate, RX antenna (`RX` or `LB`). The test
transmits a band-limited pseudo-random QPSK burst of 512 samples at a timed
sample position and locates it in the RX stream by cross-correlation. It sets
434 MHz and low gains (LNA 0, PGA 0, DAC 0, MIXER 30 dB) for the attenuator
setup; with higher gains the receiver clips, which makes the correlation
ambiguous. `PHX_VERBOSE=1` prints the delay per stream start, the correlation
peaks and the received power envelope of every burst; check the envelope
before trusting a correlation.
