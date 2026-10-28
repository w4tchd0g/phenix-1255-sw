/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Phenix-1255 contributors
 *
 * I/Q sample engine: SAI1 in I2S target mode (clocks from the SX1255),
 * TX synchronous to RX, both serviced by circular eDMA transfers.
 *
 * The shared 64-bit sample counter counts I2S frames since stream start.
 * RX sample n and TX sample n belong to the same I2S frame n.
 */

#ifndef PHENIX_STREAM_H_
#define PHENIX_STREAM_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phenix1255/protocol.h>

/* One I/Q sample as transferred over I2S: two left-aligned 32-bit words */
struct stream_iq {
	int32_t i;
	int32_t q;
};

/* RX ring size in samples (power of two) */
#define STREAM_RX_RING_SAMPLES 4096U

/* TX buffer indexed by the sample counter (power of two) */
#define STREAM_TX_BUFFER_SAMPLES 32768U

struct stream_stats {
	uint64_t counter;          /**< Samples received since start */
	uint32_t rate;             /**< Configured sample rate */
	uint32_t tx_block_samples; /**< TX refill granularity */
	uint32_t rx_fifo_errors;   /**< SAI RX FIFO overruns */
	uint32_t tx_fifo_errors;   /**< SAI TX FIFO underruns */
	uint32_t rx_sync_errors;   /**< SAI RX frame sync errors */
	uint32_t tx_sync_errors;   /**< SAI TX frame sync errors */
	uint32_t tx_refill_late;   /**< TX refills that missed their deadline */
	uint32_t tx_late;          /**< TX packets discarded as too late */
	uint32_t tx_early;         /**< TX packets discarded as too early */
	uint64_t tx_blocks;        /**< TX refill blocks since start */
	bool running;
	bool tx_enabled;
};

/**
 * Called from the system work queue when the stream was stopped because
 * of an internal error (SAI FIFO error). The PA is already off.
 */
typedef void (*stream_fault_handler_t)(int err);

int stream_init(void);

void stream_set_fault_handler(stream_fault_handler_t handler);

/**
 * Start RX and TX at the current SX1255 sample rate. Enables the SX1255
 * receiver and transmitter (without TX driver) and resets the counter.
 */
int stream_start(void);

/** Stop streaming. Disables TX (PA off) first. */
int stream_stop(void);

bool stream_is_running(void);

/** Current sample counter: number of RX samples completely written. */
uint64_t stream_counter(void);

void stream_get_stats(struct stream_stats *stats);

/**
 * Read RX samples as CS16.
 *
 * @retval 0          Samples [index, index + count) copied
 * @retval -EAGAIN    Not yet received, or stream not running
 * @retval -EOVERFLOW Already overwritten in the ring
 */
int stream_rx_read(uint64_t index, struct phx_cs16 *dst, size_t count);

/**
 * Enable or disable TX. Enabling switches on PA_EN and, if tx_driver is
 * set, the SX1255 TX driver. Disabling switches both off and clears the
 * TX buffer.
 */
int stream_tx_enable(bool enable, bool tx_driver);

bool stream_tx_is_enabled(void);

/**
 * Place TX samples at their sample counter position.
 *
 * @retval 0       Accepted
 * @retval -EAGAIN TX not enabled
 * @retval -ETIME  Too late, the position is already committed to the DMA
 * @retval -EFBIG  Too early, beyond the TX buffer horizon
 */
int stream_tx_write(int64_t timestamp, const struct phx_cs16 *src, size_t count);

/**
 * Copy the most recent RX samples (diagnostics).
 *
 * @param dst         Destination buffer
 * @param count       Number of samples, at most STREAM_RX_RING_SAMPLES / 2
 * @param first_index Receives the counter value of dst[0]
 */
int stream_rx_snapshot(struct stream_iq *dst, size_t count, uint64_t *first_index);

/**
 * Test signal on the TX path: complex tone at freq_hz (may be negative)
 * with the given amplitude in permille of full scale. While a tone is
 * active, it replaces the TX buffer contents. Amplitude 0 turns it off.
 */
void stream_tx_set_tone(int32_t freq_hz, uint16_t amplitude_permille);

/** Raw SAI register values for diagnostics. */
struct stream_sai_regs {
	uint32_t tcsr, tcr2, tcr3, tcr4, tcr5, tfr0;
	uint32_t rcsr, rcr2, rcr3, rcr4, rcr5, rfr0;
};

void stream_get_sai_regs(struct stream_sai_regs *regs);

#endif /* PHENIX_STREAM_H_ */
