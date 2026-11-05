/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Phenix-1255 contributors
 *
 * Gain source per direction: the host sets the gains (SET_GAIN), or they
 * are set manually on the device (front panel, console) and host requests
 * are answered without a change.
 */

#ifndef PHENIX_GAIN_CTRL_H_
#define PHENIX_GAIN_CTRL_H_

#include <stdbool.h>

#include "radio/sx1255.h"

/** Load the gain settings and apply the manual gains. After sx1255_init(). */
void gain_ctrl_init(void);

/** Apply the manual gains again, e.g. after a chip reset. */
void gain_ctrl_apply(void);

enum sx1255_dir gain_ctrl_dir(enum sx1255_gain gain);

bool gain_ctrl_is_manual(enum sx1255_dir dir);

/**
 * Select the gain source of a direction. Switching to manual takes over
 * the gains currently set as manual values.
 */
int gain_ctrl_set_manual(enum sx1255_dir dir, bool manual);

/**
 * Set a manual gain; -EPERM while the direction is in host mode.
 *
 * @param actual_db Optional, receives the gain actually set.
 */
int gain_ctrl_set(enum sx1255_gain gain, int db, int *actual_db);

/**
 * Gain request of the host. In manual mode nothing is changed and the
 * manual gain is reported as the gain actually set.
 */
int gain_ctrl_host_set(enum sx1255_gain gain, int db, int *actual_db);

/** A host session starts: ignored requests are logged once per session. */
void gain_ctrl_session_start(void);

/** Save pending changes now, regardless of a running stream (before a reboot). */
void gain_ctrl_flush(void);

#endif /* PHENIX_GAIN_CTRL_H_ */
