/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Phenix-1255 contributors
 *
 * Hardware watchdog (WWDT0) and reset cause reporting.
 */

#ifndef PHENIX_WATCHDOG_H_
#define PHENIX_WATCHDOG_H_

#include <stdbool.h>

/* The SoC is reset if the watchdog is not fed within this time */
#define WATCHDOG_TIMEOUT_MS 2000
#define WATCHDOG_FEED_MS    250

/** Log the reset cause and start the watchdog and its feeder thread. */
int watchdog_init(void);

/** True if the previous reset was caused by the watchdog. */
bool watchdog_caused_reset(void);

/** Reset cause of the last boot as text. */
const char *watchdog_reset_cause(void);

/** Stop feeding to provoke a watchdog reset (test). */
void watchdog_stop_feeding(void);

#endif /* PHENIX_WATCHDOG_H_ */
