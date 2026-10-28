/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Phenix-1255 contributors
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/logging/log.h>

#include "radio/sx1255.h"
#include "radio/sx1255_regs.h"

LOG_MODULE_REGISTER(sx1255, LOG_LEVEL_INF);

#define SX1255_NODE DT_NODELABEL(sx1255)

#define SX1255_SPI_OP (SPI_OP_MODE_CONTROLLER | SPI_WORD_SET(8) | SPI_TRANSFER_MSB)

/* Reset timing: pulse >= 100 us, then wait for the chip to start */
#define RESET_PULSE_US 1000
#define RESET_WAIT_MS  5

/* Default configuration after reset */
#define DEFAULT_FREQ_HZ     433920000U
#define DEFAULT_SAMPLE_RATE 600000U

/* Bits per sample on the I2S interface: 2 slots of 32 bit */
#define I2S_BITS_PER_SAMPLE 64U

static const struct spi_dt_spec spi = SPI_DT_SPEC_GET(SX1255_NODE, SX1255_SPI_OP);
static const struct gpio_dt_spec reset_gpio = GPIO_DT_SPEC_GET(SX1255_NODE, reset_gpios);
static const struct gpio_dt_spec dio_gpio[4] = {
	[0] = GPIO_DT_SPEC_GET_OR(SX1255_NODE, dio0_gpios, {0}),
	[1] = GPIO_DT_SPEC_GET_OR(SX1255_NODE, dio1_gpios, {0}),
	[3] = GPIO_DT_SPEC_GET_OR(SX1255_NODE, dio3_gpios, {0}),
};

static const uint32_t xtal_hz = DT_PROP(SX1255_NODE, clock_frequency);

/*
 * Initial register values (0x00..0x13), taken from SoapySX. Frequency and
 * sample rate registers are written afterwards by the regular setters.
 */
static const uint8_t init_regs[SX1255_NUM_REGS] = {
	/* 0x00: reference oscillator on, RX/TX off */
	SX1255_MODE_REF_EN,
	/* 0x01..0x06: RX and TX frequency, set later */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	/* 0x07: version, read-only */
	SX1255_VERSION,
	/* 0x08: TX gains, datasheet default */
	0x2E,
	/* 0x09..0x0B: datasheet defaults */
	0x24, 0x30, 0x02,
	/* 0x0C: RX gains, datasheet default */
	0x3F,
	/* 0x0D: narrow RX filters, ADC trim 7 minimizes ADC spurs */
	0x3B,
	/* 0x0E: datasheet default */
	0x06,
	/* 0x0F: IO_MAP, datasheet default */
	0x00,
	/* 0x10: CK_SEL, datasheet default (CK_OUT enabled, no loopback) */
	0x02,
	/* 0x11: status, read-only */
	0x00,
	/* 0x12..0x13: I2S interface and decimation, sample rate set later */
	0x22, 0x2C,
};

/* Register settings per sample rate, from SoapySX */
struct rate_cfg {
	/* Ratio f_xtal / sample rate */
	uint16_t div;
	/* IISM clock divider, CK_OUT = f_xtal / 2^clkout */
	uint8_t clkout;
	/* Decimation: int_dec_mantisse, int_dec_m_parameter, int_dec_n_parameter */
	uint8_t mant;
	uint8_t m;
	uint8_t n;
};

static const struct rate_cfg rate_table[SX1255_NUM_SAMPLE_RATES] = {
	{64, 0, 0, 0, 3},   /* 600 kHz at 38.4 MHz */
	{128, 1, 0, 0, 4},  /* 300 kHz */
	{256, 2, 0, 0, 5},  /* 150 kHz */
	{512, 3, 0, 0, 6},  /* 75 kHz */
	{768, 4, 0, 1, 5},  /* 50 kHz */
	{1536, 6, 0, 1, 6}, /* 25 kHz */
};

static const struct sx1255_gain_range gain_ranges[SX1255_GAIN_COUNT] = {
	[SX1255_GAIN_LNA] = {0, 48, 6},
	[SX1255_GAIN_PGA] = {0, 30, 2},
	[SX1255_GAIN_DAC] = {0, 9, 3},
	[SX1255_GAIN_MIXER] = {0, 30, 2},
};

static const char *const gain_names[SX1255_GAIN_COUNT] = {"LNA", "PGA", "DAC", "MIXER"};

