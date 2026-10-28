/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Phenix-1255 contributors
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/version.h>
#include <zephyr/app_version.h>

#include "board/board_io.h"
#include "board/watchdog.h"
#include "config/config_store.h"
#include "control/control.h"
#include "display/display_fb.h"
#include "display/display_ui.h"
#include "ethernet/net_config.h"
#include "radio/sx1255.h"
#include "radio/sx1255_regs.h"
#include "stream/sample_io.h"
#include "stream/stream.h"

LOG_MODULE_REGISTER(display_ui, LOG_LEVEL_INF);

#define UI_STACK_SIZE 2048
/* Below the web interface, above the watchdog feeder */
#define UI_PRIORITY   K_PRIO_PREEMPT(9)

#define REFRESH_MS      250
#define SPLASH_MS       1500
#define PAGE_PERIOD_MS  5000
/* Automatic page cycling resumes this long after a manual page change */
#define MANUAL_HOLD_MS  60000
/* Menus close after this time without input */
#define MENU_TIMEOUT_MS 60000
#define MESSAGE_MS      2500
#define LONG_PRESS_MS   800
/* Encoder detents closer than this step IP octets by 10 */
#define FAST_TURN_MS    60

/* SSD1306 contrast per brightness level, and while dimmed */
#define CONTRAST_STEP 25
#define CONTRAST_DIM  0

/*
 * Text rows 0 and 1 lie in the yellow zone (pixel rows 0..15) of the
 * two-colour panel, rows 2..7 in the blue zone (16..63).
 */
#define ROW_TITLE  0
#define ROW_SUB    1
#define ROW_BODY   2
#define BODY_LINES (FB_ROWS - ROW_BODY)

/* Inset of menu labels and values from the screen edges */
#define MENU_INSET 2

static const struct device *const disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

enum ui_event_type {
	EV_KEY,
	EV_TEST,
};

struct ui_event {
	uint8_t type;
	uint8_t arg;
};

K_MSGQ_DEFINE(ui_events, sizeof(struct ui_event), 16, 1);

enum screen {
	SCREEN_SPLASH,
	SCREEN_PAGES,
	SCREEN_MENU,
	SCREEN_CONFIRM,
	SCREEN_MESSAGE,
	SCREEN_TEST,
};

static const char *const screen_names[] = {
	[SCREEN_SPLASH] = "splash", [SCREEN_PAGES] = "pages",     [SCREEN_MENU] = "menu",
	[SCREEN_CONFIRM] = "confirm", [SCREEN_MESSAGE] = "message", [SCREEN_TEST] = "test",
};

enum page {
	PAGE_STATUS,
	PAGE_NETWORK,
	PAGE_RF,
	PAGE_STREAM,
	PAGE_SYSTEM,
	PAGE_COUNT,
};

static const char *const page_titles[PAGE_COUNT] = {
	"STATUS", "NETWORK", "RF", "STREAM", "SYSTEM",
};

enum item {
	ITEM_DISPLAY,
	ITEM_NETWORK,
	ITEM_REBOOT,
	ITEM_FACTORY_RESET,
	ITEM_CONTRAST,
	ITEM_DIM,
	ITEM_AUTO_PAGES,
	ITEM_NET_MODE,
	ITEM_NET_IP,
	ITEM_NET_MASK,
	ITEM_NET_GW,
	ITEM_NET_DNS,
	ITEM_NET_SAVE,
	ITEM_BACK,
};

static const char *const item_labels[] = {
	[ITEM_DISPLAY] = "Display",     [ITEM_NETWORK] = "Network",
	[ITEM_REBOOT] = "Reboot",       [ITEM_FACTORY_RESET] = "Factory reset",
	[ITEM_CONTRAST] = "Brightness", [ITEM_DIM] = "Dim after",
	[ITEM_AUTO_PAGES] = "Auto pages", [ITEM_NET_MODE] = "Mode",
	[ITEM_NET_IP] = "IP",           [ITEM_NET_MASK] = "Mask",
	[ITEM_NET_GW] = "GW",           [ITEM_NET_DNS] = "DNS",
	[ITEM_NET_SAVE] = "Save+reboot", [ITEM_BACK] = "Back",
};

enum menu_id {
	MENU_MAIN,
	MENU_DISPLAY,
	MENU_NETWORK,
};

static const uint8_t main_items[] = {ITEM_DISPLAY, ITEM_NETWORK, ITEM_REBOOT, ITEM_FACTORY_RESET,
				     ITEM_BACK};
static const uint8_t display_items[] = {ITEM_CONTRAST, ITEM_DIM, ITEM_AUTO_PAGES, ITEM_BACK};
static const uint8_t network_items[] = {ITEM_NET_MODE, ITEM_NET_IP,  ITEM_NET_MASK, ITEM_NET_GW,
					ITEM_NET_DNS,  ITEM_NET_SAVE, ITEM_BACK};

