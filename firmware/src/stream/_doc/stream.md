# Sample stream and timing

`stream.c` drives SAI1 and the eDMA, keeps the sample counter, the RX ring and
the TX buffer. `sample_io.c` contains the RX pump and the TX packet ingest
shared by both transports.

## I2S interface

The SX1255 is the I2S controller (CK_OUT = bit clock, DIO2 = word select);
its configuration and the bit clock per sample rate are described in
[radio.md](../../radio/_doc/radio.md#sample-rates).

| Parameter | Value |
|---|---|
| Slots | 2 x 32 bit per frame, one frame per I/Q sample |
| Slot order | WS low = I, WS high = Q |
| Alignment | WS changes one bit clock before the MSB, MSB first |
| Bit clock | 64 x sample rate (38.4 MHz at 600 kHz) |

On the MCXN947, SAI1 receives in target mode (RX_BCLK, RX_FS, RXD0). The
transmitter runs synchronous to the receiver (`TCR2.SYNC = 1`) and only uses
TXD0, so TX_BCLK and TX_FS are not connected. The SAI and eDMA are driven
directly with the MCUX HAL drivers (`fsl_sai.c`, `fsl_edma.c`), not with the
Zephyr I2S/DMA drivers.

## DMA and sample counter

Both directions use one eDMA channel each with a single transfer control
descriptor that runs forever over a ring buffer:

- the memory address wraps to the ring start after each major loop
  (`DLAST`/`SLAST` = -ring size), the request is never auto-disabled,
- one minor loop moves 4 words (2 samples) per SAI FIFO request,
- half and major loop interrupts count completed ring halves.

The 64-bit sample counter is the number of RX samples written to memory:

```
counter = halves * half_size + (BITER - CITER) * 2
```

`halves` is corrected by one when the DMA has already crossed a half
boundary whose interrupt is still pending (the parity of `halves` does not
match the half the DMA is in). CITER is read directly from the TCD;
`EDMA_GetRemainingMajorLoopCount()` returns 0 while the DONE flag is set,
which in circular operation lasts until the next minor loop after every major
loop.

The counter is derived only from the SX1255 bit clock. The CPU clock (FRO) is
not involved and differs from it by roughly -900 ppm on the FRDM-MCXN947
used for the measurements, which is irrelevant for the sample timing.

## RX/TX alignment

TE is set before RE. The transmitter waits for the receiver's bit clock, which
starts with RE, so both start on the same frame sync. TX ring element *n* is
therefore sent in the same I2S frame in which RX sample *n* is received; both
share the counter above.

The TX ring is refilled in blocks of half its size from the DMA half/major
interrupts. The block size is chosen per sample rate (about 0.4 ms, 16 to 256
samples). The data for a sample must be available one to two blocks before
its frame.

## Sample paths

**RX:** the eDMA writes the SAI receive FIFO into a ring of 4096 samples
(`stream.c`). The RX pump (`sample_io.c`, every 0.5 ms) reads complete
packets from the ring, converts them to CS16 and sends them with the counter
value of their first sample. A packet covers about 0.6 ms (360 samples at
600 kHz, at least 16 samples). If the host or the link is too slow and the
backlog approaches the ring size, the pump skips ahead, sets the discontinuity flag
and counts the lost samples in `rx_dropped`.

**TX:** received TX packets (`sample_io.c`) are copied into a buffer of 32768
samples indexed by the counter value. The TX DMA interrupt moves the next
block from this buffer into the DMA ring and clears it, so unused positions
are sent as zero. Packets that are too late or beyond the buffer are
discarded and counted (`tx_late`, `tx_early`).

The packet format is defined in
[protocol.md](../../../../common/_doc/protocol.md#sample-channel).

## TX to RX loop delay

A TX sample sent at counter value *N* appears in the RX stream at *N + d*.
The delay *d* consists of the SX1255 interpolation and decimation filters and
the RF path; it does not depend on the host link, the stream start or the
time since boot. Measured with `phx_loopback_test` (TX -> 20 dB attenuator ->
RX cable, 434 MHz, 6 x 100 bursts over 5 stream starts per rate, two board
restarts; identical results over HS USB and Ethernet):

| Sample rate | Delay (samples) | Delay (µs) | Fraction of a sample |
|---|---|---|---|
| 600 kHz | 11 | 18.3 | -0.27 |
| 300 kHz | 9 | 30.0 | -0.12 |
| 150 kHz | 9 | 60.0 | -0.34 |
| 75 kHz | 8 | 106.7 | +0.44 |
| 50 kHz | 8 | 160.0 | +0.41 |
| 25 kHz | 8 | 320.0 | +0.31 |

Every burst appeared at the same integer sample position; the interpolated
peak position varied by at most 0.02 samples. The SX1255 internal RF loopback
(`LB` antenna) gave the same result with the cable connected.

The measurement procedure is described in
[tests.md](../../../../soapy/tests/_doc/tests.md#phx_loopback_test).

## Console

`stream status`, `stream rate`, `stream check`, `stream rxstats` and
`stream tone` show and exercise the sample path, see
[shell.md](../../shell/_doc/shell.md#stream).