/*
 * LNA gain codes are not linear in dB. Code 1 is maximum gain (48 dB in the
 * range above), codes 2..6 are -6, -12, -24, -36 and -48 dB relative to it.
 * Index: gain step (dB / 6), value: register code.
 */
static const uint8_t lna_step_to_code[9] = {6, 6, 5, 5, 4, 4, 3, 2, 1};
/* Index: register code, value: gain step */
static const uint8_t lna_code_to_step[8] = {0, 8, 7, 6, 4, 2, 0, 0};

static K_MUTEX_DEFINE(lock);
static uint8_t regs[SX1255_NUM_REGS];
static uint32_t sample_rate;
static bool ready;

static int spi_write_regs_raw(uint8_t addr, const uint8_t *data, size_t len)
{
	uint8_t cmd = (addr & SX1255_ADDR_MASK) | SX1255_SPI_WRITE;
	const struct spi_buf tx_bufs[2] = {
		{.buf = &cmd, .len = 1},
		{.buf = (void *)data, .len = len},
	};
	const struct spi_buf_set tx = {.buffers = tx_bufs, .count = 2};

	return spi_write_dt(&spi, &tx);
}

static int spi_read_regs_raw(uint8_t addr, uint8_t *data, size_t len)
{
	uint8_t cmd = addr & SX1255_ADDR_MASK;
	const struct spi_buf tx_bufs[1] = {
		{.buf = &cmd, .len = 1},
	};
	const struct spi_buf rx_bufs[2] = {
		{.buf = NULL, .len = 1},
		{.buf = data, .len = len},
	};
	const struct spi_buf_set tx = {.buffers = tx_bufs, .count = 1};
	const struct spi_buf_set rx = {.buffers = rx_bufs, .count = 2};

	return spi_transceive_dt(&spi, &tx, &rx);
}

/* Write a range of the register cache to the chip */
static int flush_regs(uint8_t addr, size_t len)
{
	return spi_write_regs_raw(addr, &regs[addr], len);
}

static void set_field(uint8_t addr, uint8_t mask, uint8_t shift, uint8_t value)
{
	regs[addr] = (regs[addr] & ~mask) | ((value << shift) & mask);
}

static uint8_t get_field(uint8_t addr, uint8_t mask, uint8_t shift)
{
	return (regs[addr] & mask) >> shift;
}

static int chip_reset(void)
{
	int ret;

	ret = gpio_pin_configure_dt(&reset_gpio, GPIO_OUTPUT_ACTIVE);
	if (ret < 0) {
		return ret;
	}
	k_busy_wait(RESET_PULSE_US);

	/* Release: RESET is high impedance during normal operation */
	ret = gpio_pin_configure_dt(&reset_gpio, GPIO_INPUT);
	if (ret < 0) {
		return ret;
	}
	k_msleep(RESET_WAIT_MS);

	return 0;
}

static const struct rate_cfg *find_rate(uint32_t rate_hz)
{
	if (rate_hz == 0U || (xtal_hz % rate_hz) != 0U) {
		return NULL;
	}

	for (int i = 0; i < SX1255_NUM_SAMPLE_RATES; i++) {
		if (rate_table[i].div == xtal_hz / rate_hz) {
			return &rate_table[i];
		}
	}

	return NULL;
}

static int apply_sample_rate(uint32_t rate_hz)
{
	const struct rate_cfg *cfg = find_rate(rate_hz);
	uint8_t mode = regs[SX1255_REG_MODE];
	int ret;

	if (cfg == NULL) {
		return -EINVAL;
	}

	/*
	 * Disable RX and TX while changing the rate. Changing it while running
	 * was observed to corrupt the I2S frame order (SoapySX).
	 */
	regs[SX1255_REG_MODE] = mode & ~(SX1255_MODE_RX_EN | SX1255_MODE_TX_EN);
	ret = flush_regs(SX1255_REG_MODE, 1);
	if (ret < 0) {
		return ret;
	}

	set_field(SX1255_REG_IISM, SX1255_IISM_CLK_DIV_MASK, SX1255_IISM_CLK_DIV_SHIFT,
		  cfg->clkout);
	set_field(SX1255_REG_DIG_BRIDGE, SX1255_DIG_BRIDGE_MANT_MASK,
		  SX1255_DIG_BRIDGE_MANT_SHIFT, cfg->mant);
	set_field(SX1255_REG_DIG_BRIDGE, SX1255_DIG_BRIDGE_M_MASK, SX1255_DIG_BRIDGE_M_SHIFT,
		  cfg->m);
	set_field(SX1255_REG_DIG_BRIDGE, SX1255_DIG_BRIDGE_N_MASK, SX1255_DIG_BRIDGE_N_SHIFT,
		  cfg->n);
	ret = flush_regs(SX1255_REG_IISM, 2);
	if (ret < 0) {
		return ret;
	}

	regs[SX1255_REG_MODE] = mode;
	ret = flush_regs(SX1255_REG_MODE, 1);
	if (ret < 0) {
		return ret;
	}

	sample_rate = rate_hz;

	return 0;
}

