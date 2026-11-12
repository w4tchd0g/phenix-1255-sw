/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Phenix-1255 contributors
 */

#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "board/hw_id.h"

LOG_MODULE_REGISTER(hw_id, LOG_LEVEL_INF);

#define USER_NODE DT_PATH(zephyr_user)

/* The internal pull-downs charge the open jumper lines low within this time */
#define HW_ID_SETTLE_US       1000
#define HW_ID_SAMPLES         5
#define HW_ID_SAMPLE_INTERVAL K_MSEC(5)

/* Unsupported hardware: 5 Hz blink, error message repeated for late terminals */
#define HALT_BLINK_MS  100
#define HALT_REPEAT_MS 5000

BUILD_ASSERT(DT_PROP_LEN(USER_NODE, hw_id_gpios) == HW_ID_BITS,
	     "hw-id-gpios must list HW_ID0..HW_ID2");

static const struct gpio_dt_spec hw_id_pins[HW_ID_BITS] = {
	DT_FOREACH_PROP_ELEM_SEP(USER_NODE, hw_id_gpios, GPIO_DT_SPEC_GET_BY_IDX, (,))};

/*
 * Red LED of the FRDM-MCXN947 itself: on unknown hardware it is unclear
 * what is connected to the phenix-1255-hw LED pins.
 */
static const struct gpio_dt_spec halt_led = GPIO_DT_SPEC_GET(DT_ALIAS(led_frdm_err), gpios);

struct hw_variant {
	uint8_t id;
	const char *name;
};

/* No jumper fitted: phenix-1255-hw, µCell and compatible boards */
static const struct hw_variant variants[] = {
	{0, "base"},
};

static int hw_id = -ENODEV;
static const struct hw_variant *variant;

static void bits_str(int id, char *buf, size_t len)
{
	if (id < 0 || len < HW_ID_BITS + 1) {
		snprintk(buf, len, "?");
		return;
	}

	for (int i = 0; i < HW_ID_BITS; i++) {
		buf[i] = (id & BIT(HW_ID_BITS - 1 - i)) ? '1' : '0';
	}
	buf[HW_ID_BITS] = '\0';
}

static int read_jumpers(void)
{
	int count[HW_ID_BITS] = {0};
	int id = 0;
	int ret;

	for (int i = 0; i < HW_ID_BITS; i++) {
		if (!gpio_is_ready_dt(&hw_id_pins[i])) {
			LOG_ERR("HW_ID%d GPIO not ready", i);
			return -ENODEV;
		}
		/* The devicetree flags add the internal pull-down */
		ret = gpio_pin_configure_dt(&hw_id_pins[i], GPIO_INPUT);
		if (ret < 0) {
			LOG_ERR("HW_ID%d configuration failed (%d)", i, ret);
			return ret;
		}
	}

	k_busy_wait(HW_ID_SETTLE_US);

	/* Majority vote per jumper */
	for (int s = 0; s < HW_ID_SAMPLES; s++) {
		for (int i = 0; i < HW_ID_BITS; i++) {
			ret = gpio_pin_get_dt(&hw_id_pins[i]);
			if (ret < 0) {
				LOG_ERR("HW_ID%d read failed (%d)", i, ret);
				return ret;
			}
			count[i] += ret;
		}
		if (s + 1 < HW_ID_SAMPLES) {
			k_sleep(HW_ID_SAMPLE_INTERVAL);
		}
	}

	for (int i = 0; i < HW_ID_BITS; i++) {
		if (count[i] > HW_ID_SAMPLES / 2) {
			id |= BIT(i);
		}
	}

	return id;
}

int hw_id_init(void)
{
	char bits[HW_ID_BITS + 1];

	hw_id = read_jumpers();
	if (hw_id < 0) {
		return hw_id;
	}

	bits_str(hw_id, bits, sizeof(bits));

	for (size_t i = 0; i < ARRAY_SIZE(variants); i++) {
		if (variants[i].id == hw_id) {
			variant = &variants[i];
			LOG_INF("Hardware: %s (HW_ID2..0 = %s)", variant->name, bits);
			return 0;
		}
	}

	return -ENOTSUP;
}

bool hw_id_is_supported(void)
{
	return variant != NULL;
}

int hw_id_get(void)
{
	return hw_id;
}

const char *hw_id_name(void)
{
	return (variant != NULL) ? variant->name : "unknown";
}

void hw_id_bits_str(char *buf, size_t len)
{
	bits_str(hw_id, buf, len);
}

void hw_id_supported_str(char *buf, size_t len)
{
	char bits[HW_ID_BITS + 1];
	size_t pos = 0;

	buf[0] = '\0';
	for (size_t i = 0; i < ARRAY_SIZE(variants) && pos < len; i++) {
		bits_str(variants[i].id, bits, sizeof(bits));
		pos += snprintk(&buf[pos], len - pos, "%s%s %s", (i > 0) ? ", " : "", bits,
				variants[i].name);
	}
}

FUNC_NORETURN void hw_id_halt(void)
{
	char bits[HW_ID_BITS + 1];
	uint32_t since_msg = HALT_REPEAT_MS;
	bool led_ok = false;
	bool on = false;

	if (gpio_is_ready_dt(&halt_led)) {
		led_ok = gpio_pin_configure_dt(&halt_led, GPIO_OUTPUT_INACTIVE) == 0;
	}

	while (true) {
		if (since_msg >= HALT_REPEAT_MS) {
			since_msg = 0U;
			if (hw_id < 0) {
				LOG_ERR("HW_ID jumpers could not be read (%d)", hw_id);
			} else {
				hw_id_bits_str(bits, sizeof(bits));
				LOG_ERR("Unsupported hardware: HW_ID2..0 = %s", bits);
			}
			for (size_t i = 0; i < ARRAY_SIZE(variants); i++) {
				bits_str(variants[i].id, bits, sizeof(bits));
				LOG_ERR("Supported: HW_ID2..0 = %s (%s)", bits, variants[i].name);
			}
			LOG_ERR("Start-up halted, only the console is available");
		}

		if (led_ok) {
			on = !on;
			gpio_pin_set_dt(&halt_led, on ? 1 : 0);
		}
		k_msleep(HALT_BLINK_MS);
		since_msg += HALT_BLINK_MS;
	}
}
