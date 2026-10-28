# Firmware

Zephyr application for the FRDM-MCXN947, target
`frdm_mcxn947/mcxn947/cpu0`. Building and flashing:
[building.md](../../_doc/building.md#firmware). System overview:
[architecture.md](../../_doc/architecture.md).

## Structure

```
firmware/
├── _doc/           this document
├── boards/         devicetree overlay: pins, SAI1, LPSPI1, LPI2C3, flash partitions
├── dts/bindings/   binding for the SX1255 node (semtech,sx1255)
├── prj.conf        Kconfig: network, USB, settings, shell, thread priorities
├── CMakeLists.txt  sources, SRAMX relocation, MCUX SAI/eDMA drivers
├── VERSION         firmware version
└── src/
    ├── main.c      start-up sequence, selection of the host link
    └── <module>/   one folder per module, each with its own _doc
```

| Module | Files | Purpose | Documentation |
|---|---|---|---|
| `board/` | `board_io.c`, `watchdog.c` | Status LEDs, PA enable, `COMM_SEL`, factory reset button, hardware watchdog | [board.md](../src/board/_doc/board.md) |
| `config/` | `config_store.c` | Persistent network and display settings (NVS) | [config.md](../src/config/_doc/config.md) |
| `radio/` | `sx1255.c`, `sx1255_regs.h` | SX1255 over SPI: frequencies, gains, sample rate, modes | [radio.md](../src/radio/_doc/radio.md) |
| `stream/` | `stream.c`, `sample_io.c` | SAI1 and eDMA, sample counter, RX pump, TX ingest | [stream.md](../src/stream/_doc/stream.md) |
| `control/` | `control.c` | Host session and control commands, independent of the transport | [control.md](../src/control/_doc/control.md) |
| `ethernet/` | `transport_eth.c`, `net_config.c`, `web_ui.c` | TCP/UDP transport, addressing, mDNS, web interface | [ethernet.md](../src/ethernet/_doc/ethernet.md) |
| `usb/` | `transport_usb.c` | HS USB vendor function with bulk endpoints | [usb.md](../src/usb/_doc/usb.md) |
| `display/` | `display_ui.c`, `display_fb.c` | Front panel: OLED status pages, menu, encoder | [display.md](../src/display/_doc/display.md) |
| `shell/` | `shell_commands.c` | Console commands for diagnostics | [shell.md](../src/shell/_doc/shell.md) |

Headers are included with their module folder (`#include "stream/stream.h"`);
`firmware/src` and `common/include` are the include directories. The wire
protocol shared with the host is in
[`common/include/phenix1255/protocol.h`](../../common/include/phenix1255/protocol.h).

## Start-up

1. `EARLY` init level: PA_EN is driven inactive before clock setup and before
   any driver ([board.md](../src/board/_doc/board.md#pa-enable)).
2. `main()`: board I/O, watchdog (red LED blinks slowly after a watchdog
   reset), factory reset check (SW2), settings, SX1255 (433.92 MHz, 600 kHz),
   stream, control and RX pump.
3. Ethernet is always configured and the web interface and the front panel
   are started, independent of `COMM_SEL`.
4. Depending on `COMM_SEL`, the Ethernet or the USB transport is started.
   The green LED then blinks slowly (ready, no host).

An initialization error lights the red LED; the firmware keeps running so
that the console and the web interface stay available.

## Threads and interrupts

| Context | Priority | Work |
|---|---|---|
| eDMA RX/TX interrupts | DT priority | Count ring halves, refill the TX ring from the TX buffer |
| SAI interrupt | 2 | FIFO errors (stream fault, PA off), frame sync error counters |
| LPI2C3, GPIO port 0/1 interrupts | 3 | Display bus, encoder and button edges |
| USB device stack thread | cooperative 8 | USB events, TX packet ingest on USB |
| ENET-QOS RX work queue | cooperative 1 | Ethernet frame reception (descriptor handling) |
| UDP sample thread | preemptive 3 | TX packet ingest on Ethernet |
| RX pump | preemptive 4 | Packs RX samples every 0.5 ms and sends them |
| Network RX thread (`rx_q[0]`) | preemptive 5 | IP/UDP/TCP processing of received frames |
| Control threads (TCP, USB) | preemptive 6 | Control commands of the session |
| Web interface | preemptive 8 | HTTP server |
| Front panel | preemptive 9 | Display refresh every 250 ms, menu |
| Watchdog feeder | preemptive 10 | Feeds the watchdog while the system is responsive |
| System work queue | | Link timeout, stream faults, DHCP fallback, USB link loss, button debounce |

The RX ring holds 5.6 ms at 600 kS/s, so nothing above the RX pump may run
that long. The network RX thread is therefore preemptive and below the
pump (`CONFIG_NET_TC_THREAD_PREEMPTIVE`): as the default cooperative thread
it processed a burst of incoming TX packets (a host catching up after a
stall) in one go and starved the pump for 5 to 7 ms, which lost about 3000
RX samples at every stream start and a few times per minute in full duplex
over Ethernet.

## PA safety

PA_EN is driven off as early as possible during boot and is switched on only
by `STREAM_TX`. It is switched off immediately on `STREAM_TX` off, stream
stop, SAI FIFO errors, session end (TCP close, USB reset, deconfiguration,
suspend or VBUS loss, link timeout of 1 s) and before a reboot from the web
interface or the front panel menu (`control_reboot()`). The watchdog resets
the MCU if the system hangs; the reset ends any transmission and the early
boot code drives PA_EN off again. While the MCU is in reset, PA_EN is high
impedance and the pull-up on the PA gate of phenix-1255-hw keeps the PA off
(see [board.md](../src/board/_doc/board.md#pa-enable)).

Every new error or link-loss path must switch the PA off immediately.

## Memory

### RAM

The second core is not used, so SRAM A..H (416 KB at 0x20000000) belongs to
the application core. It holds all buffers that DMA engines access (SAI
rings and TX buffer, Ethernet and USB buffers) and the network stack.
Buffers and thread stacks that only the CPU accesses (sample packet
assembly, web interface, console diagnostics, Ethernet transport, front
panel display) are placed in SRAMX (96 KB at 0x04000000) with Zephyr code
relocation, see `firmware/CMakeLists.txt`. Files listed there must not
contain DMA buffers.

The relocation script finds the object file by its file name and the name of
its parent folder and can pick a Zephyr object with the same names instead.
File names in the relocation list must therefore not exist in Zephyr; check
the memory report after every change (RAM about 94 %, SRAMX about 47 %).

### Flash

MCUboot is not used. The internal flash (2 MB) is split into:

| Partition | Offset | Size |
|---|---|---|
| code | 0x000000 | 1984 KB |
| settings (NVS) | 0x1F0000 | 64 KB |

The settings partition and the flash cache handling are described in
[config.md](../src/config/_doc/config.md).
