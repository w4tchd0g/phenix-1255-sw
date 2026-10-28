# SX1255 radio

`sx1255.c` configures the Semtech SX1255 over SPI (LPSPI1, 10 MHz, GPIO chip
select): reset, register access, frequencies, sample rate, gains, modes and
RF loopback. The I/Q samples run over I2S and are handled by the stream
module ([stream.md](../../stream/_doc/stream.md)). Register definitions are
in `sx1255_regs.h`; pins in [hardware.md](../../../../_doc/hardware.md#pin-assignment).

## Initialization

1. RESET is pulsed high for 1 ms, then the firmware waits 5 ms. RESET is high
   impedance during normal operation.
2. Registers 0x00..0x13 are written from a register cache whose initial
   values follow SoapySX (reference oscillator on, RX/TX off, narrow RX
   filters, ADC trim 7 against ADC spurs, CK_OUT enabled).
3. RX and TX are set to 433.92 MHz, the sample rate to 600 kHz.

The SX1255 runs from the 38.4 MHz TCXO (`clock-frequency` of the devicetree
node).

## Frequencies

400 to 510 MHz, set independently for RX and TX (two synthesizers). The
register value is `frf = freq * 2^20 / f_xtal`; the frequency actually set
(resolution f_xtal / 2^20, about 36.6 Hz) is returned to the caller.

`STAT` (0x11): bit 0 TX PLL lock, bit 1 RX PLL lock, bit 2 oscillator ready.

## Sample rates

The I2S interface runs in IISM mode B2 (register 0x12, bits 5:4 = 10),
standard I2S with 2 x 32 bit per sample. RX and TX are disabled while the
rate changes, because changing it while running corrupts the I2S frame
order.

| Sample rate | Decimation | CK_OUT divider | Bit clock |
|---|---|---|---|
| 600 kHz | 64 | 1 | 38.4 MHz |
| 300 kHz | 128 | 2 | 19.2 MHz |
| 150 kHz | 256 | 4 | 9.6 MHz |
| 75 kHz | 512 | 8 | 4.8 MHz |
| 50 kHz | 768 | 12 | 3.2 MHz |
| 25 kHz | 1536 | 24 | 1.6 MHz |

## Gains

Ranges and steps match SoapySX so that host configurations can be shared.
Values are clamped and quantized; the value actually set is returned.

| Element | Direction | Range | Step |
|---|---|---|---|
| LNA | RX | 0..48 dB | 6 dB (register codes are not linear in dB) |
| PGA | RX | 0..30 dB | 2 dB |
| DAC | TX | 0..9 dB | 3 dB |
| MIXER | TX | 0..30 dB | 2 dB |

## Modes and loopback

`sx1255_set_mode()` enables the RX block, the TX block and the TX driver
(PA driver output) separately. The control module enables the TX driver
only while TX streams and `SET_TX_OUTPUT` allows it
([control.md](../../control/_doc/control.md)).

The RF loopback (RX input `loopback`, `LB` antenna in the SoapySDR module)
needs the TX driver on; the digital loopback of the SX1255 yields no
samples.

DIO0, DIO1 and DIO3 are read as GPIO inputs (`sx1255 info`).