struct menu {
	const char *title;
	const uint8_t *items;
	uint8_t count;
};

static const struct menu menus[] = {
	[MENU_MAIN] = {"MENU", main_items, ARRAY_SIZE(main_items)},
	[MENU_DISPLAY] = {"DISPLAY", display_items, ARRAY_SIZE(display_items)},
	[MENU_NETWORK] = {"NETWORK", network_items, ARRAY_SIZE(network_items)},
};

enum confirm {
	CONFIRM_REBOOT,
	CONFIRM_FACTORY_RESET,
	CONFIRM_FACTORY_RESET_AGAIN,
	CONFIRM_NET_SAVE,
};

static const uint16_t dim_options_s[] = {0, 60, 120, 300, 600, 1800, 3600};

/* Snapshot of the device state for one frame */
struct ui_status {
	bool session;
	bool error;
	struct stream_stats st;
	struct sample_io_stats io;
};

static struct {
	enum screen screen;
	int page;
	enum menu_id menu;
	int sel;
	int top;
	bool editing;
	int octet;
	enum confirm confirm;
	bool confirm_yes;
	const char *msg_title;
	char msg[3][FB_COLS + 1];
	/* Menu shown after the message; final messages stay until the reset */
	bool msg_final;
	enum menu_id msg_menu;
	int msg_sel;
	int64_t screen_until;
	int64_t last_input;
	int64_t last_adjust;
	int64_t page_since;
	int64_t manual_until;
	bool dimmed;
	bool ready;
	struct phenix_display_cfg cfg;
	bool cfg_dirty;
	struct phenix_net_cfg net;
	bool net_dirty;
	uint32_t activity;
} ui;

static uint8_t fb[FB_SIZE];
static uint8_t fb_sent[FB_SIZE];
static bool fb_sent_valid;
static bool write_failed;

static void post_event(uint8_t type, uint8_t arg)
{
	struct ui_event ev = {.type = type, .arg = arg};

	/* Inputs beyond the queue depth are dropped, the operator repeats them */
	(void)k_msgq_put(&ui_events, &ev, K_NO_WAIT);
}

/*
 * Runs in the context of the reporting driver (INPUT_MODE_SYNCHRONOUS):
 * the work queue for buttons, the sampling timer for the encoder.
 */
static void input_cb(struct input_event *evt, void *user_data)
{
	static int64_t press_start;

	ARG_UNUSED(user_data);

	if (evt->type == INPUT_EV_REL && evt->code == INPUT_REL_WHEEL) {
		uint8_t key = (evt->value > 0) ? DISPLAY_KEY_NEXT : DISPLAY_KEY_PREV;

		for (int i = 0; i < abs(evt->value); i++) {
			post_event(EV_KEY, key);
		}
	} else if (evt->type == INPUT_EV_KEY && evt->code == INPUT_KEY_DOWN) {
		if (evt->value != 0) {
			post_event(EV_KEY, DISPLAY_KEY_NEXT);
		}
	} else if (evt->type == INPUT_EV_KEY && evt->code == INPUT_KEY_ENTER) {
		if (evt->value != 0) {
			press_start = k_uptime_get();
		} else {
			post_event(EV_KEY, (k_uptime_get() - press_start >= LONG_PRESS_MS)
						   ? DISPLAY_KEY_BACK
						   : DISPLAY_KEY_SELECT);
		}
	}
}

INPUT_CALLBACK_DEFINE(NULL, input_cb, NULL);

void display_ui_inject(enum display_ui_key key)
{
	post_event(EV_KEY, (uint8_t)key);
}

void display_ui_test_pattern(int seconds)
{
	post_event(EV_TEST, (uint8_t)CLAMP(seconds, 1, 60));
}

void display_ui_get_info(struct display_ui_info *info)
{
	info->ready = ui.ready;
	info->dimmed = ui.dimmed;
	info->screen = screen_names[ui.screen];
	info->page = ui.page;
}

bool display_ui_snapshot(uint8_t *buf, size_t len)
{
	if (!fb_sent_valid || len < FB_SIZE) {
		return false;
	}
	/* Diagnostics only: a frame sent meanwhile may tear the copy */
	memcpy(buf, fb_sent, FB_SIZE);

	return true;
}

/* Drawing helpers */

static void line(int idx, const char *fmt, ...)
{
	char buf[FB_COLS + 1];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	fb_text(fb, ROW_BODY + idx, 0, buf, false);
}

static void text_right(int row, const char *str, bool invert)
{
	fb_text(fb, row, FB_WIDTH - fb_text_width(str), str, invert);
}

