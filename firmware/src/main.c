/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Phenix-1255 contributors
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/version.h>
#include <zephyr/app_version.h>

#include "board/board_io.h"
#include "board/hw_id.h"
#include "board/watchdog.h"
#include "config/config_store.h"
#include "control/control.h"
#include "display/display_ui.h"
#include "ethernet/net_config.h"
#include "ethernet/transport_eth.h"
#include "ethernet/web_ui.h"
#include "radio/sx1255.h"
#include "stream/sample_io.h"
#include "stream/stream.h"
#include "usb/transport_usb.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* LED pattern shown after a factory reset */
#define FACTORY_RESET_INDICATION K_SECONDS(2)

int main(void)
{
	struct phenix_net_cfg net_cfg;
	bool factory_reset;
	int ret;

	LOG_INF("Phenix-1255 firmware %s (Zephyr %s)", APP_VERSION_EXTENDED_STRING,
		KERNEL_VERSION_STRING);

	/*
	 * Before any other pin is driven: on unknown hardware only the early
	 * PA_EN shutdown and the error screen on the display (whose driver is
	 * already initialized) touch the board.
	 */
	if (hw_id_init() < 0) {
		display_ui_show_hw_error();
		hw_id_halt();
	}

	ret = board_io_init();
	if (ret < 0) {
		LOG_ERR("Board I/O init failed (%d)", ret);
		return ret;
	}

	ret = watchdog_init();
	if (ret < 0) {
		board_led_set(BOARD_LED_ERR, BOARD_LED_ON);
	} else if (watchdog_caused_reset()) {
		/* Signal the watchdog reset until the next successful stream start */
		board_led_set(BOARD_LED_ERR, BOARD_LED_BLINK_SLOW);
	}

	factory_reset = board_io_factory_reset_requested();

	ret = config_store_init(factory_reset);
	if (ret < 0) {
		LOG_ERR("Configuration store init failed (%d), using defaults", ret);
		board_led_set(BOARD_LED_ERR, BOARD_LED_ON);
	}

	if (factory_reset) {
		/* All LEDs blink fast to acknowledge the reset */
		board_led_set(BOARD_LED_ERR, BOARD_LED_BLINK_FAST);
		board_led_set(BOARD_LED_WARN, BOARD_LED_BLINK_FAST);
		board_led_set(BOARD_LED_OK, BOARD_LED_BLINK_FAST);
		k_sleep(FACTORY_RESET_INDICATION);
		board_led_set(BOARD_LED_ERR, BOARD_LED_OFF);
		board_led_set(BOARD_LED_WARN, BOARD_LED_OFF);
	}

	ret = sx1255_init();
	if (ret < 0) {
		LOG_ERR("SX1255 init failed (%d)", ret);
		board_led_set(BOARD_LED_ERR, BOARD_LED_ON);
	}

	ret = stream_init();
	if (ret < 0) {
		LOG_ERR("Stream init failed (%d)", ret);
		board_led_set(BOARD_LED_ERR, BOARD_LED_ON);
	}

	control_init();
	sample_io_init();

	/* Ethernet is always configured: the web interface runs in both modes */
	config_store_get_net(&net_cfg);
	ret = net_config_start(&net_cfg);
	if (ret < 0) {
		LOG_ERR("Network configuration failed (%d)", ret);
	}
	web_ui_start();
	display_ui_start();

	if (board_io_comm_mode() == BOARD_COMM_ETHERNET) {
		transport_eth_start(net_cfg.ctrl_port, net_cfg.sample_port);
	} else {
		LOG_INF("Host link: HS USB");
		ret = transport_usb_start();
		if (ret < 0) {
			board_led_set(BOARD_LED_ERR, BOARD_LED_ON);
		}
	}

	/* Ready, no host connected */
	board_led_set(BOARD_LED_OK, BOARD_LED_BLINK_SLOW);

	return 0;
}
