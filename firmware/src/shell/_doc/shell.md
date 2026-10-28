# Console commands

`shell_commands.c` registers the diagnostic commands on the MCU-Link virtual
COM port (115200 baud, 8N1, `/dev/ttyACM*` on Linux, `/dev/cu.usbmodem*` on
macOS). The Zephyr shell, network shell (`net ...`) and `kernel` commands
are available as well.

The file name must stay unique within Zephyr: it is relocated to SRAMX, and
the relocation script would otherwise pick Zephyr's own `shell_cmds.c`
([firmware.md](../../../_doc/firmware.md#ram)).

## phenix

| Command | Purpose |
|---|---|
| `phenix status` | Hardware variant and HW_ID jumpers, host link selected by `COMM_SEL` at boot and the current pin level, PA state |
| `phenix led <err\|warn\|ok> <off\|on\|slow\|fast>` | Set an LED ([board.md](../../board/_doc/board.md#status-leds)) |
| `phenix wdt_test` | Stop feeding the watchdog, provokes a reset ([board.md](../../board/_doc/board.md#watchdog)) |
| `phenix config show` | Stored network configuration |
| `phenix config set <key> <value>` | Validate and save a network value: `dhcp`, `ip`, `netmask`, `gateway`, `dns`, `ctrl_port`, `sample_port` ([ethernet.md](../../ethernet/_doc/ethernet.md#configuration-on-the-console)) |
| `phenix display` | Display state and settings |
| `phenix display key <prev\|next\|select\|back>` | Simulate a front panel input |
| `phenix display test [s]` | Colour zone test pattern (default 10 s) |
| `phenix display dump` | Print the content last sent to the display ([display.md](../../display/_doc/display.md)) |

Network settings take effect after `kernel reboot cold`.

On unsupported hardware the start-up is halted
([board.md](../../board/_doc/board.md#hardware-variant)). `phenix status`
then shows only the hardware line; `phenix config set` and all `sx1255`
commands are refused.

## sx1255

| Command | Purpose |
|---|---|
| `sx1255 info` | Version, reference, mode, PLL lock, frequencies, rate, gains, RX input, DIO levels |
| `sx1255 dump` | Registers 0x00..0x13 |
| `sx1255 reg read <addr> [count]` | Read registers |
| `sx1255 reg write <addr> <value>` | Write a register |
| `sx1255 reset` | Reset and re-initialize the chip |
| `sx1255 mode <off\|rx\|tx\|rxtx>` | Enable the RX and TX blocks |
| `sx1255 txdrv <on\|off>` | TX driver output (emits RF) |
| `sx1255 freq <rx\|tx> <hz>` | Set a synthesizer frequency |
| `sx1255 rate <S/s>` | Set the sample rate |
| `sx1255 gain <lna\|pga\|dac\|mixer> <dB>` | Set a gain element |
| `sx1255 rxin <antenna\|loopback>` | RX input, RF loopback ([radio.md](../../radio/_doc/radio.md#modes-and-loopback)) |

## stream

| Command | Purpose |
|---|---|
| `stream start` / `stream stop` | Start/stop the I2S interface and the sample counter (the PA is not switched) |
| `stream status` | Counter, FIFO and frame sync errors, late TX refills, packet counters |
| `stream regs` | SAI registers |
| `stream rate [ms]` | Counter rate against the CPU clock |
| `stream check [ms]` | Counter monotonicity under continuous reads |
| `stream rxstats [n] [probe_hz]` | RX level, DC offset, dominant tone frequency |
| `stream tone <hz\|off> [permille]` | TX test tone |

The sample path is described in [stream.md](../../stream/_doc/stream.md).
