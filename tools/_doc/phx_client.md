# Protocol test client

[`tools/phx_client.py`](../phx_client.py) exercises the wire protocol
([protocol.md](../../common/_doc/protocol.md)) directly over Ethernet,
without the SoapySDR module. It needs Python 3 and the standard library
only and mirrors the constants of `protocol.h`; protocol changes must be
applied here as well.

```sh
python3 tools/phx_client.py --host phenix-1255.local info
python3 tools/phx_client.py --host phenix-1255.local rx --seconds 10
python3 tools/phx_client.py --host phenix-1255.local burst
python3 tools/phx_client.py --host phenix-1255.local late
python3 tools/phx_client.py --host phenix-1255.local timeout
```

Global options: `--host` (default `phenix-1255.local`), `--ctrl-port`
(1255), `--sample-port` (1256). The device must be in Ethernet mode
(`COMM_SEL` closed to GND).

| Command | Checks | Pass condition |
|---|---|---|
| `info` | `GET_INFO` and `GET_STATUS` | Prints all fields |
| `rx [--seconds] [--rate]` | RX stream | No timestamp gaps, no FIFO errors |
| `tone [--seconds] [--rate] [--freq] [--offset] [--amplitude] [--lead-ms] [--lna] [--pga] [--dac] [--mixer]` | Timed TX tone, found in the RX stream | Tone dominates the RX signal, no late TX packets |
| `burst [--rate]` | Bursts of 2 to 60 TX packets 5 ms ahead | No late or early packets |
| `late` | One TX packet in the past, one far in the future | `tx_late` and `tx_early` incremented by one |
| `timeout` | Stops sending keepalives with TX enabled | Session closed after about 1 s, PA off in a new session |

`tone` transmits a test tone. `burst`, `late` and `timeout` enable TX
streaming (PA and SX1255 TX driver on) and send zero samples. Use all four
only with an attenuator or dummy load between TX and RX, or on a frequency
you are allowed to transmit on.
