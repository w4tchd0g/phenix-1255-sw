/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Phenix-1255 contributors
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/net/dns_resolve.h>
#include <zephyr/net/net_mgmt.h>

#include "board/board_io.h"
#include "ethernet/net_config.h"

LOG_MODULE_REGISTER(net_config, LOG_LEVEL_INF);

static struct net_if *iface;
static struct net_mgmt_event_callback mgmt_cb;
static struct k_work_delayable dhcp_timeout_work;
static struct net_in_addr fallback_addr;
static bool fallback_active;
static bool dhcp_mode;
static bool dhcp_bound;
static char current_addr[NET_IPV4_ADDR_LEN];

static void to_in_addr(const uint8_t src[4], struct net_in_addr *dst)
{
	memcpy(&dst->s_addr, src, 4);
}

static void set_current(const struct net_in_addr *addr)
{
	if (addr == NULL) {
		current_addr[0] = '\0';
	} else {
		net_addr_ntop(NET_AF_INET, addr, current_addr, sizeof(current_addr));
	}
}

static int add_static(const struct net_in_addr *addr, const struct net_in_addr *mask)
{
	if (net_if_ipv4_addr_add(iface, (struct net_in_addr *)addr, NET_ADDR_MANUAL, 0) == NULL) {
		return -ENOMEM;
	}
	net_if_ipv4_set_netmask_by_addr(iface, addr, mask);

	return 0;
}

static void dhcp_timeout_handler(struct k_work *work)
{
	static const uint8_t ip[4] = NET_FALLBACK_IP;
	static const uint8_t mask[4] = NET_FALLBACK_NETMASK;
	struct net_in_addr netmask;

	ARG_UNUSED(work);

	to_in_addr(ip, &fallback_addr);
	to_in_addr(mask, &netmask);

	if (add_static(&fallback_addr, &netmask) < 0) {
		LOG_ERR("Adding fallback address failed");
		return;
	}

	/* DHCP continues in the background and replaces the fallback */
	fallback_active = true;
	set_current(&fallback_addr);
	board_led_set(BOARD_LED_WARN, BOARD_LED_ON);
	LOG_WRN("No DHCP lease within %d s, using fallback %s/24", NET_DHCP_TIMEOUT_S,
		current_addr);
}

static void net_event_handler(struct net_mgmt_event_callback *cb, uint64_t event,
			      struct net_if *evt_iface)
{
	ARG_UNUSED(cb);

	if (evt_iface != iface) {
		return;
	}

	if (event == NET_EVENT_IPV4_DHCP_BOUND) {
		struct net_if_dhcpv4 *dhcp = &iface->config.dhcpv4;

		k_work_cancel_delayable(&dhcp_timeout_work);
		if (fallback_active) {
			net_if_ipv4_addr_rm(iface, &fallback_addr);
			fallback_active = false;
		}
		set_current(&dhcp->requested_ip);
		dhcp_bound = true;
		board_led_set(BOARD_LED_WARN, BOARD_LED_OFF);
		LOG_INF("DHCP address %s, lease %u s", current_addr, dhcp->lease_time);
	} else if (event == NET_EVENT_IF_UP) {
		LOG_INF("Ethernet link up");
	} else if (event == NET_EVENT_IF_DOWN) {
		LOG_WRN("Ethernet link down");
	}
}

static void configure_dns(const uint8_t dns[4])
{
	struct net_in_addr addr;
	char dns_str[NET_IPV4_ADDR_LEN];
	const char *servers[] = {dns_str, NULL};
	int ret;

	to_in_addr(dns, &addr);
	if (addr.s_addr == 0U) {
		return;
	}

	net_addr_ntop(NET_AF_INET, &addr, dns_str, sizeof(dns_str));
	ret = dns_resolve_reconfigure(dns_resolve_get_default(), servers, NULL,
				      DNS_SOURCE_MANUAL);
	if (ret < 0) {
		LOG_WRN("DNS server configuration failed (%d)", ret);
	}
}

int net_config_start(const struct phenix_net_cfg *cfg)
{
	iface = net_if_get_default();
	if (iface == NULL) {
		LOG_ERR("No network interface");
		return -ENODEV;
	}

	k_work_init_delayable(&dhcp_timeout_work, dhcp_timeout_handler);
	net_mgmt_init_event_callback(&mgmt_cb, net_event_handler,
				     NET_EVENT_IPV4_DHCP_BOUND | NET_EVENT_IF_UP |
					     NET_EVENT_IF_DOWN);
	net_mgmt_add_event_callback(&mgmt_cb);

	dhcp_mode = cfg->dhcp;

	if (cfg->dhcp) {
		board_led_set(BOARD_LED_WARN, BOARD_LED_BLINK_SLOW);
		net_dhcpv4_start(iface);
		k_work_reschedule(&dhcp_timeout_work, K_SECONDS(NET_DHCP_TIMEOUT_S));
		LOG_INF("DHCP started, fallback after %d s", NET_DHCP_TIMEOUT_S);
	} else {
		struct net_in_addr addr, mask, gw;
		int ret;

		to_in_addr(cfg->ip, &addr);
		to_in_addr(cfg->netmask, &mask);
		to_in_addr(cfg->gateway, &gw);

		ret = add_static(&addr, &mask);
		if (ret < 0) {
			LOG_ERR("Static address configuration failed (%d)", ret);
			return ret;
		}
		if (gw.s_addr != 0U) {
			net_if_ipv4_set_gw(iface, &gw);
		}
		configure_dns(cfg->dns);
		set_current(&addr);
		LOG_INF("Static address %s", current_addr);
	}

	return 0;
}

void net_config_get_address(char *buf, size_t len)
{
	strncpy(buf, current_addr, len - 1U);
	buf[len - 1U] = '\0';
}

const char *net_config_get_mode(void)
{
	if (!dhcp_mode) {
		return "static";
	}
	if (dhcp_bound) {
		return "DHCP";
	}

	return fallback_active ? "fallback" : "DHCP (waiting)";
}

void net_config_get_mac(char *buf, size_t len)
{
	struct net_linkaddr *ll = (iface != NULL) ? net_if_get_link_addr(iface) : NULL;

	if (ll == NULL || ll->len < 6U) {
		buf[0] = '\0';
		return;
	}

	snprintf(buf, len, "%02x:%02x:%02x:%02x:%02x:%02x", ll->addr[0], ll->addr[1],
		 ll->addr[2], ll->addr[3], ll->addr[4], ll->addr[5]);
}
