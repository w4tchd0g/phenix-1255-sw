/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Phenix-1255 contributors
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include <fsl_clock.h>

#include "board/board_io.h"

LOG_MODULE_REGISTER(board_io, LOG_LEVEL_INF);

#define USER_NODE DT_PATH(zephyr_user)

/* Tick of the LED state machine */
#define LED_TICK_MS      50
#define LED_SLOW_TICKS   (500 / LED_TICK_MS)
#define LED_FAST_TICKS   (100 / LED_TICK_MS)
#define LED_FLASH_TICKS  (100 / LED_TICK_MS)

/* Boot input sampling */
#define INPUT_SETTLE_US       1000
#define COMM_SEL_SAMPLES      5
#define FACTORY_RST_SAMPLES   10
#define INPUT_SAMPLE_INTERVAL K_MSEC(5)

/*
 * PA_EN early shutdown.
 *
 * The gate pull-up on phenix-1255-hw keeps the PA off only while the pin is
 * high impedance, and other SX1255 boards may lack it. Drive the pin to the
 * inactive level in the EARLY init level, which runs before SoC clock setup
 * and before any device driver. Register access is used directly because no
 * GPIO driver is available yet.
 */
#define PA_EN_CTLR  DT_GPIO_CTLR(USER_NODE, pa_en_gpios)
#define PA_EN_PIN   DT_GPIO_PIN(USER_NODE, pa_en_gpios)
#define PA_EN_FLAGS DT_GPIO_FLAGS(USER_NODE, pa_en_gpios)

BUILD_ASSERT(DT_SAME_NODE(PA_EN_CTLR, DT_NODELABEL(gpio2)),
	     "pa_en_early_off() enables the GPIO2 clock only");

static int pa_en_early_off(void)
{
	GPIO_Type *gpio = (GPIO_Type *)DT_REG_ADDR(PA_EN_CTLR);

	CLOCK_EnableClock(kCLOCK_Gpio2);

	if (PA_EN_FLAGS & GPIO_ACTIVE_LOW) {
		gpio->PSOR = BIT(PA_EN_PIN);
	} else {
		gpio->PCOR = BIT(PA_EN_PIN);
	}
	gpio->PDDR |= BIT(PA_EN_PIN);

	return 0;
}

SYS_INIT(pa_en_early_off, EARLY, 0);

static const struct gpio_dt_spec pa_en = GPIO_DT_SPEC_GET(USER_NODE, pa_en_gpios);
static const struct gpio_dt_spec comm_sel = GPIO_DT_SPEC_GET(USER_NODE, comm_sel_gpios);
static const struct gpio_dt_spec factory_rst = GPIO_DT_SPEC_GET(USER_NODE, factory_reset_gpios);

static const struct gpio_dt_spec leds[BOARD_LED_COUNT] = {
	[BOARD_LED_ERR] = GPIO_DT_SPEC_GET(DT_ALIAS(led_err), gpios),
	[BOARD_LED_WARN] = GPIO_DT_SPEC_GET(DT_ALIAS(led_warn), gpios),
	[BOARD_LED_OK] = GPIO_DT_SPEC_GET(DT_ALIAS(led_ok), gpios),
};

static const char *const led_names[BOARD_LED_COUNT] = {"ERR", "WARN", "OK"};

static atomic_t led_mode[BOARD_LED_COUNT];
static atomic_t led_flash_ticks[BOARD_LED_COUNT];
static uint32_t led_tick;

static enum board_comm_mode comm_mode;
static bool factory_reset_requested;
static atomic_t pa_enabled;

static void led_timer_handler(struct k_timer *timer)
{
	ARG_UNUSED(timer);

	led_tick++;

	for (int i = 0; i < BOARD_LED_COUNT; i++) {
		bool on;

		switch ((enum board_led_mode)atomic_get(&led_mode[i])) {
		case BOARD_LED_ON:
			on = true;
			break;
		case BOARD_LED_BLINK_SLOW:
			on = ((led_tick / LED_SLOW_TICKS) & 1U) != 0U;
			break;
		case BOARD_LED_BLINK_FAST:
			on = ((led_tick / LED_FAST_TICKS) & 1U) != 0U;
			break;
		case BOARD_LED_OFF:
		default:
			on = false;
			break;
		}

		/* A pending flash inverts the LED for its duration */
		if (atomic_get(&led_flash_ticks[i]) > 0) {
			atomic_dec(&led_flash_ticks[i]);
			on = !on;
		}

		gpio_pin_set_dt(&leds[i], on ? 1 : 0);
	}
}

