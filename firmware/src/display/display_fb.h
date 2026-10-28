/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Phenix-1255 contributors
 *
 * Monochrome 128x64 frame buffer in the SSD1306 memory layout (8 pages of
 * 128 column bytes, bit 0 = top pixel) with a 5x7 text font. Text is drawn
 * in page-aligned rows, so a glyph column is one frame buffer byte.
 */

#ifndef PHENIX_DISPLAY_FB_H_
#define PHENIX_DISPLAY_FB_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FB_WIDTH      128
#define FB_HEIGHT     64
#define FB_ROWS       (FB_HEIGHT / 8)
#define FB_SIZE       (FB_WIDTH * FB_ROWS)
/* Glyph cell: 5 pixel columns plus one column spacing */
#define FB_CHAR_WIDTH 6
#define FB_COLS       (FB_WIDTH / FB_CHAR_WIDTH)

void fb_clear(uint8_t *fb);

/** Set or clear the columns [x0, x1) of a text row. */
void fb_fill(uint8_t *fb, int row, int x0, int x1, bool on);

/**
 * Draw text into a row at pixel column x, clipped at the right edge.
 * Inverted text is drawn dark on a lit background.
 *
 * @return Pixel column after the last character
 */
int fb_text(uint8_t *fb, int row, int x, const char *str, bool invert);

/** Pixel width of a string. */
int fb_text_width(const char *str);

/**
 * Draw a highlighted label: inverted text with one lit column on each side.
 *
 * @return Pixel column after the label
 */
int fb_chip(uint8_t *fb, int row, int x, const char *str);

/** Width of a label drawn by fb_chip(). */
int fb_chip_width(const char *str);

#endif /* PHENIX_DISPLAY_FB_H_ */
