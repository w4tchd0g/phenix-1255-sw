# Persistent configuration

`config_store.c` stores the network and display settings with the Zephyr
settings subsystem (NVS backend) in a dedicated 64 KB internal flash
partition at 0x1F0000 ([firmware.md](../../../_doc/firmware.md#flash)).

## Settings

| Key | Content | Users |
|---|---|---|
| `phenix/net` | DHCP on/off, static address, netmask, gateway, DNS, control port, sample port | [ethernet.md](../../ethernet/_doc/ethernet.md) (web interface, console), front panel menu |
| `phenix/display` | Brightness, dim timeout, auto pages, encoder direction | [display.md](../../display/_doc/display.md) |

Factory defaults: DHCP on, fallback/static address 192.168.2.10/24, no
gateway and DNS, control port 1255/TCP, sample port 1256/UDP; brightness 8,
dim after 5 min, auto pages on, encoder direction normal.

- Every network configuration is validated (`config_store_validate()`)
  before it is stored, and again when it is loaded; an invalid stored record
  is ignored and the defaults apply. The web interface, the console and the
  front panel menu use the same validation.
- Network settings take effect after the next boot.
- A display record of the previous format, without the encoder direction, is
  read as well: its settings stay valid after a firmware update, and the
  encoder direction starts as normal.
- A factory reset (SW2 at boot, or the front panel menu) erases the whole
  partition ([board.md](../../board/_doc/board.md#factory-reset)).

## Flash caches

The flash driver of Zephyr v4.5.0-rc3 invalidates the flash cache and the
LPCAC code cache after every program and erase operation but leaves the
clear bits (`NVM_CTRL[CLR_FLASH_CACHE]`, `LPCAC_CTRL[CLR_LPCAC]`) set, which
holds both caches cleared. The code then runs uncached until the next reset;
on Ethernet the RX pump keeps up with only about 220 kS/s of 600 kS/s.
`config_store.c` releases both bits after every flash operation; any new
flash write must do the same.

The driver also locks interrupts while it programs or erases, so settings are
written at runtime only when no stream runs or the device reboots
afterwards. Display settings changed during a stream are saved when it
stops.
