/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Phenix-1255 contributors
 *
 * Sample packet handling, independent of the transport: the RX pump that
 * packs received samples into sample packets, and the TX packet ingest.
 */

#ifndef PHENIX_SAMPLE_IO_H_
#define PHENIX_SAMPLE_IO_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

int sample_io_init(void);

/** Reset packet sequence numbers and drop counters (new session). */
void sample_io_reset(void);

/** Start or stop sending RX packets, starting at the current counter. */
void sample_io_rx_enable(bool enable);

uint32_t sample_io_rx_dropped(void);

struct sample_io_stats {
	uint32_t rx_packets;   /**< RX packets sent */
	uint32_t rx_dropped;   /**< RX samples dropped */
	uint32_t tx_packets;   /**< Valid TX packets received */
	uint32_t tx_seq_gaps;  /**< TX packets missing according to the sequence */
	uint32_t tx_invalid;   /**< Malformed TX packets */
};

void sample_io_get_stats(struct sample_io_stats *stats);

/** Validate a received TX sample packet and place it in the TX buffer. */
void sample_io_handle_tx_packet(const uint8_t *buf, size_t len);

#endif /* PHENIX_SAMPLE_IO_H_ */
