/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Phenix-1255 contributors
 *
 * Hardware variant detection from the HW_ID solder jumpers.
 */

#ifndef PHENIX_HW_ID_H_
#define PHENIX_HW_ID_H_

#include <stdbool.h>
#include <stddef.h>

#include <zephyr/toolchain.h>

/** Number of HW_ID jumpers; bit n of the ID is HW_ID<n>. */
#define HW_ID_BITS 3

/**
 * Read the HW_ID jumpers and look up the hardware variant.
 *
 * @retval 0 the variant is supported by this firmware.
 * @retval -ENOTSUP unknown variant.
 * @retval <0 other error, the jumpers could not be read.
 */
int hw_id_init(void);

/** True if hw_id_init() found a supported variant. */
bool hw_id_is_supported(void);

/** Jumper code read by hw_id_init(), negative if it could not be read. */
int hw_id_get(void);

/** Name of the detected variant, "unknown" if unsupported. */
const char *hw_id_name(void);

/**
 * Format the jumper code as binary digits HW_ID2..HW_ID0 ("010").
 * Writes "?" if the jumpers could not be read.
 */
void hw_id_bits_str(char *buf, size_t len);

/** List the supported variants as "000 base[, ...]". */
void hw_id_supported_str(char *buf, size_t len);

/**
 * Stop the start-up on unsupported hardware.
 *
 * Blinks the red FRDM-MCXN947 LED and repeats the error message on the
 * console; the shell stays usable. Never returns.
 */
FUNC_NORETURN void hw_id_halt(void);

#endif /* PHENIX_HW_ID_H_ */
