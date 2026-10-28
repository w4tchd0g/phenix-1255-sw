/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Phenix-1255 contributors
 *
 * Host session and control command handling, independent of the transport
 * (Ethernet or HS USB). See common/_doc/protocol.md.
 */

#ifndef PHENIX_CONTROL_H_
#define PHENIX_CONTROL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/toolchain.h>

#include <phenix1255/protocol.h>

/* Operations a transport provides for an open session */
struct control_transport {
	enum phx_transport type;
	/** Send one complete control message to the host. */
	int (*send_ctrl)(const void *msg, size_t len);
	/** Send one RX sample packet to the host. */
	int (*send_sample)(const void *pkt, size_t len);
	/** Set the RX sample destination port from HELLO (Ethernet only). */
	void (*set_rx_port)(uint16_t port);
	/** Terminate the session from the device side (link timeout). */
	void (*close)(void);
};

int control_init(void);

/**
 * Open a session on a transport.
 *
 * @retval -EBUSY Another session is active
 */
int control_session_open(const struct control_transport *transport);

/**
 * Close the session (connection lost or closed by the transport).
 * Stops TX (PA off) and all streaming.
 */
void control_session_close(void);

/**
 * Terminate the session from the device side: switch TX off, ask the
 * transport to drop the connection and close the session.
 */
void control_session_terminate(void);

bool control_session_active(void);

/**
 * Leave the RF path in a safe state, end the host session and reset the
 * SoC. With factory_reset, all stored settings are erased before the reset.
 * Must be called from a thread that may sleep; does not return.
 */
FUNC_NORETURN void control_reboot(bool factory_reset);

/**
 * Process one complete control message received from the host and send
 * the response. Malformed messages are dropped.
 */
void control_handle_message(const uint8_t *msg, size_t len);

/** Send an RX sample packet over the session transport. */
int control_send_sample(const void *pkt, size_t len);

#endif /* PHENIX_CONTROL_H_ */
