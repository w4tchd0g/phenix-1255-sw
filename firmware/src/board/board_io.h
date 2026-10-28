/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Phenix-1255 contributors
 *
 * Board-level I/O: status LEDs, PA enable, COMM_SEL switch and the
 * factory reset button.
 */

#ifndef PHENIX_BOARD_IO_H_
#define PHENIX_BOARD_IO_H_

#include <stdbool.h>

/** Host link selected by the COMM_SEL switch at boot. */
enum board_comm_mode {
	BOARD_COMM_ETHERNET,
	BOARD_COMM_USB,
};

enum board_led {
	BOARD_LED_ERR,  /**< LED0, red */
	BOARD_LED_WARN, /**< LED1, yellow */
	BOARD_LED_OK,   /**< LED2, green */
	BOARD_LED_COUNT,
};

enum board_led_mode {
	BOARD_LED_OFF,
	BOARD_LED_ON,
	BOARD_LED_BLINK_SLOW, /**< 500 ms on, 500 ms off */
	BOARD_LED_BLINK_FAST, /**< 100 ms on, 100 ms off */
};

/**
 * Configure all board I/O and sample the boot-time inputs.
 *
 * PA_EN is already forced inactive by an early init hook before the kernel
 * starts; this function takes it over through the GPIO driver.
 */
int board_io_init(void);

/** COMM_SEL state sampled during board_io_init(). */
enum board_comm_mode board_io_comm_mode(void);

/**
 * Current physical level of the COMM_SEL input (1 = high, 0 = low,
 * negative on error). Diagnostic only; the active mode is fixed at boot.
 */
int board_io_comm_sel_level(void);

/** True if SW2 was held during board_io_init(). */
bool board_io_factory_reset_requested(void);

const char *board_comm_mode_str(enum board_comm_mode mode);

void board_led_set(enum board_led led, enum board_led_mode mode);

enum board_led_mode board_led_get(enum board_led led);

/** Short single pulse on top of the current LED mode. */
void board_led_flash(enum board_led led);

/**
 * Switch the PA supply (VBAT3).
 *
 * Only to be called by the TX stream start/stop logic and by the safety
 * shutdown paths.
 */
int board_pa_set(bool enable);

bool board_pa_is_enabled(void);

#endif /* PHENIX_BOARD_IO_H_ */