static int apply_frequency(enum sx1255_dir dir, uint32_t hz, uint32_t *actual_hz)
{
	uint8_t base = (dir == SX1255_RX) ? SX1255_REG_FRF_RX_MSB : SX1255_REG_FRF_TX_MSB;
	uint64_t frf;

	if (hz < SX1255_FREQ_MIN_HZ || hz > SX1255_FREQ_MAX_HZ) {
		return -ERANGE;
	}

	frf = (((uint64_t)hz << SX1255_FRF_SHIFT) + xtal_hz / 2U) / xtal_hz;
	if (frf > SX1255_FRF_MAX) {
		return -ERANGE;
	}

	regs[base] = (uint8_t)(frf >> 16);
	regs[base + 1] = (uint8_t)(frf >> 8);
	regs[base + 2] = (uint8_t)frf;

	if (actual_hz != NULL) {
		*actual_hz = (uint32_t)((frf * xtal_hz + BIT(SX1255_FRF_SHIFT - 1)) >>
					SX1255_FRF_SHIFT);
	}

	return flush_regs(base, 3);
}

int sx1255_init(void)
{
	uint8_t version = 0;
	int ret;

	k_mutex_lock(&lock, K_FOREVER);
	ready = false;

	if (!spi_is_ready_dt(&spi) || !gpio_is_ready_dt(&reset_gpio)) {
		LOG_ERR("SPI bus or RESET GPIO not ready");
		ret = -ENODEV;
		goto out;
	}

	for (int i = 0; i < ARRAY_SIZE(dio_gpio); i++) {
		if (dio_gpio[i].port != NULL) {
			gpio_pin_configure_dt(&dio_gpio[i], GPIO_INPUT);
		}
	}

	ret = chip_reset();
	if (ret < 0) {
		LOG_ERR("Reset failed (%d)", ret);
		goto out;
	}

	ret = spi_read_regs_raw(SX1255_REG_VERSION, &version, 1);
	if (ret < 0) {
		LOG_ERR("SPI read failed (%d)", ret);
		goto out;
	}
	if (version != SX1255_VERSION) {
		LOG_ERR("Unexpected version 0x%02x (expected 0x%02x), check wiring", version,
			SX1255_VERSION);
		ret = -ENODEV;
		goto out;
	}

	memcpy(regs, init_regs, sizeof(regs));
	ret = flush_regs(0, SX1255_NUM_REGS);
	if (ret == 0) {
		ret = apply_frequency(SX1255_RX, DEFAULT_FREQ_HZ, NULL);
	}
	if (ret == 0) {
		ret = apply_frequency(SX1255_TX, DEFAULT_FREQ_HZ, NULL);
	}
	if (ret == 0) {
		ret = apply_sample_rate(DEFAULT_SAMPLE_RATE);
	}
	if (ret < 0) {
		LOG_ERR("Initial configuration failed (%d)", ret);
		goto out;
	}

	ready = true;
	LOG_INF("SX1255 version 0x%02x, reference %u Hz, %u S/s", version, xtal_hz,
		sample_rate);

out:
	k_mutex_unlock(&lock);
	return ret;
}

bool sx1255_is_ready(void)
{
	return ready;
}

uint32_t sx1255_xtal_hz(void)
{
	return xtal_hz;
}

int sx1255_read_regs(uint8_t addr, uint8_t *buf, size_t len)
{
	int ret;

	if (addr > SX1255_ADDR_MASK || len == 0U || len > (SX1255_ADDR_MASK + 1U - addr)) {
		return -EINVAL;
	}

	k_mutex_lock(&lock, K_FOREVER);
	ret = spi_read_regs_raw(addr, buf, len);
	k_mutex_unlock(&lock);

	return ret;
}

