/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Phenix-1255 contributors
 */

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <zephyr/kernel.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/app_version.h>

#include "board/board_io.h"
#include "board/hw_id.h"
#include "board/watchdog.h"
#include "config/config_store.h"
#include "control/control.h"
#include "ethernet/net_config.h"
#include "ethernet/web_ui.h"
#include "radio/sx1255.h"
#include "stream/stream.h"

LOG_MODULE_REGISTER(web_ui, LOG_LEVEL_INF);

#define WEB_STACK_SIZE 4096
#define WEB_PRIORITY   K_PRIO_PREEMPT(8)

#define REQ_MAX       2048
#define BODY_MAX      1024
#define PAGE_MAX      6144
#define RECV_TIMEOUT  5 /* s */
#define REBOOT_DELAY  K_MSEC(500)

static char req[REQ_MAX + 1];
static char page[PAGE_MAX];
static size_t page_len;

static const char page_head[] =
	"<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
	"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
	"<title>Phenix-1255</title><style>"
	"body{font-family:sans-serif;max-width:40em;margin:1em auto;padding:0 1em}"
	"table{border-collapse:collapse}td{padding:.2em .8em .2em 0}"
	"label{display:inline-block;width:9em}input,select{margin:.2em 0;width:12em}"
	".err{color:#b00}.ok{color:#070}</style></head><body><h1>Phenix-1255</h1>";

static const char page_tail[] =
	"<p><small>Hold SW2 during boot to restore the factory settings.</small></p>"
	"</body></html>";

static void out(const char *fmt, ...)
{
	va_list ap;
	int n;

	if (page_len >= sizeof(page)) {
		return;
	}

	va_start(ap, fmt);
	n = vsnprintf(&page[page_len], sizeof(page) - page_len, fmt, ap);
	va_end(ap);

	if (n > 0) {
		page_len = MIN(page_len + (size_t)n, sizeof(page) - 1U);
	}
}

static void send_all(int sock, const char *buf, size_t len)
{
	while (len > 0U) {
		ssize_t n = zsock_send(sock, buf, len, 0);

		if (n <= 0) {
			return;
		}
		buf += n;
		len -= (size_t)n;
	}
}

