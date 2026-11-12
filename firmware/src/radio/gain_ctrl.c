/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Phenix-1255 contributors
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include "config/config_store.h"
#include "radio/gain_ctrl.h"
#include "stream/stream.h"

LOG_MODULE_REGISTER(gain_ctrl, LOG_LEVEL_INF);

BUILD_ASSERT(PHENIX_GAIN_COUNT == SX1255_GAIN_COUNT, "gain element count");

/* Collects a burst of changes (held button) into one flash write */
#define SAVE_DELAY K_SECONDS(2)
/* Retry interval while a stream runs: flash writes lock interrupts */
#define SAVE_RETRY K_SECONDS(1)

static struct phenix_gain_cfg cfg;
static bool dirty;
static K_MUTEX_DEFINE(lock);

/* Bit per direction: an ignored host request was logged in this session */
static atomic_t logged;

static int save(void)
{
	struct phenix_gain_cfg copy;
	bool pending;
	int ret = 0;

	k_mutex_lock(&lock, K_FOREVER);
	pending = dirty;
	dirty = false;
	copy = cfg;
	k_mutex_unlock(&lock);

	if (pending) {
		ret = config_store_set_gain(&copy);
		if (ret < 0) {
			k_mutex_lock(&lock, K_FOREVER);
			dirty = true;
			k_mutex_unlock(&lock);
		}
	}

	return ret;
}

static void save_work_handler(struct k_work *work);

static K_WORK_DELAYABLE_DEFINE(save_work, save_work_handler);

static void save_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	/* config_store refuses while a stream runs; also covers a start meanwhile */
	if (stream_is_running() || save() == -EBUSY) {
		k_work_reschedule(&save_work, SAVE_RETRY);
	}
}

/* Caller holds the lock */
static void mark_dirty(void)
{
	dirty = true;
	k_work_reschedule(&save_work, SAVE_DELAY);
}

static bool manual_locked(enum sx1255_dir dir)
{
	return (dir == SX1255_RX) ? cfg.rx_manual : cfg.tx_manual;
}

enum sx1255_dir gain_ctrl_dir(enum sx1255_gain gain)
{
	return (gain == SX1255_GAIN_LNA || gain == SX1255_GAIN_PGA) ? SX1255_RX : SX1255_TX;
}

void gain_ctrl_apply(void)
{
	k_mutex_lock(&lock, K_FOREVER);
	for (int i = 0; i < SX1255_GAIN_COUNT; i++) {
		if (manual_locked(gain_ctrl_dir((enum sx1255_gain)i))) {
			(void)sx1255_set_gain((enum sx1255_gain)i, cfg.db[i], NULL);
		}
	}
	k_mutex_unlock(&lock);
}

void gain_ctrl_init(void)
{
	config_store_get_gain(&cfg);
	gain_ctrl_apply();

	LOG_INF("Gain source RX %s, TX %s", cfg.rx_manual ? "manual" : "host",
		cfg.tx_manual ? "manual" : "host");
}

bool gain_ctrl_is_manual(enum sx1255_dir dir)
{
	bool manual;

	k_mutex_lock(&lock, K_FOREVER);
	manual = manual_locked(dir);
	k_mutex_unlock(&lock);

	return manual;
}

int gain_ctrl_set_manual(enum sx1255_dir dir, bool manual)
{
	k_mutex_lock(&lock, K_FOREVER);

	if (manual_locked(dir) != manual) {
		if (manual) {
			/* Continue from the gains in use, e.g. those of the host */
			for (int i = 0; i < SX1255_GAIN_COUNT; i++) {
				int db = sx1255_get_gain((enum sx1255_gain)i);

				if (gain_ctrl_dir((enum sx1255_gain)i) == dir && db >= 0) {
					cfg.db[i] = (uint8_t)db;
				}
			}
		}
		if (dir == SX1255_RX) {
			cfg.rx_manual = manual;
		} else {
			cfg.tx_manual = manual;
		}
		mark_dirty();
		LOG_INF("%s gain source: %s", (dir == SX1255_RX) ? "RX" : "TX",
			manual ? "manual" : "host");
	}

	k_mutex_unlock(&lock);

	return 0;
}

int gain_ctrl_set(enum sx1255_gain gain, int db, int *actual_db)
{
	int actual;
	int ret;

	if (gain >= SX1255_GAIN_COUNT) {
		return -EINVAL;
	}

	k_mutex_lock(&lock, K_FOREVER);

	if (!manual_locked(gain_ctrl_dir(gain))) {
		k_mutex_unlock(&lock);
		return -EPERM;
	}

	ret = sx1255_set_gain(gain, db, &actual);
	if (ret == 0) {
		cfg.db[gain] = (uint8_t)actual;
		mark_dirty();
		if (actual_db != NULL) {
			*actual_db = actual;
		}
	}

	k_mutex_unlock(&lock);

	return ret;
}

int gain_ctrl_host_set(enum sx1255_gain gain, int db, int *actual_db)
{
	enum sx1255_dir dir = gain_ctrl_dir(gain);

	if (gain >= SX1255_GAIN_COUNT) {
		return -EINVAL;
	}

	if (!gain_ctrl_is_manual(dir)) {
		return sx1255_set_gain(gain, db, actual_db);
	}

	/* FlowStation and others abort on errors: report the manual gain instead */
	if (!atomic_test_and_set_bit(&logged, dir)) {
		LOG_INF("%s gain is manual, host gain requests are ignored",
			(dir == SX1255_RX) ? "RX" : "TX");
	}
	if (actual_db != NULL) {
		*actual_db = sx1255_get_gain(gain);
	}

	return 0;
}

void gain_ctrl_session_start(void)
{
	atomic_clear(&logged);
}

void gain_ctrl_flush(void)
{
	(void)k_work_cancel_delayable(&save_work);
	(void)save();
}
