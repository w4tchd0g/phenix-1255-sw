/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Phenix-1255 contributors
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/logging/log.h>

#include "board/watchdog.h"

LOG_MODULE_REGISTER(watchdog, LOG_LEVEL_INF);

#define FEEDER_STACK_SIZE 768
/*
 * Below all streaming and network threads, above the shell: the watchdog
 * fires if the application threads keep the CPU busy without pause.
 */
#define FEEDER_PRIORITY K_PRIO_PREEMPT(10)

static const struct device *const wdt = DEVICE_DT_GET(DT_NODELABEL(wwdt0));

static int wdt_channel = -1;
static volatile bool feeding = true;
static uint32_t reset_cause;

static K_SEM_DEFINE(started, 0, 1);

static void feeder(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	k_sem_take(&started, K_FOREVER);

	while (true) {
		if (feeding) {
			wdt_feed(wdt, wdt_channel);
		}
		k_msleep(WATCHDOG_FEED_MS);
	}
}

K_THREAD_DEFINE(wdt_feeder_tid, FEEDER_STACK_SIZE, feeder, NULL, NULL, NULL, FEEDER_PRIORITY,
		0, 0);

int watchdog_init(void)
{
	struct wdt_timeout_cfg cfg = {
		.window = {.min = 0U, .max = WATCHDOG_TIMEOUT_MS},
		.flags = WDT_FLAG_RESET_SOC,
	};
	int ret;

	if (hwinfo_get_reset_cause(&reset_cause) == 0) {
		hwinfo_clear_reset_cause();
	}
	if (watchdog_caused_reset()) {
		LOG_ERR("Previous reset was caused by the watchdog");
	} else {
		LOG_INF("Reset cause: %s", watchdog_reset_cause());
	}

	if (!device_is_ready(wdt)) {
		LOG_ERR("Watchdog device not ready");
		return -ENODEV;
	}

	wdt_channel = wdt_install_timeout(wdt, &cfg);
	if (wdt_channel < 0) {
		LOG_ERR("Watchdog timeout install failed (%d)", wdt_channel);
		return wdt_channel;
	}

	ret = wdt_setup(wdt, WDT_OPT_PAUSE_HALTED_BY_DBG);
	if (ret < 0) {
		LOG_ERR("Watchdog setup failed (%d)", ret);
		return ret;
	}

	k_sem_give(&started);
	LOG_INF("Watchdog active, timeout %d ms", WATCHDOG_TIMEOUT_MS);

	return 0;
}

bool watchdog_caused_reset(void)
{
	return (reset_cause & RESET_WATCHDOG) != 0U;
}

const char *watchdog_reset_cause(void)
{
	if (reset_cause & RESET_WATCHDOG) {
		return "watchdog";
	}
	if (reset_cause & RESET_SOFTWARE) {
		return "software";
	}
	if (reset_cause & RESET_POR) {
		return "power-on";
	}
	if (reset_cause & RESET_PIN) {
		return "reset pin";
	}
	if (reset_cause & RESET_DEBUG) {
		return "debugger";
	}
	if (reset_cause & RESET_BROWNOUT) {
		return "brown-out";
	}

	return (reset_cause != 0U) ? "other" : "unknown";
}

void watchdog_stop_feeding(void)
{
	feeding = false;
}