static void send_response(int sock, const char *status, const char *type)
{
	char hdr[160];
	int n = snprintf(hdr, sizeof(hdr),
			 "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
			 "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
			 status, type, (unsigned int)page_len);

	send_all(sock, hdr, (size_t)n);
	send_all(sock, page, page_len);
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

static void render_status(void)
{
	struct stream_stats st;
	char addr[NET_IPV4_ADDR_LEN];
	char mac[18];
	char serial[33];
	char bits[HW_ID_BITS + 1];

	stream_get_stats(&st);
	net_config_get_address(addr, sizeof(addr));
	net_config_get_mac(mac, sizeof(mac));
	serial_string(serial, sizeof(serial));

	out("<h2>Status</h2><table>");
	out("<tr><td>Firmware</td><td>%s</td></tr>", APP_VERSION_EXTENDED_STRING);
	out("<tr><td>Serial</td><td>%s</td></tr>", serial);
	hw_id_bits_str(bits, sizeof(bits));
	out("<tr><td>Hardware</td><td>%s (HW_ID %s)</td></tr>", hw_id_name(), bits);
	out("<tr><td>Host link</td><td>%s</td></tr>", board_comm_mode_str(board_io_comm_mode()));
	out("<tr><td>Host session</td><td>%s</td></tr>",
	    control_session_active() ? "connected" : "none");
	out("<tr><td>Address</td><td>%s (%s)</td></tr>", addr[0] ? addr : "-",
	    net_config_get_mode());
	out("<tr><td>MAC</td><td>%s</td></tr>", mac);
	out("<tr><td>SX1255</td><td>%s</td></tr>", sx1255_is_ready() ? "ready" : "not ready");
	out("<tr><td>Streaming</td><td>%s%s, %u S/s</td></tr>", st.running ? "running" : "stopped",
	    st.tx_enabled ? ", TX on" : "", sx1255_get_sample_rate());
	out("<tr><td>PA</td><td>%s</td></tr>", board_pa_is_enabled() ? "on" : "off");
	out("<tr><td>Last reset</td><td>%s</td></tr>", watchdog_reset_cause());
	out("</table>");
}

static void render_form(const struct phenix_net_cfg *cfg, const char *msg, bool error)
{
	char ip[PHENIX_IPV4_STR_LEN], mask[PHENIX_IPV4_STR_LEN];
	char gw[PHENIX_IPV4_STR_LEN], dns[PHENIX_IPV4_STR_LEN];

	config_store_format_ipv4(cfg->ip, ip, sizeof(ip));
	config_store_format_ipv4(cfg->netmask, mask, sizeof(mask));
	config_store_format_ipv4(cfg->gateway, gw, sizeof(gw));
	config_store_format_ipv4(cfg->dns, dns, sizeof(dns));

	out("<h2>Network configuration</h2>");
	if (msg != NULL) {
		out("<p class=\"%s\">%s</p>", error ? "err" : "ok", msg);
	}
	out("<form method=\"post\" action=\"/config\">");
	out("<label>DHCP</label><select name=\"dhcp\">"
	    "<option value=\"on\"%s>on</option><option value=\"off\"%s>off</option>"
	    "</select><br>",
	    cfg->dhcp ? " selected" : "", cfg->dhcp ? "" : " selected");
	out("<p><small>Static settings are used when DHCP is off. Without a DHCP lease "
	    "within 30 s the device uses 192.168.2.10/24.</small></p>");
	out("<label>IP address</label><input name=\"ip\" value=\"%s\"><br>", ip);
	out("<label>Netmask</label><input name=\"netmask\" value=\"%s\"><br>", mask);
	out("<label>Gateway</label><input name=\"gateway\" value=\"%s\"><br>", gw);
	out("<label>DNS server</label><input name=\"dns\" value=\"%s\"><br>", dns);
	out("<label>Control port (TCP)</label><input name=\"ctrl_port\" value=\"%u\"><br>",
	    cfg->ctrl_port);
	out("<label>Sample port (UDP)</label><input name=\"sample_port\" value=\"%u\"><br>",
	    cfg->sample_port);
	out("<p><input type=\"submit\" value=\"Save and reboot\"></p></form>");
}

static void render_page(const struct phenix_net_cfg *cfg, const char *msg, bool error)
{
	page_len = 0;
	out("%s", page_head);
	render_status();
	render_form(cfg, msg, error);
	out("%s", page_tail);
}

static void render_json(void)
{
	struct stream_stats st;
	char addr[NET_IPV4_ADDR_LEN];
	char serial[33];

	stream_get_stats(&st);
	net_config_get_address(addr, sizeof(addr));
	serial_string(serial, sizeof(serial));

	page_len = 0;
	out("{\"firmware\":\"%s\",\"serial\":\"%s\",\"hardware\":\"%s\",\"hw_id\":%d,"
	    "\"host_link\":\"%s\","
	    "\"session\":%s,\"address\":\"%s\",\"address_mode\":\"%s\","
	    "\"sx1255_ready\":%s,\"streaming\":%s,\"tx_enabled\":%s,\"pa\":%s,"
	    "\"sample_rate\":%u,\"counter\":%llu,\"tx_late\":%u,\"tx_early\":%u,"
	    "\"fifo_errors\":%u,\"last_reset\":\"%s\"}\n",
	    APP_VERSION_EXTENDED_STRING, serial, hw_id_name(), hw_id_get(),
	    (board_io_comm_mode() == BOARD_COMM_USB) ? "usb" : "ethernet",
	    control_session_active() ? "true" : "false", addr, net_config_get_mode(),
	    sx1255_is_ready() ? "true" : "false", st.running ? "true" : "false",
	    st.tx_enabled ? "true" : "false", board_pa_is_enabled() ? "true" : "false",
	    sx1255_get_sample_rate(), st.counter, st.tx_late, st.tx_early,
	    st.rx_fifo_errors + st.tx_fifo_errors, watchdog_reset_cause());
}

/* Decode an application/x-www-form-urlencoded value in place */
static void url_decode(char *s)
{
	char *d = s;

	while (*s != '\0') {
		if (*s == '+') {
			*d++ = ' ';
			s++;
		} else if (*s == '%' && isxdigit((unsigned char)s[1]) &&
			   isxdigit((unsigned char)s[2])) {
			char hex[3] = {s[1], s[2], '\0'};

			*d++ = (char)strtoul(hex, NULL, 16);
			s += 3;
		} else {
			*d++ = *s++;
		}
	}
	*d = '\0';
}

static int parse_port(const char *s, uint16_t *port)
{
	char *end;
	unsigned long v = strtoul(s, &end, 10);

	if (*s == '\0' || *end != '\0' || v == 0UL || v > 65535UL) {
		return -EINVAL;
	}
	*port = (uint16_t)v;

	return 0;
}

/* Apply one form field; returns an error text or NULL */
static const char *apply_field(struct phenix_net_cfg *cfg, const char *key, const char *val)
{
	if (strcmp(key, "dhcp") == 0) {
		if (strcmp(val, "on") == 0) {
			cfg->dhcp = true;
		} else if (strcmp(val, "off") == 0) {
			cfg->dhcp = false;
		} else {
			return "Invalid DHCP setting";
		}
	} else if (strcmp(key, "ip") == 0) {
		if (config_store_parse_ipv4(val, cfg->ip) < 0) {
			return "Invalid IP address";
		}
	} else if (strcmp(key, "netmask") == 0) {
		if (config_store_parse_ipv4(val, cfg->netmask) < 0) {
			return "Invalid netmask";
		}
	} else if (strcmp(key, "gateway") == 0) {
		if (config_store_parse_ipv4(val[0] ? val : "0.0.0.0", cfg->gateway) < 0) {
			return "Invalid gateway";
		}
	} else if (strcmp(key, "dns") == 0) {
		if (config_store_parse_ipv4(val[0] ? val : "0.0.0.0", cfg->dns) < 0) {
			return "Invalid DNS server";
		}
	} else if (strcmp(key, "ctrl_port") == 0) {
		if (parse_port(val, &cfg->ctrl_port) < 0) {
			return "Invalid control port";
		}
	} else if (strcmp(key, "sample_port") == 0) {
		if (parse_port(val, &cfg->sample_port) < 0) {
			return "Invalid sample port";
		}
	}

	return NULL;
}

static const char *parse_form(char *body, struct phenix_net_cfg *cfg)
{
	char *save = NULL;

	for (char *pair = strtok_r(body, "&", &save); pair != NULL;
	     pair = strtok_r(NULL, "&", &save)) {
		char *eq = strchr(pair, '=');
		const char *err;

		if (eq == NULL) {
			continue;
		}
		*eq = '\0';
		url_decode(pair);
		url_decode(eq + 1);

		err = apply_field(cfg, pair, eq + 1);
		if (err != NULL) {
			return err;
		}
	}

	return config_store_validate(cfg);
}

static void reboot_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	LOG_WRN("Rebooting to apply the new configuration");
	control_reboot(false);
}

