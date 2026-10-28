# Control and session

`control.c` implements the host session and the control commands of the
wire protocol ([protocol.md](../../../../common/_doc/protocol.md)),
independent of the transport. The Ethernet and USB transports
([ethernet.md](../../ethernet/_doc/ethernet.md), [usb.md](../../usb/_doc/usb.md))
register a `struct control_transport` with functions to send control
messages and RX sample packets, to set the RX destination port (Ethernet) and
to close the link from the device side.

## Session

- `control_session_open()` opens the session for one transport and returns
  `EBUSY` if another session is open. The green LED lights while a session
  is open and blinks slowly otherwise.
- Every valid control message restarts the link timeout of
  `PHX_LINK_TIMEOUT_MS` (1000 ms). When it expires, the session is closed and
  the transport is told to drop the link.
- `control_session_close()` stops TX (PA and TX driver off), RX and the
  I2S interface. `control_session_terminate()` (link timeout, device-side
  close) switches TX off before it takes the session lock, so the PA goes
  off even while a sender is blocked.
- A mutex serializes command execution, session open/close and sending, so
  a command that switches the PA cannot interleave with a session close.

## Commands

Requests are dispatched by opcode; each request gets exactly one response
with the same sequence number. Notable behaviour:

- `STREAM_RX` / `STREAM_TX` start the I2S interface and the sample counter
  with the first enabled stream and stop them when both are disabled.
- `STREAM_TX` enable switches on PA_EN and, if allowed by `SET_TX_OUTPUT`,
  the SX1255 TX driver; disable switches both off and clears the TX buffer.
- `SET_RATE` returns `EBUSY` while a stream runs.
- A stream fault (SAI FIFO error) switches the PA off in the stream module;
  the control module then marks the streams stopped, lights the red LED and
  sends the events "stream stopped" and "PA switched off" to the host.

## Reboot

`control_reboot()` is used by the web interface and the front panel menu.
It closes the session (the host sees the session end), switches the PA off,
optionally erases the settings partition (factory reset) and reboots after a
short delay that lets the TCP close and the log output complete.
