/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Phenix-1255 contributors
 *
 * SX1255 control over SPI: reset, register access, frequency, sample rate,
 * gains and loopback. The I/Q sample path is handled by the stream module.
 */

#ifndef PHENIX_SX1255_H_
#define PHENIX_SX1255_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum sx1255_dir {
	SX1255_RX,
	SX1255_TX,
};

/*
 * Gain elements. Ranges and steps match SoapySX so that host
 * configurations can be shared:
 *   LNA   0..48 dB, 6 dB steps (RX)
 *   PGA   0..30 dB, 2 dB steps (RX)
 *   DAC   0..9 dB,  3 dB steps (TX)
 *   MIXER 0..30 dB, 2 dB steps (TX)
 */
enum sx1255_gain {
	SX1255_GAIN_LNA,
	SX1255_GAIN_PGA,
	SX1255_GAIN_DAC,
	SX1255_GAIN_MIXER,
	SX1255_GAIN_COUNT,
};

struct sx1255_gain_range {
	int min_db;
	int max_db;
	int step_db;
};

/** RX input selection */
enum sx1255_rx_input {
	SX1255_RX_INPUT_ANTENNA,
	SX1255_RX_INPUT_LOOPBACK, /**< Internal RF loopback from the TX path */
};

/* Supported frequency range of the SX1255 in Hz */
#define SX1255_FREQ_MIN_HZ 400000000U
#define SX1255_FREQ_MAX_HZ 510000000U

/* Sample rates are f_xtal / div with div from this list */
#define SX1255_NUM_SAMPLE_RATES 6

/** Reset the chip, verify its version and load the default configuration. */
int sx1255_init(void);

bool sx1255_is_ready(void);

uint32_t sx1255_xtal_hz(void);

int sx1255_read_regs(uint8_t addr, uint8_t *buf, size_t len);
int sx1255_write_regs(uint8_t addr, const uint8_t *buf, size_t len);

/** Enable receiver, transmitter and TX driver output. */
int sx1255_set_mode(bool rx, bool tx, bool tx_driver);
void sx1255_get_mode(bool *rx, bool *tx, bool *tx_driver);

/**
 * Set a synthesizer frequency.
 *
 * @param actual_hz Optional, receives the frequency actually set
 *                  (resolution f_xtal / 2^20).
 */
int sx1255_set_frequency(enum sx1255_dir dir, uint32_t hz, uint32_t *actual_hz);
uint32_t sx1255_get_frequency(enum sx1255_dir dir);

/** Synthesizer word; the exact frequency is frf * f_xtal / 2^20. */
uint32_t sx1255_get_frf(enum sx1255_dir dir);

/** Read the PLL lock flags (SX1255_STAT_PLL_LOCK_MASK). */
int sx1255_get_pll_lock(uint8_t *lock_flags);

/** Set the sample rate; must be one of sx1255_sample_rate(). */
int sx1255_set_sample_rate(uint32_t rate_hz);
uint32_t sx1255_get_sample_rate(void);

/** Sample rate at index 0..SX1255_NUM_SAMPLE_RATES-1, highest first. */
uint32_t sx1255_sample_rate(int index);

/** I2S bit clock for a given sample rate (64 bit per sample). */
uint32_t sx1255_bit_clock(uint32_t rate_hz);

const struct sx1255_gain_range *sx1255_gain_range(enum sx1255_gain gain);
const char *sx1255_gain_name(enum sx1255_gain gain);

/**
 * Set a gain element. The value is clamped and quantized to the range.
 *
 * @param actual_db Optional, receives the gain actually set.
 */
int sx1255_set_gain(enum sx1255_gain gain, int db, int *actual_db);
int sx1255_get_gain(enum sx1255_gain gain);

int sx1255_set_rx_input(enum sx1255_rx_input input);
enum sx1255_rx_input sx1255_get_rx_input(void);

/** Read DIO0/DIO1/DIO3 (index 0, 1, 3). Negative if not available. */
int sx1255_get_dio(int index);

#endif /* PHENIX_SX1255_H_ */