static K_WORK_DELAYABLE_DEFINE(reboot_work, reboot_work_handler);

static void handle_post_config(int sock, char *body)
{
	struct phenix_net_cfg cfg;
	const char *err;
	char msg[160];

	config_store_get_net(&cfg);
	err = parse_form(body, &cfg);
	if (err != NULL) {
		snprintf(msg, sizeof(msg), "Not saved: %s.", err);
		render_page(&cfg, msg, true);
		send_response(sock, "400 Bad Request", "text/html; charset=utf-8");
		return;
	}

	if (config_store_set_net(&cfg) < 0) {
		render_page(&cfg, "Saving failed.", true);
		send_response(sock, "500 Internal Server Error", "text/html; charset=utf-8");
		return;
	}

	if (cfg.dhcp) {
		snprintf(msg, sizeof(msg),
			 "Saved. Rebooting with DHCP; reconnect via "
			 "<a href=\"http://%s.local/\">%s.local</a>.",
			 PHX_MDNS_HOSTNAME, PHX_MDNS_HOSTNAME);
	} else {
		char ip[PHENIX_IPV4_STR_LEN];

		config_store_format_ipv4(cfg.ip, ip, sizeof(ip));
		snprintf(msg, sizeof(msg),
			 "Saved. Rebooting; reconnect via <a href=\"http://%s/\">%s</a>.", ip, ip);
	}

	page_len = 0;
	out("%s<p class=\"ok\">%s</p>%s", page_head, msg, page_tail);
	send_response(sock, "200 OK", "text/html; charset=utf-8");

	LOG_INF("Configuration saved via web interface");
	k_work_reschedule(&reboot_work, REBOOT_DELAY);
}

