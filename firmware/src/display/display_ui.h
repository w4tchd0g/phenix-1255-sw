/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Phenix-1255 contributors
 *
 * Front panel: status pages and settings menu on the SSD1306 OLED,
 * operated with the rotary encoder (turn, push) and SW3 (step forward).
 */

#ifndef PHENIX_DISPLAY_UI_H_
#define PHENIX_DISPLAY_UI_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Operator inputs, as produced by the encoder and the buttons. */
enum display_ui_key {
	DISPLAY_KEY_PREV,   /**< Encoder counter-clockwise */
	DISPLAY_KEY_NEXT,   /**< Encoder clockwise, SW3 */
	DISPLAY_KEY_SELECT, /**< Short push */
	DISPLAY_KEY_BACK,   /**< Long push */
};

struct display_ui_info {
	bool ready;
	bool dimmed;
	const char *screen;
	int page;
};

/** Start the display thread. Without a display the firmware runs as before. */
int display_ui_start(void);

/**
 * Show the unsupported hardware error as a static image, without starting
 * the display thread. Does nothing without a display.
 */
void display_ui_show_hw_error(void);

/** Inject an operator input (console test without the encoder). */
void display_ui_inject(enum display_ui_key key);

/** Show a pattern marking the yellow and blue zones for the given time. */
void display_ui_test_pattern(int seconds);

void display_ui_get_info(struct display_ui_info *info);

/**
 * Copy the frame last sent to the display (FB_SIZE bytes, SSD1306 page
 * layout). Returns false if no frame has been sent.
 */
bool display_ui_snapshot(uint8_t *buf, size_t len);

#endif /* PHENIX_DISPLAY_UI_H_ */
