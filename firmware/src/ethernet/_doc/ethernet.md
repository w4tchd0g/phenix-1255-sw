# Ethernet

`transport_eth.c` runs the TCP control channel and the UDP sample channel,
`net_config.c` the addressing (DHCP, fallback, static) and mDNS, `web_ui.c`
the web interface. Ethernet is always active, independent of the `COMM_SEL`
switch: the web interface is reachable in both modes. The TCP control
channel and the UDP sample channel are only started when `COMM_SEL` selects
Ethernet. Message formats: [protocol.md](../../../../common/_doc/protocol.md).

## Addressing

| Setting | Behaviour |
|---|---|
| DHCP on (default) | DHCPv4 client. If no lease is obtained within 30 s, the fallback address **192.168.2.10/24** is added. DHCP keeps running; when a lease is obtained later, the fallback address is removed. |
| DHCP off | Static address, netmask, gateway (optional) and DNS server (optional) from the configuration. |

In DHCP mode the DNS server is taken from the DHCP lease; the configured DNS
server is used only in static mode.

The MAC address is derived from the MCU unique ID (locally administered
address), so it is stable across reboots and DHCP reservations work.

## Host name

The device answers mDNS queries for **`phenix-1255.local`**. Only one device
with this name should be on a network segment.

## Ports

| Port | Protocol | Use |
|---|---|---|
| 80 | TCP | Web interface |
| 1255 (configurable) | TCP | Control channel |
| 1256 (configurable) | UDP | Sample channel |
| 5353 | UDP | mDNS |

## Status LEDs

| LED | State | Meaning |
|---|---|---|
| LED1 yellow | slow blink | Waiting for a DHCP lease |
| LED1 yellow | on | Fallback address 192.168.2.10 in use |
| LED1 yellow | off | Address configured (DHCP lease or static) |
| LED1 yellow | short flash | Late TX packet or RX samples dropped |
| LED2 green | slow blink | Ready, no host session |
| LED2 green | on | Host session active |

## Web interface

`http://phenix-1255.local/` (or the device IP address) shows the device status
and a form for the network settings. There is no authentication; the device
is intended for a trusted local network.

| Path | Method | Content |
|---|---|---|
| `/` | GET | Status and configuration form |
| `/config` | POST | Save the settings (`application/x-www-form-urlencoded`) |
| `/status.json` | GET | Status as JSON |

Saving validates all fields (address, netmask, gateway in the local subnet,
ports 1..65535, control port not 80). Invalid input is rejected and nothing is
stored. After a successful save the device switches TX off (PA off), closes
the host session and reboots with the new settings.

Form fields of `/config`: `dhcp` (`on`/`off`), `ip`, `netmask`, `gateway`,
`dns` (empty = none), `ctrl_port`, `sample_port`.

The web interface is active in both `COMM_SEL` modes.

On macOS, name resolution of `phenix-1255.local` in browsers and `curl` may
take a few seconds: the system also asks for an IPv6 address, which the
IPv4-only device does not answer. Using the IP address avoids the delay.

## Configuration on the console

The network configuration can also be changed on the MCU-Link console; it
takes effect after a reboot:

```
phenix config show
phenix config set dhcp off
phenix config set ip 192.168.2.10
phenix config set netmask 255.255.255.0
phenix config set gateway 192.168.2.1
phenix config set dns 192.168.2.1
phenix config set ctrl_port 1255
phenix config set sample_port 1256
kernel reboot cold
```

Holding SW2 during boot restores the factory defaults.

## Configuration on the front panel

The *Network* menu of the front panel display sets DHCP or a static
address, netmask, gateway and DNS server with the rotary encoder; the ports
are not editable there. *Save+reboot* applies the same validation as the
web interface and reboots. See [display.md](../../display/_doc/display.md#menu).

## Test client

[phx_client.md](../../../../tools/_doc/phx_client.md) describes a Python
client that exercises the protocol over Ethernet without the SoapySDR module.
