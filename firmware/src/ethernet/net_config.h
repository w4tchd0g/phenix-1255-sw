/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Phenix-1255 contributors
 *
 * IPv4 configuration of the Ethernet interface: DHCP with fallback
 * address, or static configuration from the configuration store.
 */

#ifndef PHENIX_NET_CONFIG_H_
#define PHENIX_NET_CONFIG_H_

#include <stdbool.h>

#include "config/config_store.h"

/* Fallback address when DHCP gets no lease within the timeout */
#define NET_FALLBACK_IP      {192, 168, 2, 10}
#define NET_FALLBACK_NETMASK {255, 255, 255, 0}
#define NET_DHCP_TIMEOUT_S   30

int net_config_start(const struct phenix_net_cfg *cfg);

/** Current address as dotted quad; empty string if none. */
void net_config_get_address(char *buf, size_t len);

/** Address source: "DHCP", "DHCP (waiting)", "fallback" or "static". */
const char *net_config_get_mode(void);

/** MAC address as "xx:xx:xx:xx:xx:xx". */
void net_config_get_mac(char *buf, size_t len);

#endif /* PHENIX_NET_CONFIG_H_ */