static void draw_header(const char *title, const struct ui_status *s)
{
	const char *link = (board_io_comm_mode() == BOARD_COMM_USB) ? "USB" : "ETH";
	int x = FB_WIDTH;

	fb_text(fb, ROW_TITLE, 0, "PHENIX-1255", false);
	/* The host link label is highlighted while a host session is open */
	if (s->session) {
		fb_chip(fb, ROW_TITLE, FB_WIDTH - fb_chip_width(link), link);
	} else {
		text_right(ROW_TITLE, link, false);
	}

	fb_text(fb, ROW_SUB, 0, title, false);
	if (s->st.tx_enabled) {
		x -= fb_chip_width("TX");
		fb_chip(fb, ROW_SUB, x, "TX");
		x -= 2;
	}
	if (s->st.running) {
		x -= fb_chip_width("RX");
		fb_chip(fb, ROW_SUB, x, "RX");
		x -= 2;
	}
	if (s->error) {
		x -= fb_chip_width("ERR");
		fb_chip(fb, ROW_SUB, x, "ERR");
	}
}

static void collect_status(struct ui_status *s)
{
	s->session = control_session_active();
	s->error = board_led_get(BOARD_LED_ERR) != BOARD_LED_OFF || !sx1255_is_ready();
	stream_get_stats(&s->st);
	sample_io_get_stats(&s->io);
}

static void format_mhz(char *buf, size_t len, uint32_t hz)
{
	snprintf(buf, len, "%u.%06u MHz", hz / 1000000U, hz % 1000000U);
}

static const char *pll_state(bool enabled, uint8_t flags, uint8_t mask)
{
	if (!enabled) {
		return "off";
	}

	return (flags & mask) ? "lock" : "UNLK";
}

static void serial_string(char *buf, size_t len)
{
	uint8_t id[16];
	ssize_t n = hwinfo_get_device_id(id, sizeof(id));
	size_t pos = 0;

	for (ssize_t i = 0; i < n && pos + 2U < len; i++) {
		pos += snprintf(&buf[pos], len - pos, "%02x", id[i]);
	}
	buf[pos] = '\0';
}

/* Status pages */

static void draw_page_status(const struct ui_status *s)
{
	char freq[20];
	uint8_t lock = 0;
	bool rx_on, tx_on, tx_drv;

	format_mhz(freq, sizeof(freq), sx1255_get_frequency(SX1255_RX));
	line(0, "RX %s", freq);
	format_mhz(freq, sizeof(freq), sx1255_get_frequency(SX1255_TX));
	line(1, "TX %s", freq);
	line(2, "Rate %u kHz", sx1255_get_sample_rate() / 1000U);

	sx1255_get_mode(&rx_on, &tx_on, &tx_drv);
	if (sx1255_is_ready()) {
		(void)sx1255_get_pll_lock(&lock);
	}
	line(3, "PLL  RX %s  TX %s", pll_state(rx_on, lock, SX1255_STAT_PLL_LOCK_RX),
	     pll_state(tx_on, lock, SX1255_STAT_PLL_LOCK_TX));
	line(4, "Host %s", s->session ? "connected" : "none");
	line(5, "Late %u  Drop %u", s->st.tx_late, s->io.rx_dropped);
}

static void draw_page_network(void)
{
	struct phenix_net_cfg cfg;
	char addr[NET_IPV4_ADDR_LEN];
	char mac[18];

	net_config_get_address(addr, sizeof(addr));
	net_config_get_mac(mac, sizeof(mac));
	config_store_get_net(&cfg);

	line(0, "IP   %s", addr[0] ? addr : "-");
	line(1, "Mode %s", net_config_get_mode());
	line(2, "MAC %s", mac);
	line(3, "Ctrl %u/tcp", cfg.ctrl_port);
	line(4, "Data %u/udp", cfg.sample_port);
	line(5, "Host link %s", board_comm_mode_str(board_io_comm_mode()));
}

static void draw_page_rf(void)
{
	bool rx_on, tx_on, tx_drv;

	sx1255_get_mode(&rx_on, &tx_on, &tx_drv);

	line(0, "LNA %2d dB  PGA %2d dB", sx1255_get_gain(SX1255_GAIN_LNA),
	     sx1255_get_gain(SX1255_GAIN_PGA));
	line(1, "DAC %2d dB  MIX %2d dB", sx1255_get_gain(SX1255_GAIN_DAC),
	     sx1255_get_gain(SX1255_GAIN_MIXER));
	line(2, "RX input  %s",
	     (sx1255_get_rx_input() == SX1255_RX_INPUT_LOOPBACK) ? "loopback" : "antenna");
	line(3, "TX driver %s", tx_drv ? "on" : "off");
	line(4, "PA        %s", board_pa_is_enabled() ? "ON" : "off");
	line(5, "SX1255    %s", sx1255_is_ready() ? "ready" : "NOT READY");
}

