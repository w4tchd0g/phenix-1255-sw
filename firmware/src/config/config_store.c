/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Phenix-1255 contributors
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/byteorder.h>

#include <fsl_device_registers.h>

#include "config/config_store.h"

LOG_MODULE_REGISTER(config_store, LOG_LEVEL_INF);

#define SETTINGS_PARTITION_ID DT_PARTITION_ID(DT_CHOSEN(zephyr_settings_partition))

#define NET_RECORD_VERSION     1
#define DISPLAY_RECORD_VERSION 2

/* On-flash layout of the network configuration, little endian */
struct net_record {
	uint8_t version;
	uint8_t dhcp;
	uint8_t ip[4];
	uint8_t netmask[4];
	uint8_t gateway[4];
	uint8_t dns[4];
	uint8_t ctrl_port[2];
	uint8_t sample_port[2];
} __packed;

/* On-flash layout of the display settings, little endian */
struct display_record {
	uint8_t version;
	uint8_t contrast;
	uint8_t dim_timeout_s[2];
	uint8_t auto_pages;
	uint8_t encoder_reversed;
} __packed;

/* Version 1 ended here; such a record keeps its settings after an update */
#define DISPLAY_RECORD_V1      1
#define DISPLAY_RECORD_V1_SIZE offsetof(struct display_record, encoder_reversed)

/*
 * The MCXN flash driver (soc_flash_mcux.c, Zephyr v4.5.0-rc3) invalidates
 * the flash cache and the LPCAC code cache after every program and erase by
 * setting NVM_CTRL[CLR_FLASH_CACHE] and LPCAC_CTRL[CLR_LPCAC], but never
 * writes them back to 0. Both bits hold their cache cleared while set, so
 * the code runs uncached until the next reset and Ethernet streaming loses
 * about 60 % of the RX samples. Every flash operation here is followed by
 * this release.
 */
static void flash_caches_release(void)
{
	SYSCON->NVM_CTRL &= ~SYSCON_NVM_CTRL_CLR_FLASH_CACHE_MASK;
	SYSCON->LPCAC_CTRL &= ~SYSCON_LPCAC_CTRL_CLR_LPCAC_MASK;
	__DSB();
	__ISB();
}

static struct phenix_net_cfg net_cfg;
static struct phenix_display_cfg display_cfg;
static K_MUTEX_DEFINE(cfg_lock);

static void display_defaults(struct phenix_display_cfg *cfg)
{
	cfg->contrast = PHENIX_DEFAULT_DISPLAY_CONTRAST;
	cfg->dim_timeout_s = PHENIX_DEFAULT_DISPLAY_DIM_S;
	cfg->auto_pages = PHENIX_DEFAULT_DISPLAY_AUTO_PAGES;
	cfg->encoder_reversed = PHENIX_DEFAULT_DISPLAY_ENCODER_REVERSED;
}

static bool display_cfg_valid(const struct phenix_display_cfg *cfg)
{
	return cfg->contrast >= 1U && cfg->contrast <= PHENIX_CONTRAST_MAX &&
	       cfg->dim_timeout_s <= PHENIX_DIM_TIMEOUT_MAX_S;
}

void config_store_defaults(struct phenix_net_cfg *cfg)
{
	const struct phenix_net_cfg defaults = {
		.dhcp = PHENIX_DEFAULT_DHCP,
		.ip = PHENIX_DEFAULT_IP,
		.netmask = PHENIX_DEFAULT_NETMASK,
		.gateway = PHENIX_DEFAULT_GATEWAY,
		.dns = PHENIX_DEFAULT_DNS,
		.ctrl_port = PHENIX_DEFAULT_CTRL_PORT,
		.sample_port = PHENIX_DEFAULT_SAMPLE_PORT,
	};

	*cfg = defaults;
}

static void record_from_cfg(struct net_record *rec, const struct phenix_net_cfg *cfg)
{
	rec->version = NET_RECORD_VERSION;
	rec->dhcp = cfg->dhcp ? 1U : 0U;
	memcpy(rec->ip, cfg->ip, 4);
	memcpy(rec->netmask, cfg->netmask, 4);
	memcpy(rec->gateway, cfg->gateway, 4);
	memcpy(rec->dns, cfg->dns, 4);
	sys_put_le16(cfg->ctrl_port, rec->ctrl_port);
	sys_put_le16(cfg->sample_port, rec->sample_port);
}