int sx1255_write_regs(uint8_t addr, const uint8_t *buf, size_t len)
{
	int ret;

	if (addr > SX1255_ADDR_MASK || len == 0U || len > (SX1255_ADDR_MASK + 1U - addr)) {
		return -EINVAL;
	}

	k_mutex_lock(&lock, K_FOREVER);
	ret = spi_write_regs_raw(addr, buf, len);
	if (ret == 0) {
		/* Keep the cache coherent for the documented registers */
		for (size_t i = 0; i < len; i++) {
			if (addr + i < SX1255_NUM_REGS) {
				regs[addr + i] = buf[i];
			}
		}
	}
	k_mutex_unlock(&lock);

	return ret;
}

int sx1255_set_mode(bool rx, bool tx, bool tx_driver)
{
	int ret;

	k_mutex_lock(&lock, K_FOREVER);
	regs[SX1255_REG_MODE] = SX1255_MODE_REF_EN | (rx ? SX1255_MODE_RX_EN : 0U) |
				(tx ? SX1255_MODE_TX_EN : 0U) |
				(tx_driver ? SX1255_MODE_DRIVER_EN : 0U);
	ret = flush_regs(SX1255_REG_MODE, 1);
	k_mutex_unlock(&lock);

	return ret;
}

void sx1255_get_mode(bool *rx, bool *tx, bool *tx_driver)
{
	uint8_t mode = regs[SX1255_REG_MODE];

	*rx = (mode & SX1255_MODE_RX_EN) != 0U;
	*tx = (mode & SX1255_MODE_TX_EN) != 0U;
	*tx_driver = (mode & SX1255_MODE_DRIVER_EN) != 0U;
}

int sx1255_set_frequency(enum sx1255_dir dir, uint32_t hz, uint32_t *actual_hz)
{
	int ret;

	k_mutex_lock(&lock, K_FOREVER);
	ret = apply_frequency(dir, hz, actual_hz);
	k_mutex_unlock(&lock);

	return ret;
}

uint32_t sx1255_get_frf(enum sx1255_dir dir)
{
	uint8_t base = (dir == SX1255_RX) ? SX1255_REG_FRF_RX_MSB : SX1255_REG_FRF_TX_MSB;
	uint32_t frf;

	k_mutex_lock(&lock, K_FOREVER);
	frf = ((uint32_t)regs[base] << 16) | ((uint32_t)regs[base + 1] << 8) | regs[base + 2];
	k_mutex_unlock(&lock);

	return frf;
}

uint32_t sx1255_get_frequency(enum sx1255_dir dir)
{
	uint64_t frf = sx1255_get_frf(dir);

	return (uint32_t)((frf * xtal_hz + BIT(SX1255_FRF_SHIFT - 1)) >> SX1255_FRF_SHIFT);
}

int sx1255_get_pll_lock(uint8_t *lock_flags)
{
	uint8_t stat;
	int ret = sx1255_read_regs(SX1255_REG_STAT, &stat, 1);

	if (ret == 0) {
		*lock_flags = stat & SX1255_STAT_PLL_LOCK_MASK;
	}

	return ret;
}

int sx1255_set_sample_rate(uint32_t rate_hz)
{
	int ret;

	k_mutex_lock(&lock, K_FOREVER);
	ret = apply_sample_rate(rate_hz);
	k_mutex_unlock(&lock);

	return ret;
}

uint32_t sx1255_get_sample_rate(void)
{
	return sample_rate;
}

uint32_t sx1255_sample_rate(int index)
{
	if (index < 0 || index >= SX1255_NUM_SAMPLE_RATES) {
		return 0;
	}

	return xtal_hz / rate_table[index].div;
}

uint32_t sx1255_bit_clock(uint32_t rate_hz)
{
	return rate_hz * I2S_BITS_PER_SAMPLE;
}

const struct sx1255_gain_range *sx1255_gain_range(enum sx1255_gain gain)
{
	return (gain < SX1255_GAIN_COUNT) ? &gain_ranges[gain] : NULL;
}

const char *sx1255_gain_name(enum sx1255_gain gain)
{
	return (gain < SX1255_GAIN_COUNT) ? gain_names[gain] : "?";
}