static void draw_page_stream(const struct ui_status *s)
{
	line(0, "Count %llu", (unsigned long long)s->st.counter);
	line(1, "RX pkt %u drop %u", s->io.rx_packets, s->io.rx_dropped);
	line(2, "TX pkt %u gap %u", s->io.tx_packets, s->io.tx_seq_gaps);
	line(3, "TX late %u early %u", s->st.tx_late, s->st.tx_early);
	line(4, "FIFO rx %u tx %u", s->st.rx_fifo_errors, s->st.tx_fifo_errors);
	line(5, "Sync rx %u tx %u", s->st.rx_sync_errors, s->st.tx_sync_errors);
}

static void draw_page_system(void)
{
	char serial[33];
	uint32_t up = (uint32_t)k_uptime_seconds();

	serial_string(serial, sizeof(serial));

	line(0, "FW %s", APP_VERSION_EXTENDED_STRING);
	line(1, "Zephyr %s", KERNEL_VERSION_STRING);
	line(2, "Up %ud %02u:%02u:%02u", up / 86400U, (up / 3600U) % 24U, (up / 60U) % 60U,
	     up % 60U);
	line(3, "Reset %s", watchdog_reset_cause());
	line(4, "SN %.16s", serial);
	line(5, "   %s", (strlen(serial) > 16U) ? &serial[16] : "");
}

static void draw_pages(const struct ui_status *s)
{
	char title[16];

	snprintf(title, sizeof(title), "%s %d/%d", page_titles[ui.page], ui.page + 1, PAGE_COUNT);
	draw_header(title, s);

	switch (ui.page) {
	case PAGE_STATUS:
		draw_page_status(s);
		break;
	case PAGE_NETWORK:
		draw_page_network();
		break;
	case PAGE_RF:
		draw_page_rf();
		break;
	case PAGE_STREAM:
		draw_page_stream(s);
		break;
	case PAGE_SYSTEM:
	default:
		draw_page_system();
		break;
	}
}

/* Menu */

static int netmask_prefix(const uint8_t mask[4])
{
	return (int)__builtin_popcount(sys_get_be32(mask));
}

static void set_netmask_prefix(uint8_t mask[4], int prefix)
{
	sys_put_be32((prefix <= 0) ? 0U : (uint32_t)(0xFFFFFFFFULL << (32 - prefix)), mask);
}

static uint8_t *item_ipv4(enum item item)
{
	switch (item) {
	case ITEM_NET_IP:
		return ui.net.ip;
	case ITEM_NET_GW:
		return ui.net.gateway;
	case ITEM_NET_DNS:
		return ui.net.dns;
	default:
		return NULL;
	}
}

static int dim_option_index(uint16_t seconds)
{
	for (int i = 0; i < ARRAY_SIZE(dim_options_s); i++) {
		if (dim_options_s[i] >= seconds) {
			return i;
		}
	}

	return ARRAY_SIZE(dim_options_s) - 1;
}

static void item_value(enum item item, char *buf, size_t len)
{
	buf[0] = '\0';

	switch (item) {
	case ITEM_DISPLAY:
	case ITEM_NETWORK:
		snprintf(buf, len, ">");
		break;
	case ITEM_CONTRAST:
		snprintf(buf, len, "%u", ui.cfg.contrast);
		break;
	case ITEM_DIM:
		if (ui.cfg.dim_timeout_s == 0U) {
			snprintf(buf, len, "never");
		} else {
			snprintf(buf, len, "%u min", ui.cfg.dim_timeout_s / 60U);
		}
		break;
	case ITEM_AUTO_PAGES:
		snprintf(buf, len, "%s", ui.cfg.auto_pages ? "on" : "off");
		break;
	case ITEM_NET_MODE:
		snprintf(buf, len, "%s", ui.net.dhcp ? "DHCP" : "static");
		break;
	case ITEM_NET_MASK:
		snprintf(buf, len, "/%d", netmask_prefix(ui.net.netmask));
		break;
	case ITEM_NET_IP:
	case ITEM_NET_GW:
	case ITEM_NET_DNS:
		config_store_format_ipv4(item_ipv4(item), buf, len);
		break;
	default:
		break;
	}
}

/*
 * Draw an IPv4 value right-aligned; while editing, the active octet is
 * drawn normal inside the inverted row.
 */
static void draw_ipv4_value(int row, const uint8_t addr[4], bool selected, bool editing)
{
	char part[5];
	char full[PHENIX_IPV4_STR_LEN];
	int x;

	config_store_format_ipv4(addr, full, sizeof(full));
	x = FB_WIDTH - MENU_INSET - fb_text_width(full);

	for (int i = 0; i < 4; i++) {
		snprintf(part, sizeof(part), (i < 3) ? "%u." : "%u", addr[i]);
		if (editing && i == ui.octet) {
			/* Without the dot, so that only the number is highlighted */
			part[strcspn(part, ".")] = '\0';
			x = fb_text(fb, row, x, part, false);
			if (i < 3) {
				x = fb_text(fb, row, x, ".", selected);
			}
		} else {
			x = fb_text(fb, row, x, part, selected);
		}
	}
}

