# Persistent configuration

`config_store.c` stores the network, host link and display settings with
the Zephyr settings subsystem (NVS backend) in a dedicated 64 KB internal
flash partition at 0x1F0000 ([firmware.md](../../../_doc/firmware.md#flash)).

## Settings

| Key | Content | Users |
|---|---|---|
| `phenix/net` | DHCP on/off, static address, netmask, gateway, DNS, control port, sample port | [ethernet.md](../../ethernet/_doc/ethernet.md) (web interface, console), front panel menu |
| `phenix/link` | Host link: HS USB or Ethernet | [Host link](#host-link) |
| `phenix/display` | Brightness, dim timeout, auto pages | [display.md](../../display/_doc/display.md) |

Factory defaults: DHCP on, fallback/static address 192.168.2.10/24, no
gateway and DNS, control port 1255/TCP, sample port 1256/UDP; host link
HS USB; brightness 8, dim after 5 min, auto pages on.

- Every network configuration is validated (`config_store_validate()`)
  before it is stored, and again when it is loaded; an invalid stored record
  is ignored and the defaults apply. The web interface, the console and the
  front panel menu use the same validation.
- Network settings take effect after the next boot.
- A display record of an older format with one more byte, for a setting
  that no longer exists, is read as well; its other settings stay valid
  after a firmware update.
- A factory reset (SW2 at boot, or the front panel menu) erases the whole
  partition ([board.md](../../board/_doc/board.md#factory-reset)).

## Host link

The host link selects the interface for the control channel and the sample
stream: HS USB ([usb.md](../../usb/_doc/usb.md)) or Ethernet
([ethernet.md](../../ethernet/_doc/ethernet.md)). Ethernet and the web
interface are active with both settings. Without a stored value, e.g. after
a factory reset, HS USB is used.

The link is read once at boot and the matching transport is started; a
change is stored and takes effect after a reboot. It can be changed in three
places:

- Front panel: *Host link* in the main menu asks for confirmation, saves and
  reboots ([display.md](../../display/_doc/display.md#menu)).
- Web interface: the *Host link* field of the configuration form, saved
  together with the network settings before the reboot
  ([ethernet.md](../../ethernet/_doc/ethernet.md#web-interface)).
- Console: `phenix config set link <usb|ethernet>`, then
  `kernel reboot cold` ([shell.md](../../shell/_doc/shell.md#phenix)).

Until the reboot, `phenix status`, the web interface and the NETWORK page
of the front panel show the link in use together with the stored one.

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