int sx1255_set_gain(enum sx1255_gain gain, int db, int *actual_db)
{
	const struct sx1255_gain_range *r = sx1255_gain_range(gain);
	uint8_t step;
	uint8_t reg;
	int ret;

	if (r == NULL) {
		return -EINVAL;
	}

	db = CLAMP(db, r->min_db, r->max_db);
	step = (uint8_t)((db - r->min_db + r->step_db / 2) / r->step_db);

	k_mutex_lock(&lock, K_FOREVER);

	switch (gain) {
	case SX1255_GAIN_LNA:
		reg = SX1255_REG_RXFE1;
		set_field(reg, SX1255_RXFE1_LNA_GAIN_MASK, SX1255_RXFE1_LNA_GAIN_SHIFT,
			  lna_step_to_code[step]);
		break;
	case SX1255_GAIN_PGA:
		reg = SX1255_REG_RXFE1;
		set_field(reg, SX1255_RXFE1_PGA_GAIN_MASK, SX1255_RXFE1_PGA_GAIN_SHIFT, step);
		break;
	case SX1255_GAIN_DAC:
		reg = SX1255_REG_TXFE1;
		set_field(reg, SX1255_TXFE1_DAC_GAIN_MASK, SX1255_TXFE1_DAC_GAIN_SHIFT, step);
		break;
	case SX1255_GAIN_MIXER:
	default:
		reg = SX1255_REG_TXFE1;
		set_field(reg, SX1255_TXFE1_MIX_GAIN_MASK, SX1255_TXFE1_MIX_GAIN_SHIFT, step);
		break;
	}

	ret = flush_regs(reg, 1);
	k_mutex_unlock(&lock);

	if (ret == 0 && actual_db != NULL) {
		*actual_db = sx1255_get_gain(gain);
	}

	return ret;
}

int sx1255_get_gain(enum sx1255_gain gain)
{
	const struct sx1255_gain_range *r = sx1255_gain_range(gain);
	uint8_t step;

	if (r == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&lock, K_FOREVER);
	switch (gain) {
	case SX1255_GAIN_LNA:
		step = lna_code_to_step[get_field(SX1255_REG_RXFE1, SX1255_RXFE1_LNA_GAIN_MASK,
						  SX1255_RXFE1_LNA_GAIN_SHIFT)];
		break;
	case SX1255_GAIN_PGA:
		step = get_field(SX1255_REG_RXFE1, SX1255_RXFE1_PGA_GAIN_MASK,
				 SX1255_RXFE1_PGA_GAIN_SHIFT);
		break;
	case SX1255_GAIN_DAC:
		step = get_field(SX1255_REG_TXFE1, SX1255_TXFE1_DAC_GAIN_MASK,
				 SX1255_TXFE1_DAC_GAIN_SHIFT);
		break;
	case SX1255_GAIN_MIXER:
	default:
		step = get_field(SX1255_REG_TXFE1, SX1255_TXFE1_MIX_GAIN_MASK,
				 SX1255_TXFE1_MIX_GAIN_SHIFT);
		break;
	}
	k_mutex_unlock(&lock);

	return MIN(r->min_db + step * r->step_db, r->max_db);
}

int sx1255_set_rx_input(enum sx1255_rx_input input)
{
	int ret;

	k_mutex_lock(&lock, K_FOREVER);
	set_field(SX1255_REG_CK_SEL, SX1255_CK_SEL_LOOPBACK_MASK, SX1255_CK_SEL_LOOPBACK_SHIFT,
		  (input == SX1255_RX_INPUT_LOOPBACK) ? SX1255_LOOPBACK_RF : SX1255_LOOPBACK_OFF);
	ret = flush_regs(SX1255_REG_CK_SEL, 1);
	k_mutex_unlock(&lock);

	return ret;
}

enum sx1255_rx_input sx1255_get_rx_input(void)
{
	uint8_t lb = get_field(SX1255_REG_CK_SEL, SX1255_CK_SEL_LOOPBACK_MASK,
			       SX1255_CK_SEL_LOOPBACK_SHIFT);

	return (lb == SX1255_LOOPBACK_RF) ? SX1255_RX_INPUT_LOOPBACK : SX1255_RX_INPUT_ANTENNA;
}

int sx1255_get_dio(int index)
{
	if (index < 0 || index >= (int)ARRAY_SIZE(dio_gpio) || dio_gpio[index].port == NULL) {
		return -ENODEV;
	}

	return gpio_pin_get_dt(&dio_gpio[index]);
}