static void draw_menu(const struct ui_status *s)
{
	const struct menu *m = &menus[ui.menu];

	draw_header(m->title, s);

	for (int i = 0; i < BODY_LINES && ui.top + i < m->count; i++) {
		int idx = ui.top + i;
		enum item item = m->items[idx];
		bool selected = (idx == ui.sel);
		int row = ROW_BODY + i;
		char value[PHENIX_IPV4_STR_LEN];

		if (selected) {
			fb_fill(fb, row, 0, FB_WIDTH, true);
		}
		fb_text(fb, row, MENU_INSET, item_labels[item], selected);

		if (item_ipv4(item) != NULL) {
			draw_ipv4_value(row, item_ipv4(item), selected, selected && ui.editing);
			continue;
		}

		item_value(item, value, sizeof(value));
		if (value[0] != '\0') {
			int x = FB_WIDTH - MENU_INSET - fb_text_width(value);

			/* The value under edit is drawn normal inside the inverted row */
			if (selected && ui.editing) {
				fb_fill(fb, row, x - 1, FB_WIDTH - MENU_INSET + 1, false);
			}
			fb_text(fb, row, x, value, selected && !ui.editing);
		}
	}
}

static void set_contrast(uint8_t level)
{
	(void)display_set_contrast(disp, level);
}

static void apply_brightness(void)
{
	set_contrast(ui.dimmed ? CONTRAST_DIM : ui.cfg.contrast * CONTRAST_STEP);
}

static void open_menu(enum menu_id menu, int sel)
{
	ui.screen = SCREEN_MENU;
	ui.menu = menu;
	ui.sel = sel;
	ui.top = 0;
	ui.editing = false;
}

static void menu_scroll(void)
{
	if (ui.sel < ui.top) {
		ui.top = ui.sel;
	} else if (ui.sel >= ui.top + BODY_LINES) {
		ui.top = ui.sel - BODY_LINES + 1;
	}
}

/* Word-wrapped text on up to three lines; the rest is cut off */
static void show_message(const char *title, const char *text, enum menu_id menu, int sel)
{
	ui.screen = SCREEN_MESSAGE;
	ui.msg_title = title;

	for (int i = 0; i < ARRAY_SIZE(ui.msg); i++) {
		size_t len = strlen(text);

		if (len > FB_COLS) {
			len = FB_COLS;
			while (len > 0U && text[len] != ' ') {
				len--;
			}
			if (len == 0U) {
				len = FB_COLS;
			}
		}
		snprintf(ui.msg[i], sizeof(ui.msg[i]), "%.*s", (int)len, text);
		text += len;
		while (*text == ' ') {
			text++;
		}
	}
	ui.msg_final = false;
	ui.msg_menu = menu;
	ui.msg_sel = sel;
	ui.screen_until = k_uptime_get() + MESSAGE_MS;
}

static void close_message(void)
{
	open_menu(ui.msg_menu, ui.msg_sel);
	menu_scroll();
}

static void open_confirm(enum confirm confirm)
{
	ui.screen = SCREEN_CONFIRM;
	ui.confirm = confirm;
	ui.confirm_yes = false;
}

/*
 * The flash driver programs and erases with interrupts locked, and an NVS
 * sector erase takes milliseconds: while a stream runs, saving is deferred
 * (update_timers) unless the device reboots anyway.
 */
static void save_display_cfg(bool force)
{
	if (ui.cfg_dirty && (force || !stream_is_running())) {
		ui.cfg_dirty = false;
		(void)config_store_set_display(&ui.cfg);
	}
}

/* Leave the current menu towards the status pages */
static void menu_back(void)
{
	switch (ui.menu) {
	case MENU_DISPLAY:
		save_display_cfg(false);
		open_menu(MENU_MAIN, 0);
		break;
	case MENU_NETWORK:
		if (ui.net_dirty) {
			ui.net_dirty = false;
			show_message("NETWORK", "Changes discarded.", MENU_MAIN, 1);
		} else {
			open_menu(MENU_MAIN, 1);
		}
		break;
	case MENU_MAIN:
	default:
		ui.screen = SCREEN_PAGES;
		ui.page_since = k_uptime_get();
		break;
	}
}