/* Read the request head and, if present, the body. Returns body or NULL. */
static int read_request(int sock, char **body_out, size_t *body_len)
{
	size_t len = 0;
	char *end = NULL;
	const char *line;
	size_t content_length = 0;
	size_t head_len;

	while (end == NULL) {
		ssize_t n;

		if (len >= REQ_MAX) {
			return -E2BIG;
		}
		n = zsock_recv(sock, &req[len], REQ_MAX - len, 0);
		if (n <= 0) {
			return -EIO;
		}
		len += (size_t)n;
		req[len] = '\0';
		end = strstr(req, "\r\n\r\n");
	}

	head_len = (size_t)(end - req) + 4U;

	/* Content-Length header (name is case-insensitive), after the request line */
	for (line = strstr(req, "\r\n"); line != NULL && line < end;
	     line = strstr(line, "\r\n")) {
		line += 2;
		if (strncasecmp(line, "Content-Length:", 15) == 0) {
			content_length = strtoul(line + 15, NULL, 10);
			break;
		}
	}

	if (content_length > BODY_MAX) {
		return -E2BIG;
	}

	while (len < head_len + content_length) {
		ssize_t n = zsock_recv(sock, &req[len], head_len + content_length - len, 0);

		if (n <= 0) {
			return -EIO;
		}
		len += (size_t)n;
	}
	req[head_len + content_length] = '\0';

	*body_out = &req[head_len];
	*body_len = content_length;

	return 0;
}

static void handle_client(int sock)
{
	struct phenix_net_cfg cfg;
	char *body;
	size_t body_len;

	if (read_request(sock, &body, &body_len) < 0) {
		page_len = 0;
		out("Bad request\n");
		send_response(sock, "400 Bad Request", "text/plain");
		return;
	}

	if (strncmp(req, "GET / ", 6) == 0 || strncmp(req, "GET /index.html ", 16) == 0) {
		config_store_get_net(&cfg);
		render_page(&cfg, NULL, false);
		send_response(sock, "200 OK", "text/html; charset=utf-8");
	} else if (strncmp(req, "GET /status.json ", 17) == 0) {
		render_json();
		send_response(sock, "200 OK", "application/json");
	} else if (strncmp(req, "POST /config ", 13) == 0) {
		handle_post_config(sock, body);
	} else {
		page_len = 0;
		out("Not found\n");
		send_response(sock, "404 Not Found", "text/plain");
	}
}

static void web_thread(void *p1, void *p2, void *p3)
{
	struct net_sockaddr_in addr = {
		.sin_family = NET_AF_INET,
		.sin_port = net_htons(PHENIX_HTTP_PORT),
		.sin_addr = {.s_addr = NET_INADDR_ANY},
	};
	int listen_sock;
	int one = 1;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	listen_sock = zsock_socket(NET_AF_INET, NET_SOCK_STREAM, NET_IPPROTO_TCP);
	if (listen_sock < 0) {
		LOG_ERR("HTTP socket failed (%d)", errno);
		return;
	}
	zsock_setsockopt(listen_sock, ZSOCK_SOL_SOCKET, ZSOCK_SO_REUSEADDR, &one, sizeof(one));

	if (zsock_bind(listen_sock, (struct net_sockaddr *)&addr, sizeof(addr)) < 0 ||
	    zsock_listen(listen_sock, 2) < 0) {
		LOG_ERR("HTTP bind/listen failed (%d)", errno);
		zsock_close(listen_sock);
		return;
	}
	LOG_INF("Web interface on TCP port %u", PHENIX_HTTP_PORT);

	while (true) {
		struct zsock_timeval tv = {.tv_sec = RECV_TIMEOUT};
		int sock = zsock_accept(listen_sock, NULL, NULL);

		if (sock < 0) {
			k_msleep(100);
			continue;
		}

		zsock_setsockopt(sock, ZSOCK_SOL_SOCKET, ZSOCK_SO_RCVTIMEO, &tv, sizeof(tv));
		handle_client(sock);
		zsock_close(sock);
	}
}

K_THREAD_DEFINE(web_tid, WEB_STACK_SIZE, web_thread, NULL, NULL, NULL, WEB_PRIORITY, 0,
		K_TICKS_FOREVER);

int web_ui_start(void)
{
	k_thread_start(web_tid);

	return 0;
}
