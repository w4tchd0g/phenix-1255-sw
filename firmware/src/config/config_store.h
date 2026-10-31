/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Phenix-1255 contributors
 *
 * Persistent device configuration stored with the Zephyr settings
 * subsystem in a dedicated internal flash partition.
 */

#ifndef PHENIX_CONFIG_STORE_H_
#define PHENIX_CONFIG_STORE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phenix1255/protocol.h>

/* Factory defaults */
#define PHENIX_DEFAULT_DHCP        true
#define PHENIX_DEFAULT_IP          {192, 168, 2, 10}
#define PHENIX_DEFAULT_NETMASK     {255, 255, 255, 0}
#define PHENIX_DEFAULT_GATEWAY     {0, 0, 0, 0}
#define PHENIX_DEFAULT_DNS         {0, 0, 0, 0}
#define PHENIX_DEFAULT_CTRL_PORT   PHX_DEFAULT_CTRL_PORT
#define PHENIX_DEFAULT_SAMPLE_PORT PHX_DEFAULT_SAMPLE_PORT

/* Front panel display */
#define PHENIX_CONTRAST_MAX                     10
#define PHENIX_DIM_TIMEOUT_MAX_S                3600
#define PHENIX_DEFAULT_DISPLAY_CONTRAST         8
#define PHENIX_DEFAULT_DISPLAY_DIM_S            300
#define PHENIX_DEFAULT_DISPLAY_AUTO_PAGES       true
#define PHENIX_DEFAULT_DISPLAY_ENCODER_REVERSED false

/* TCP port of the web interface, not configurable */
#define PHENIX_HTTP_PORT 80

/* Size of a dotted-quad string including the terminator */
#define PHENIX_IPV4_STR_LEN 16

struct phenix_net_cfg {
	bool dhcp;
	/* Static configuration, used when dhcp is false */
	uint8_t ip[4];
	uint8_t netmask[4];
	uint8_t gateway[4];
	uint8_t dns[4];
	/* TCP control channel port */
	uint16_t ctrl_port;
	/* UDP sample port */
	uint16_t sample_port;
};

struct phenix_display_cfg {
	/* Brightness level 1..PHENIX_CONTRAST_MAX */
	uint8_t contrast;
	/* Dim after this time without operator input, 0 = never */
	uint16_t dim_timeout_s;
	/* Cycle through the status pages */
	bool auto_pages;
	/* Swap the encoder turn directions, for encoders wired the other way round */
	bool encoder_reversed;
};

/**
 * Initialize the settings backend and load the stored configuration.
 *
 * @param factory_reset Erase the settings partition before loading, so
 *                      that the factory defaults are used.
 */
int config_store_init(bool factory_reset);

void config_store_defaults(struct phenix_net_cfg *cfg);

void config_store_get_net(struct phenix_net_cfg *cfg);

/** Validate and persist a new network configuration. Applied at next boot. */
int config_store_set_net(const struct phenix_net_cfg *cfg);

void config_store_get_display(struct phenix_display_cfg *cfg);

/** Validate and persist the display settings. */
int config_store_set_display(const struct phenix_display_cfg *cfg);

/**
 * Erase the settings partition. The factory defaults apply after the next
 * boot; the caller must reboot without saving anything in between.
 */
int config_store_erase(void);

/** Check a configuration for consistency. Returns a reason or NULL. */
const char *config_store_validate(const struct phenix_net_cfg *cfg);

/** Parse a dotted-quad IPv4 address. Returns 0 on success. */
int config_store_parse_ipv4(const char *str, uint8_t out[4]);

void config_store_format_ipv4(const uint8_t addr[4], char *buf, size_t len);

#endif /* PHENIX_CONFIG_STORE_H_ */