static void menu_select(enum item item)
{
	const char *reason;

	switch (item) {
	case ITEM_DISPLAY:
		open_menu(MENU_DISPLAY, 0);
		break;
	case ITEM_NETWORK:
		config_store_get_net(&ui.net);
		ui.net_dirty = false;
		open_menu(MENU_NETWORK, 0);
		break;
	case ITEM_REBOOT:
		open_confirm(CONFIRM_REBOOT);
		break;
	case ITEM_FACTORY_RESET:
		open_confirm(CONFIRM_FACTORY_RESET);
		break;
	case ITEM_AUTO_PAGES:
		ui.cfg.auto_pages = !ui.cfg.auto_pages;
		ui.cfg_dirty = true;
		break;
	case ITEM_NET_MODE:
		ui.net.dhcp = !ui.net.dhcp;
		ui.net_dirty = true;
		break;
	case ITEM_CONTRAST:
	case ITEM_DIM:
	case ITEM_NET_MASK:
	case ITEM_NET_IP:
	case ITEM_NET_GW:
	case ITEM_NET_DNS:
		ui.editing = true;
		ui.octet = 0;
		break;
	case ITEM_NET_SAVE:
		reason = config_store_validate(&ui.net);
		if (reason != NULL) {
			show_message("NOT SAVED", reason, MENU_NETWORK, ui.sel);
		} else {
			open_confirm(CONFIRM_NET_SAVE);
		}
		break;
	case ITEM_BACK:
	default:
		menu_back();
		break;
	}
}

static void menu_adjust(enum item item, int delta, bool fast)
{
	uint8_t *addr = item_ipv4(item);
	int val;

	switch (item) {
	case ITEM_CONTRAST:
		ui.cfg.contrast = CLAMP(ui.cfg.contrast + delta, 1, PHENIX_CONTRAST_MAX);
		ui.cfg_dirty = true;
		apply_brightness();
		break;
	case ITEM_DIM:
		val = CLAMP(dim_option_index(ui.cfg.dim_timeout_s) + delta, 0,
			    (int)ARRAY_SIZE(dim_options_s) - 1);
		ui.cfg.dim_timeout_s = dim_options_s[val];
		ui.cfg_dirty = true;
		break;
	case ITEM_NET_MASK:
		set_netmask_prefix(ui.net.netmask,
				   CLAMP(netmask_prefix(ui.net.netmask) + delta, 1, 30));
		ui.net_dirty = true;
		break;
	case ITEM_NET_IP:
	case ITEM_NET_GW:
	case ITEM_NET_DNS:
		val = addr[ui.octet] + delta * (fast ? 10 : 1);
		addr[ui.octet] = (uint8_t)CLAMP(val, 0, 255);
		ui.net_dirty = true;
		break;
	default:
		break;
	}
}

static void menu_key(enum display_ui_key key, int64_t now)
{
	const struct menu *m = &menus[ui.menu];
	enum item item = m->items[ui.sel];
	int delta = (key == DISPLAY_KEY_NEXT) ? 1 : -1;

	if (ui.editing) {
		switch (key) {
		case DISPLAY_KEY_NEXT:
		case DISPLAY_KEY_PREV:
			menu_adjust(item, delta, now - ui.last_adjust < FAST_TURN_MS);
			ui.last_adjust = now;
			break;
		case DISPLAY_KEY_SELECT:
			if (item_ipv4(item) != NULL && ui.octet < 3) {
				ui.octet++;
			} else {
				ui.editing = false;
			}
			break;
		case DISPLAY_KEY_BACK:
		default:
			ui.editing = false;
			break;
		}
		return;
	}

	switch (key) {
	case DISPLAY_KEY_NEXT:
	case DISPLAY_KEY_PREV:
		ui.sel = CLAMP(ui.sel + delta, 0, m->count - 1);
		menu_scroll();
		break;
	case DISPLAY_KEY_SELECT:
		menu_select(item);
		break;
	case DISPLAY_KEY_BACK:
	default:
		menu_back();
		break;
	}
}

/* Confirmation dialogs */

static void draw_confirm(const struct ui_status *s)
{
	static const int no_x = 30;
	static const int yes_x = 80;

	switch (ui.confirm) {
	case CONFIRM_REBOOT:
		draw_header("REBOOT", s);
		line(0, "Reboot the device?");
		break;
	case CONFIRM_FACTORY_RESET:
		draw_header("RESET", s);
		line(0, "Erase all settings?");
		line(1, "Network and display");
		line(2, "return to defaults.");
		break;
	case CONFIRM_FACTORY_RESET_AGAIN:
		draw_header("RESET", s);
		line(0, "Really erase all");
		line(1, "settings and reboot?");
		break;
	case CONFIRM_NET_SAVE:
		draw_header("NETWORK", s);
		line(0, "Save the network");
		line(1, "settings and reboot?");
		break;
	}

	if (s->session) {
		line(3, "Host session ends.");
	}

	if (ui.confirm_yes) {
		fb_text(fb, FB_ROWS - 1, no_x, " No ", false);
		fb_chip(fb, FB_ROWS - 1, yes_x, " Yes ");
	} else {
		fb_chip(fb, FB_ROWS - 1, no_x, " No ");
		fb_text(fb, FB_ROWS - 1, yes_x, " Yes ", false);
	}
}

static void render(void);