static void cfg_from_record(struct phenix_net_cfg *cfg, const struct net_record *rec)
{
	cfg->dhcp = rec->dhcp != 0U;
	memcpy(cfg->ip, rec->ip, 4);
	memcpy(cfg->netmask, rec->netmask, 4);
	memcpy(cfg->gateway, rec->gateway, 4);
	memcpy(cfg->dns, rec->dns, 4);
	cfg->ctrl_port = sys_get_le16(rec->ctrl_port);
	cfg->sample_port = sys_get_le16(rec->sample_port);
}

static int phenix_settings_set(const char *key, size_t len, settings_read_cb read_cb,
			       void *cb_arg)
{
	const char *next;

	if (settings_name_steq(key, "net", &next) && next == NULL) {
		struct net_record rec;
		struct phenix_net_cfg cfg;
		ssize_t rd;
		const char *reason;

		if (len != sizeof(rec)) {
			LOG_WRN("Stored network configuration has wrong size, ignored");
			return 0;
		}

		rd = read_cb(cb_arg, &rec, sizeof(rec));
		if (rd != sizeof(rec)) {
			return (rd < 0) ? (int)rd : -EIO;
		}

		if (rec.version != NET_RECORD_VERSION) {
			LOG_WRN("Stored network configuration version %u unsupported, ignored",
				rec.version);
			return 0;
		}

		cfg_from_record(&cfg, &rec);
		reason = config_store_validate(&cfg);
		if (reason != NULL) {
			LOG_WRN("Stored network configuration invalid (%s), ignored", reason);
			return 0;
		}

		net_cfg = cfg;
		return 0;
	}

	if (settings_name_steq(key, "display", &next) && next == NULL) {
		struct display_record rec = {.encoder_reversed =
						     PHENIX_DEFAULT_DISPLAY_ENCODER_REVERSED};
		struct phenix_display_cfg cfg;
		uint8_t version;
		ssize_t rd;

		if (len == sizeof(rec)) {
			version = DISPLAY_RECORD_VERSION;
		} else if (len == DISPLAY_RECORD_V1_SIZE) {
			version = DISPLAY_RECORD_V1;
		} else {
			LOG_WRN("Stored display settings have wrong size, ignored");
			return 0;
		}

		rd = read_cb(cb_arg, &rec, len);
		if (rd != (ssize_t)len) {
			return (rd < 0) ? (int)rd : -EIO;
		}

		cfg.contrast = rec.contrast;
		cfg.dim_timeout_s = sys_get_le16(rec.dim_timeout_s);
		cfg.auto_pages = rec.auto_pages != 0U;
		cfg.encoder_reversed = rec.encoder_reversed != 0U;
		if (rec.version != version || !display_cfg_valid(&cfg)) {
			LOG_WRN("Stored display settings invalid, ignored");
			return 0;
		}

		display_cfg = cfg;
		return 0;
	}

	return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(phenix, "phenix", NULL, phenix_settings_set, NULL, NULL);

int config_store_erase(void)
{
	const struct flash_area *fa;
	int ret;

	ret = flash_area_open(SETTINGS_PARTITION_ID, &fa);
	if (ret < 0) {
		return ret;
	}

	ret = flash_area_erase(fa, 0, fa->fa_size);
	flash_area_close(fa);
	flash_caches_release();

	return ret;
}

int config_store_init(bool factory_reset)
{
	int ret;

	config_store_defaults(&net_cfg);
	display_defaults(&display_cfg);

	if (factory_reset) {
		ret = config_store_erase();
		if (ret < 0) {
			LOG_ERR("Erasing settings partition failed (%d)", ret);
			return ret;
		}
		LOG_WRN("Factory reset: all settings erased");
	}

	/* Mounting and loading may write to the flash (NVS recovery, GC) */
	ret = settings_subsys_init();
	flash_caches_release();
	if (ret < 0) {
		LOG_ERR("Settings init failed (%d)", ret);
		return ret;
	}

	ret = settings_load_subtree("phenix");
	flash_caches_release();
	if (ret < 0) {
		LOG_ERR("Loading settings failed (%d)", ret);
		return ret;
	}

	return 0;
}

void config_store_get_net(struct phenix_net_cfg *cfg)
{
	k_mutex_lock(&cfg_lock, K_FOREVER);
	*cfg = net_cfg;
	k_mutex_unlock(&cfg_lock);
}

int config_store_set_net(const struct phenix_net_cfg *cfg)
{
	struct net_record rec;
	const char *reason;
	int ret;

	reason = config_store_validate(cfg);
	if (reason != NULL) {
		LOG_WRN("Rejected network configuration: %s", reason);
		return -EINVAL;
	}

	record_from_cfg(&rec, cfg);

	k_mutex_lock(&cfg_lock, K_FOREVER);
	ret = settings_save_one("phenix/net", &rec, sizeof(rec));
	flash_caches_release();
	if (ret == 0) {
		net_cfg = *cfg;
	}
	k_mutex_unlock(&cfg_lock);

	if (ret < 0) {
		LOG_ERR("Saving network configuration failed (%d)", ret);
	}

	return ret;
}

void config_store_get_display(struct phenix_display_cfg *cfg)
{
	k_mutex_lock(&cfg_lock, K_FOREVER);
	*cfg = display_cfg;
	k_mutex_unlock(&cfg_lock);
}

int config_store_set_display(const struct phenix_display_cfg *cfg)
{
	struct display_record rec;
	int ret;

	if (!display_cfg_valid(cfg)) {
		return -EINVAL;
	}

	rec.version = DISPLAY_RECORD_VERSION;
	rec.contrast = cfg->contrast;
	sys_put_le16(cfg->dim_timeout_s, rec.dim_timeout_s);
	rec.auto_pages = cfg->auto_pages ? 1U : 0U;
	rec.encoder_reversed = cfg->encoder_reversed ? 1U : 0U;

	k_mutex_lock(&cfg_lock, K_FOREVER);
	ret = settings_save_one("phenix/display", &rec, sizeof(rec));
	flash_caches_release();
	if (ret == 0) {
		display_cfg = *cfg;
	}
	k_mutex_unlock(&cfg_lock);

	if (ret < 0) {
		LOG_ERR("Saving display settings failed (%d)", ret);
	}

	return ret;
}

static uint32_t ipv4_to_u32(const uint8_t addr[4])
{
	return sys_get_be32(addr);
}

const char *config_store_validate(const struct phenix_net_cfg *cfg)
{
	uint32_t ip = ipv4_to_u32(cfg->ip);
	uint32_t mask = ipv4_to_u32(cfg->netmask);
	uint32_t gw = ipv4_to_u32(cfg->gateway);

	if (cfg->ctrl_port == 0U || cfg->sample_port == 0U) {
		return "port 0 is not allowed";
	}
	if (cfg->ctrl_port == PHENIX_HTTP_PORT) {
		return "control port collides with the web interface";
	}

	if (cfg->dhcp) {
		/* Static fields are kept but not used */
		return NULL;
	}

	/* Netmask must be a contiguous prefix of length 1..30 */
	if (mask == 0U || mask > 0xFFFFFFFCU || ((~mask + 1U) & ~mask) != 0U) {
		return "invalid netmask";
	}
	if (ip == 0U || (ip >> 24) == 127U || (ip >> 28) >= 0xEU) {
		return "invalid IP address";
	}
	if ((ip & ~mask) == 0U || (ip & ~mask) == ~mask) {
		return "IP address is the network or broadcast address";
	}
	if (gw != 0U && (gw & mask) != (ip & mask)) {
		return "gateway is not in the local subnet";
	}

	return NULL;
}

int config_store_parse_ipv4(const char *str, uint8_t out[4])
{
	unsigned int part[4];
	char tail;

	if (str == NULL ||
	    sscanf(str, "%u.%u.%u.%u%c", &part[0], &part[1], &part[2], &part[3], &tail) != 4) {
		return -EINVAL;
	}

	for (int i = 0; i < 4; i++) {
		if (part[i] > 255U) {
			return -EINVAL;
		}
		out[i] = (uint8_t)part[i];
	}

	return 0;
}

void config_store_format_ipv4(const uint8_t addr[4], char *buf, size_t len)
{
	snprintf(buf, len, "%u.%u.%u.%u", addr[0], addr[1], addr[2], addr[3]);
}