K_TIMER_DEFINE(led_timer, led_timer_handler, NULL);

static int sample_input(const struct gpio_dt_spec *spec, int samples)
{
	int active = 0;

	for (int i = 0; i < samples; i++) {
		int val = gpio_pin_get_dt(spec);

		if (val < 0) {
			return val;
		}
		active += val;
		if (i + 1 < samples) {
			k_sleep(INPUT_SAMPLE_INTERVAL);
		}
	}

	return active;
}

int board_io_init(void)
{
	int ret;

	if (!gpio_is_ready_dt(&pa_en) || !gpio_is_ready_dt(&comm_sel) ||
	    !gpio_is_ready_dt(&factory_rst)) {
		LOG_ERR("GPIO controller not ready");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&pa_en, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		LOG_ERR("PA_EN configuration failed (%d)", ret);
		return ret;
	}
	atomic_set(&pa_enabled, 0);

	for (int i = 0; i < BOARD_LED_COUNT; i++) {
		if (!gpio_is_ready_dt(&leds[i])) {
			LOG_ERR("LED %s GPIO not ready", led_names[i]);
			return -ENODEV;
		}
		ret = gpio_pin_configure_dt(&leds[i], GPIO_OUTPUT_INACTIVE);
		if (ret < 0) {
			LOG_ERR("LED %s configuration failed (%d)", led_names[i], ret);
			return ret;
		}
	}

	ret = gpio_pin_configure_dt(&comm_sel, GPIO_INPUT);
	if (ret == 0) {
		ret = gpio_pin_configure_dt(&factory_rst, GPIO_INPUT);
	}
	if (ret < 0) {
		LOG_ERR("Input configuration failed (%d)", ret);
		return ret;
	}

	/* Let the internal pull-ups charge the input lines */
	k_busy_wait(INPUT_SETTLE_US);

	/* COMM_SEL: majority vote */
	ret = sample_input(&comm_sel, COMM_SEL_SAMPLES);
	if (ret < 0) {
		return ret;
	}
	comm_mode = (ret > COMM_SEL_SAMPLES / 2) ? BOARD_COMM_USB : BOARD_COMM_ETHERNET;

	/* Factory reset: button must be held for the whole sampling window */
	ret = sample_input(&factory_rst, FACTORY_RST_SAMPLES);
	if (ret < 0) {
		return ret;
	}
	factory_reset_requested = (ret == FACTORY_RST_SAMPLES);

	k_timer_start(&led_timer, K_MSEC(LED_TICK_MS), K_MSEC(LED_TICK_MS));

	LOG_INF("COMM_SEL: %s", board_comm_mode_str(comm_mode));
	if (factory_reset_requested) {
		LOG_WRN("SW2 held during boot: factory reset requested");
	}

	return 0;
}

enum board_comm_mode board_io_comm_mode(void)
{
	return comm_mode;
}

int board_io_comm_sel_level(void)
{
	return gpio_pin_get_raw(comm_sel.port, comm_sel.pin);
}

bool board_io_factory_reset_requested(void)
{
	return factory_reset_requested;
}

const char *board_comm_mode_str(enum board_comm_mode mode)
{
	return (mode == BOARD_COMM_USB) ? "HS USB" : "Ethernet";
}

void board_led_set(enum board_led led, enum board_led_mode mode)
{
	if (led < BOARD_LED_COUNT) {
		atomic_set(&led_mode[led], mode);
	}
}

enum board_led_mode board_led_get(enum board_led led)
{
	return (led < BOARD_LED_COUNT) ? (enum board_led_mode)atomic_get(&led_mode[led])
				       : BOARD_LED_OFF;
}

void board_led_flash(enum board_led led)
{
	if (led < BOARD_LED_COUNT) {
		atomic_set(&led_flash_ticks[led], LED_FLASH_TICKS);
	}
}

int board_pa_set(bool enable)
{
	int ret = gpio_pin_set_dt(&pa_en, enable ? 1 : 0);

	if (ret < 0) {
		LOG_ERR("PA_EN write failed (%d)", ret);
		return ret;
	}

	if (atomic_set(&pa_enabled, enable ? 1 : 0) != (enable ? 1 : 0)) {
		LOG_INF("PA %s", enable ? "enabled" : "disabled");
	}

	return 0;
}

bool board_pa_is_enabled(void)
{
	return atomic_get(&pa_enabled) != 0;
}