static void reboot_now(bool factory_reset)
{
	show_message(factory_reset ? "RESET" : "REBOOT", "Rebooting ...", MENU_MAIN, 0);
	ui.msg_final = true;
	render();
	control_reboot(factory_reset);
}

static void confirm_key(enum display_ui_key key)
{
	switch (key) {
	case DISPLAY_KEY_NEXT:
	case DISPLAY_KEY_PREV:
		ui.confirm_yes = !ui.confirm_yes;
		return;
	case DISPLAY_KEY_BACK:
		ui.confirm_yes = false;
		break;
	case DISPLAY_KEY_SELECT:
	default:
		break;
	}

	if (!ui.confirm_yes) {
		ui.screen = SCREEN_MENU;
		return;
	}

	switch (ui.confirm) {
	case CONFIRM_REBOOT:
		save_display_cfg(true);
		reboot_now(false);
		break;
	case CONFIRM_FACTORY_RESET:
		open_confirm(CONFIRM_FACTORY_RESET_AGAIN);
		break;
	case CONFIRM_FACTORY_RESET_AGAIN:
		reboot_now(true);
		break;
	case CONFIRM_NET_SAVE:
		save_display_cfg(true);
		if (config_store_set_net(&ui.net) < 0) {
			show_message("NOT SAVED", "Saving failed.", MENU_NETWORK, ui.sel);
			break;
		}
		ui.net_dirty = false;
		reboot_now(false);
		break;
	}
}

/* Other screens */

static void draw_message(const struct ui_status *s)
{
	draw_header(ui.msg_title, s);
	for (int i = 0; i < ARRAY_SIZE(ui.msg); i++) {
		line(i + 1, "%s", ui.msg[i]);
	}
}

static void draw_splash(void)
{
	fb_text(fb, ROW_TITLE, (FB_WIDTH - fb_text_width("PHENIX-1255")) / 2, "PHENIX-1255",
		false);
	line(1, "  SX1255 transceiver");
	line(3, "  FW %s", APP_VERSION_EXTENDED_STRING);
	line(4, "  Starting ...");
}

/* Marks the colour zones: yellow rows 0..15 lit, blue rows 16..63 framed */
static void draw_test(void)
{
	fb_fill(fb, ROW_TITLE, 0, FB_WIDTH, true);
	fb_fill(fb, ROW_SUB, 0, FB_WIDTH, true);
	fb_text(fb, ROW_TITLE, 4, "YELLOW ZONE", true);
	fb_text(fb, ROW_SUB, 4, "rows 0..15", true);

	for (int row = ROW_BODY; row < FB_ROWS; row++) {
		uint8_t edge = 0xFF;

		fb[row * FB_WIDTH] = edge;
		fb[row * FB_WIDTH + FB_WIDTH - 1] = edge;
	}
	for (int x = 0; x < FB_WIDTH; x++) {
		fb[ROW_BODY * FB_WIDTH + x] |= BIT(0);
		fb[(FB_ROWS - 1) * FB_WIDTH + x] |= BIT(7);
	}
	fb_text(fb, 4, 4, "BLUE ZONE", false);
	fb_text(fb, 5, 4, "rows 16..63", false);
}

static void flush(void)
{
	struct display_buffer_descriptor desc = {
		.buf_size = FB_SIZE,
		.width = FB_WIDTH,
		.height = FB_HEIGHT,
		.pitch = FB_WIDTH,
	};
	int ret;

	if (fb_sent_valid && memcmp(fb, fb_sent, FB_SIZE) == 0) {
		return;
	}

	ret = display_write(disp, 0, 0, &desc, fb);
	if (ret < 0) {
		if (!write_failed) {
			LOG_WRN("Display write failed (%d)", ret);
			write_failed = true;
		}
		fb_sent_valid = false;
		return;
	}

	if (write_failed) {
		LOG_INF("Display write recovered");
		write_failed = false;
	}
	memcpy(fb_sent, fb, FB_SIZE);
	fb_sent_valid = true;
}

static void render(void)
{
	struct ui_status s;

	collect_status(&s);
	fb_clear(fb);

	switch (ui.screen) {
	case SCREEN_SPLASH:
		draw_splash();
		break;
	case SCREEN_PAGES:
		draw_pages(&s);
		break;
	case SCREEN_MENU:
		draw_menu(&s);
		break;
	case SCREEN_CONFIRM:
		draw_confirm(&s);
		break;
	case SCREEN_MESSAGE:
		draw_message(&s);
		break;
	case SCREEN_TEST:
		draw_test();
		break;
	}

	flush();
}

/* State that brightens a dimmed display when it changes */
static uint32_t activity_signature(void)
{
	struct ui_status s;

	collect_status(&s);

	return (s.session ? BIT(0) : 0U) | (s.st.tx_enabled ? BIT(1) : 0U) |
	       (s.error ? BIT(2) : 0U);
}

