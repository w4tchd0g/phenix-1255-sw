/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Phenix-1255 contributors
 *
 * SX1255 register map.
 *
 * Register usage and initial values follow SoapySX (SXceiver) and
 * SoapyMuCell (uCell), both MIT licensed.
 */

#ifndef PHENIX_SX1255_REGS_H_
#define PHENIX_SX1255_REGS_H_

#include <zephyr/sys/util.h>

/* SPI: bit 7 of the address byte selects a write access */
#define SX1255_SPI_WRITE BIT(7)
#define SX1255_ADDR_MASK 0x7FU

/* Documented configuration registers 0x00..0x13 */
#define SX1255_NUM_REGS 0x14U

#define SX1255_REG_MODE        0x00U
#define SX1255_REG_FRF_RX_MSB  0x01U
#define SX1255_REG_FRF_RX_MID  0x02U
#define SX1255_REG_FRF_RX_LSB  0x03U
#define SX1255_REG_FRF_TX_MSB  0x04U
#define SX1255_REG_FRF_TX_MID  0x05U
#define SX1255_REG_FRF_TX_LSB  0x06U
#define SX1255_REG_VERSION     0x07U
#define SX1255_REG_TXFE1       0x08U
#define SX1255_REG_TXFE2       0x09U
#define SX1255_REG_TXFE3       0x0AU
#define SX1255_REG_TXFE4       0x0BU
#define SX1255_REG_RXFE1       0x0CU
#define SX1255_REG_RXFE2       0x0DU
#define SX1255_REG_RXFE3       0x0EU
#define SX1255_REG_IO_MAP      0x0FU
#define SX1255_REG_CK_SEL      0x10U
#define SX1255_REG_STAT        0x11U
#define SX1255_REG_IISM        0x12U
#define SX1255_REG_DIG_BRIDGE  0x13U

/* Expected content of SX1255_REG_VERSION */
#define SX1255_VERSION 0x11U

/* MODE */
#define SX1255_MODE_REF_EN    BIT(0) /* Reference oscillator */
#define SX1255_MODE_RX_EN     BIT(1) /* Receiver */
#define SX1255_MODE_TX_EN     BIT(2) /* Transmitter */
#define SX1255_MODE_DRIVER_EN BIT(3) /* TX PA driver output */

/* Frequency word: f_rf = frf * f_xtal / 2^20, 24 bit */
#define SX1255_FRF_SHIFT 20
#define SX1255_FRF_MAX   0xFFFFFFU

/* TXFE1 */
#define SX1255_TXFE1_DAC_GAIN_SHIFT 4
#define SX1255_TXFE1_DAC_GAIN_MASK  (0x7U << SX1255_TXFE1_DAC_GAIN_SHIFT)
#define SX1255_TXFE1_MIX_GAIN_SHIFT 0
#define SX1255_TXFE1_MIX_GAIN_MASK  (0xFU << SX1255_TXFE1_MIX_GAIN_SHIFT)

/* RXFE1 */
#define SX1255_RXFE1_LNA_GAIN_SHIFT 5
#define SX1255_RXFE1_LNA_GAIN_MASK  (0x7U << SX1255_RXFE1_LNA_GAIN_SHIFT)
#define SX1255_RXFE1_PGA_GAIN_SHIFT 1
#define SX1255_RXFE1_PGA_GAIN_MASK  (0xFU << SX1255_RXFE1_PGA_GAIN_SHIFT)

/* CK_SEL: loopback selection, 1 = RF loopback, 3 = digital loopback */
#define SX1255_CK_SEL_LOOPBACK_SHIFT 2
#define SX1255_CK_SEL_LOOPBACK_MASK  (0x3U << SX1255_CK_SEL_LOOPBACK_SHIFT)
#define SX1255_LOOPBACK_OFF          0U
#define SX1255_LOOPBACK_RF           1U

/*
 * STAT: PLL lock flags of the RX and TX synthesizers. Bit assignment
 * verified on hardware: RX only enabled reads 0x06, TX only reads 0x05.
 * Bit 2 is set whenever the reference oscillator is running.
 */
#define SX1255_STAT_PLL_LOCK_TX   BIT(0)
#define SX1255_STAT_PLL_LOCK_RX   BIT(1)
#define SX1255_STAT_PLL_LOCK_MASK (SX1255_STAT_PLL_LOCK_RX | SX1255_STAT_PLL_LOCK_TX)

/* IISM: I2S clock output divider (CK_OUT = f_xtal / 2^clkout) */
#define SX1255_IISM_CLK_DIV_SHIFT 0
#define SX1255_IISM_CLK_DIV_MASK  (0xFU << SX1255_IISM_CLK_DIV_SHIFT)

/* DIG_BRIDGE: decimation factor */
#define SX1255_DIG_BRIDGE_MANT_SHIFT 7
#define SX1255_DIG_BRIDGE_MANT_MASK  BIT(7)
#define SX1255_DIG_BRIDGE_M_SHIFT    6
#define SX1255_DIG_BRIDGE_M_MASK     BIT(6)
#define SX1255_DIG_BRIDGE_N_SHIFT    3
#define SX1255_DIG_BRIDGE_N_MASK     (0x7U << SX1255_DIG_BRIDGE_N_SHIFT)

#endif /* PHENIX_SX1255_REGS_H_ */