static void wake(int64_t now)
{
	ui.last_input = now;
	if (ui.dimmed) {
		ui.dimmed = false;
		apply_brightness();
	}
}

static void handle_key(enum display_ui_key key, int64_t now)
{
	switch (ui.screen) {
	case SCREEN_PAGES:
		if (key == DISPLAY_KEY_SELECT) {
			open_menu(MENU_MAIN, 0);
		} else if (key == DISPLAY_KEY_BACK) {
			ui.page = PAGE_STATUS;
		} else {
			ui.page = (ui.page + ((key == DISPLAY_KEY_NEXT) ? 1 : PAGE_COUNT - 1)) %
				  PAGE_COUNT;
			ui.manual_until = now + MANUAL_HOLD_MS;
		}
		ui.page_since = now;
		break;
	case SCREEN_MENU:
		menu_key(key, now);
		break;
	case SCREEN_CONFIRM:
		confirm_key(key);
		break;
	case SCREEN_MESSAGE:
		if (!ui.msg_final) {
			close_message();
		}
		break;
	case SCREEN_SPLASH:
	case SCREEN_TEST:
	default:
		ui.screen = SCREEN_PAGES;
		break;
	}
}

static void handle_event(const struct ui_event *ev, int64_t now)
{
	bool was_dimmed = ui.dimmed;

	if (ev->type == EV_TEST) {
		ui.screen = SCREEN_TEST;
		ui.screen_until = now + ev->arg * 1000LL;
		wake(now);
		return;
	}

	wake(now);
	/* The first input on a dimmed display only brightens it */
	if (!was_dimmed) {
		handle_key((enum display_ui_key)ev->arg, now);
	}
}

static void update_timers(int64_t now)
{
	uint32_t activity = activity_signature();

	if (activity != ui.activity) {
		ui.activity = activity;
		wake(now);
	}

	if (!ui.dimmed && ui.cfg.dim_timeout_s != 0U &&
	    now - ui.last_input >= ui.cfg.dim_timeout_s * 1000LL) {
		ui.dimmed = true;
		apply_brightness();
	}

	/* Deferred save, once the stream has stopped and the menu is closed */
	if (ui.cfg_dirty && !(ui.screen == SCREEN_MENU && ui.menu == MENU_DISPLAY)) {
		save_display_cfg(false);
	}

	switch (ui.screen) {
	case SCREEN_SPLASH:
		if (now >= SPLASH_MS) {
			ui.screen = SCREEN_PAGES;
			ui.page_since = now;
		}
		break;
	case SCREEN_PAGES:
		if (ui.cfg.auto_pages && now >= ui.manual_until &&
		    now - ui.page_since >= PAGE_PERIOD_MS) {
			ui.page = (ui.page + 1) % PAGE_COUNT;
			ui.page_since = now;
		}
		break;
	case SCREEN_MENU:
	case SCREEN_CONFIRM:
		if (now - ui.last_input >= MENU_TIMEOUT_MS) {
			save_display_cfg(false);
			ui.net_dirty = false;
			ui.screen = SCREEN_PAGES;
			ui.page_since = now;
		}
		break;
	case SCREEN_MESSAGE:
		if (!ui.msg_final && now >= ui.screen_until) {
			close_message();
		}
		break;
	case SCREEN_TEST:
		if (now >= ui.screen_until) {
			ui.screen = SCREEN_PAGES;
			ui.page_since = now;
		}
		break;
	}
}

static void ui_thread(void *p1, void *p2, void *p3)
{
	struct ui_event ev;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	if (!device_is_ready(disp)) {
		LOG_WRN("Display not ready, front panel disabled");
		return;
	}

	config_store_get_display(&ui.cfg);
	ui.screen = SCREEN_SPLASH;
	ui.last_input = k_uptime_get();
	ui.activity = activity_signature();

	(void)display_set_pixel_format(disp, PIXEL_FORMAT_MONO01);
	apply_brightness();
	render();
	(void)display_blanking_off(disp);
	ui.ready = true;
	LOG_INF("Display ready");

	while (true) {
		int64_t now;

		if (k_msgq_get(&ui_events, &ev, K_MSEC(REFRESH_MS)) == 0) {
			handle_event(&ev, k_uptime_get());
			/* Apply queued inputs before drawing, e.g. a fast encoder turn */
			while (k_msgq_get(&ui_events, &ev, K_NO_WAIT) == 0) {
				handle_event(&ev, k_uptime_get());
			}
		}

		now = k_uptime_get();
		update_timers(now);
		render();
	}
}

K_THREAD_DEFINE(ui_tid, UI_STACK_SIZE, ui_thread, NULL, NULL, NULL, UI_PRIORITY, 0,
		K_TICKS_FOREVER);

int display_ui_start(void)
{
	k_thread_start(ui_tid);

	return 0;
}
