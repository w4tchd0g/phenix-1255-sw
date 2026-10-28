/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Phenix-1255 contributors
 *
 * Diagnostic shell commands on the MCU-Link console.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include "board/board_io.h"
#include "board/hw_id.h"
#include "board/watchdog.h"
#include "config/config_store.h"
#include "display/display_fb.h"
#include "display/display_ui.h"
#include "radio/sx1255.h"
#include "radio/sx1255_regs.h"
#include "stream/sample_io.h"
#include "stream/stream.h"

static void print_net_cfg(const struct shell *sh, const struct phenix_net_cfg *cfg)
{
	char buf[PHENIX_IPV4_STR_LEN];

	shell_print(sh, "dhcp:        %s", cfg->dhcp ? "on" : "off");
	config_store_format_ipv4(cfg->ip, buf, sizeof(buf));
	shell_print(sh, "ip:          %s", buf);
	config_store_format_ipv4(cfg->netmask, buf, sizeof(buf));
	shell_print(sh, "netmask:     %s", buf);
	config_store_format_ipv4(cfg->gateway, buf, sizeof(buf));
	shell_print(sh, "gateway:     %s", buf);
	config_store_format_ipv4(cfg->dns, buf, sizeof(buf));
	shell_print(sh, "dns:         %s", buf);
	shell_print(sh, "ctrl_port:   %u (TCP)", cfg->ctrl_port);
	shell_print(sh, "sample_port: %u (UDP)", cfg->sample_port);
}

/*
 * On unsupported hardware the start-up is halted: the SX1255 and the configuration
 * store are not initialized, and the board must not be written to.
 */
static int check_hw(const struct shell *sh)
{
	if (!hw_id_is_supported()) {
		shell_error(sh, "Unsupported hardware, start-up halted (see 'phenix status')");
		return -ENODEV;
	}

	return 0;
}

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	char bits[HW_ID_BITS + 1];

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	hw_id_bits_str(bits, sizeof(bits));
	shell_print(sh, "hardware:    %s (HW_ID2..0 = %s)", hw_id_name(), bits);
	if (!hw_id_is_supported()) {
		shell_print(sh, "start-up:    halted");
		return 0;
	}

	int level = board_io_comm_sel_level();

	shell_print(sh, "comm_sel:    %s (pin now: %s)", board_comm_mode_str(board_io_comm_mode()),
		    (level < 0) ? "error" : (level ? "high" : "low"));
	shell_print(sh, "pa:          %s", board_pa_is_enabled() ? "enabled" : "disabled");

	return 0;
}

static int cmd_config_show(const struct shell *sh, size_t argc, char **argv)
{
	struct phenix_net_cfg cfg;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	config_store_get_net(&cfg);
	print_net_cfg(sh, &cfg);

	return 0;
}

static int parse_port(const char *str, uint16_t *port)
{
	char *end;
	unsigned long val = strtoul(str, &end, 10);

	if (*str == '\0' || *end != '\0' || val == 0UL || val > 65535UL) {
		return -EINVAL;
	}
	*port = (uint16_t)val;

	return 0;
}

static int cmd_config_set(const struct shell *sh, size_t argc, char **argv)
{
	struct phenix_net_cfg cfg;
	const char *key = argv[1];
	const char *val = argv[2];
	const char *reason;
	int ret = 0;

	ARG_UNUSED(argc);

	ret = check_hw(sh);
	if (ret < 0) {
		return ret;
	}

	config_store_get_net(&cfg);

	if (strcmp(key, "dhcp") == 0) {
		if (strcmp(val, "on") == 0) {
			cfg.dhcp = true;
		} else if (strcmp(val, "off") == 0) {
			cfg.dhcp = false;
		} else {
			ret = -EINVAL;
		}
	} else if (strcmp(key, "ip") == 0) {
		ret = config_store_parse_ipv4(val, cfg.ip);
	} else if (strcmp(key, "netmask") == 0) {
		ret = config_store_parse_ipv4(val, cfg.netmask);
	} else if (strcmp(key, "gateway") == 0) {
		ret = config_store_parse_ipv4(val, cfg.gateway);
	} else if (strcmp(key, "dns") == 0) {
		ret = config_store_parse_ipv4(val, cfg.dns);
	} else if (strcmp(key, "ctrl_port") == 0) {
		ret = parse_port(val, &cfg.ctrl_port);
	} else if (strcmp(key, "sample_port") == 0) {
		ret = parse_port(val, &cfg.sample_port);
	} else {
		shell_error(sh, "Unknown key '%s'", key);
		return -EINVAL;
	}

	if (ret < 0) {
		shell_error(sh, "Invalid value '%s' for %s", val, key);
		return ret;
	}

	reason = config_store_validate(&cfg);
	if (reason != NULL) {
		shell_error(sh, "Rejected: %s", reason);
		return -EINVAL;
	}

	ret = config_store_set_net(&cfg);
	if (ret < 0) {
		shell_error(sh, "Saving failed (%d)", ret);
		return ret;
	}

	shell_print(sh, "Saved. Takes effect after reboot.");

	return 0;
}

static int cmd_led(const struct shell *sh, size_t argc, char **argv)
{
	static const char *const names[BOARD_LED_COUNT] = {"err", "warn", "ok"};
	static const char *const modes[] = {"off", "on", "slow", "fast"};
	int led = -1;
	int mode = -1;

	ARG_UNUSED(argc);

	for (int i = 0; i < BOARD_LED_COUNT; i++) {
		if (strcmp(argv[1], names[i]) == 0) {
			led = i;
		}
	}
	for (int i = 0; i < ARRAY_SIZE(modes); i++) {
		if (strcmp(argv[2], modes[i]) == 0) {
			mode = i;
		}
	}

	if (led < 0 || mode < 0) {
		shell_error(sh, "Usage: phenix led <err|warn|ok> <off|on|slow|fast>");
		return -EINVAL;
	}

	board_led_set((enum board_led)led, (enum board_led_mode)mode);

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_config,
	SHELL_CMD(show, NULL, "Show stored network configuration", cmd_config_show),
	SHELL_CMD_ARG(set, NULL,
		      "Set and save a value: <dhcp|ip|netmask|gateway|dns|ctrl_port|"
		      "sample_port> <value>",
		      cmd_config_set, 3, 0),
	SHELL_SUBCMD_SET_END);

static int cmd_wdt_test(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_warn(sh, "Watchdog feeding stopped, reset in %d ms", WATCHDOG_TIMEOUT_MS);
	watchdog_stop_feeding();

	return 0;
}

static int cmd_display_status(const struct shell *sh, size_t argc, char **argv)
{
	struct display_ui_info info;
	struct phenix_display_cfg cfg;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	display_ui_get_info(&info);
	config_store_get_display(&cfg);

	shell_print(sh, "display:     %s", info.ready ? "ready" : "not available");
	shell_print(sh, "screen:      %s (page %d)", info.screen, info.page + 1);
	shell_print(sh, "brightness:  %u/%u%s", cfg.contrast, PHENIX_CONTRAST_MAX,
		    info.dimmed ? " (dimmed)" : "");
	shell_print(sh, "dim after:   %u s", cfg.dim_timeout_s);
	shell_print(sh, "auto pages:  %s", cfg.auto_pages ? "on" : "off");

	return 0;
}

static int cmd_display_key(const struct shell *sh, size_t argc, char **argv)
{
	static const char *const keys[] = {
		[DISPLAY_KEY_PREV] = "prev",
		[DISPLAY_KEY_NEXT] = "next",
		[DISPLAY_KEY_SELECT] = "select",
		[DISPLAY_KEY_BACK] = "back",
	};

	ARG_UNUSED(argc);

	for (int i = 0; i < ARRAY_SIZE(keys); i++) {
		if (strcmp(argv[1], keys[i]) == 0) {
			display_ui_inject((enum display_ui_key)i);
			return 0;
		}
	}

	shell_error(sh, "Usage: phenix display key <prev|next|select|back>");

	return -EINVAL;
}

static int cmd_display_test(const struct shell *sh, size_t argc, char **argv)
{
	int seconds = (argc > 1) ? atoi(argv[1]) : 10;

	ARG_UNUSED(sh);

	display_ui_test_pattern(seconds);

	return 0;
}

static int cmd_display_dump(const struct shell *sh, size_t argc, char **argv)
{
	/* Two pixel rows per character: none, upper, lower, both */
	static const char *const cells[] = {" ", "\u2580", "\u2584", "\u2588"};
	static uint8_t frame[FB_SIZE];
	char text[FB_WIDTH * 3 + 1];

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (!display_ui_snapshot(frame, sizeof(frame))) {
		shell_error(sh, "No frame sent yet");
		return -ENODATA;
	}

	for (int y = 0; y < FB_HEIGHT; y += 2) {
		size_t pos = 0;

		for (int x = 0; x < FB_WIDTH; x++) {
			const uint8_t *col = &frame[(y / 8) * FB_WIDTH + x];
			int cell = ((*col >> (y % 8)) & 1) | (((*col >> (y % 8 + 1)) & 1) << 1);

			pos += strlen(strcpy(&text[pos], cells[cell]));
		}
		shell_print(sh, "|%s|", text);
	}

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_display,
	SHELL_CMD(status, NULL, "Show display state and settings", cmd_display_status),
	SHELL_CMD_ARG(key, NULL, "Simulate an input: <prev|next|select|back>", cmd_display_key,
		      2, 0),
	SHELL_CMD_ARG(test, NULL, "Show the colour zone test pattern: [seconds]",
		      cmd_display_test, 1, 1),
	SHELL_CMD(dump, NULL, "Print the current display content", cmd_display_dump),
	SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_phenix,
	SHELL_CMD(wdt_test, NULL, "Stop feeding the watchdog (provokes a reset)", cmd_wdt_test),
	SHELL_CMD(status, NULL, "Show device status", cmd_status),
	SHELL_CMD(config, &sub_config, "Persistent configuration", NULL),
	SHELL_CMD(display, &sub_display, "Front panel display", cmd_display_status),
	SHELL_CMD_ARG(led, NULL, "Set LED mode: <err|warn|ok> <off|on|slow|fast>", cmd_led, 3,
		      0),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(phenix, &sub_phenix, "Phenix-1255 commands", NULL);

/* SX1255 diagnostics */

static int parse_u32(const char *str, uint32_t *out)
{
	char *end;
	unsigned long val = strtoul(str, &end, 0);

	if (*str == '\0' || *end != '\0') {
		return -EINVAL;
	}
	*out = (uint32_t)val;

	return 0;
}

static int check_ready(const struct shell *sh)
{
	int ret = check_hw(sh);

	if (ret < 0) {
		return ret;
	}

	if (!sx1255_is_ready()) {
		shell_error(sh, "SX1255 not initialized (use 'sx1255 reset')");
		return -ENODEV;
	}

	return 0;
}

static int cmd_sx_info(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t version = 0;
	uint8_t lock = 0;
	bool rx, tx, drv;
	int ret;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	ret = check_ready(sh);
	if (ret < 0) {
		return ret;
	}

	sx1255_read_regs(SX1255_REG_VERSION, &version, 1);
	sx1255_get_pll_lock(&lock);
	sx1255_get_mode(&rx, &tx, &drv);

	shell_print(sh, "version:     0x%02x", version);
	shell_print(sh, "reference:   %u Hz", sx1255_xtal_hz());
	shell_print(sh, "mode:        rx=%s tx=%s tx_driver=%s", rx ? "on" : "off",
		    tx ? "on" : "off", drv ? "on" : "off");
	shell_print(sh, "pll_lock:    rx=%s tx=%s",
		    (lock & SX1255_STAT_PLL_LOCK_RX) ? "locked" : "no",
		    (lock & SX1255_STAT_PLL_LOCK_TX) ? "locked" : "no");
	shell_print(sh, "rx_freq:     %u Hz", sx1255_get_frequency(SX1255_RX));
	shell_print(sh, "tx_freq:     %u Hz", sx1255_get_frequency(SX1255_TX));
	shell_print(sh, "sample_rate: %u S/s (I2S bit clock %u Hz)", sx1255_get_sample_rate(),
		    sx1255_bit_clock(sx1255_get_sample_rate()));
	for (int g = 0; g < SX1255_GAIN_COUNT; g++) {
		shell_print(sh, "gain %-6s %d dB", sx1255_gain_name(g), sx1255_get_gain(g));
	}
	shell_print(sh, "rx_input:    %s",
		    (sx1255_get_rx_input() == SX1255_RX_INPUT_LOOPBACK) ? "loopback" : "antenna");
	shell_print(sh, "dio:         dio0=%d dio1=%d dio3=%d", sx1255_get_dio(0),
		    sx1255_get_dio(1), sx1255_get_dio(3));

	return 0;
}

static int cmd_sx_reset(const struct shell *sh, size_t argc, char **argv)
{
	int ret;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	ret = check_hw(sh);
	if (ret < 0) {
		return ret;
	}

	ret = sx1255_init();
	if (ret < 0) {
		shell_error(sh, "Init failed (%d)", ret);
	}

	return ret;
}

static int cmd_sx_dump(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t buf[SX1255_NUM_REGS];
	int ret;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	ret = check_hw(sh);
	if (ret < 0) {
		return ret;
	}

	ret = sx1255_read_regs(0, buf, sizeof(buf));
	if (ret < 0) {
		shell_error(sh, "Read failed (%d)", ret);
		return ret;
	}
	shell_hexdump(sh, buf, sizeof(buf));

	return 0;
}

static int cmd_sx_reg_read(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t buf[16];
	uint32_t addr;
	uint32_t len = 1;
	int ret;

	ret = check_hw(sh);
	if (ret < 0) {
		return ret;
	}

	if (parse_u32(argv[1], &addr) < 0 || addr > SX1255_ADDR_MASK ||
	    (argc > 2 && (parse_u32(argv[2], &len) < 0 || len == 0U || len > sizeof(buf)))) {
		shell_error(sh, "Usage: sx1255 reg read <addr> [count <= %u]", sizeof(buf));
		return -EINVAL;
	}

	ret = sx1255_read_regs((uint8_t)addr, buf, len);
	if (ret < 0) {
		shell_error(sh, "Read failed (%d)", ret);
		return ret;
	}
	for (uint32_t i = 0; i < len; i++) {
		shell_print(sh, "0x%02x: 0x%02x", addr + i, buf[i]);
	}

	return 0;
}

static int cmd_sx_reg_write(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t addr;
	uint32_t val;
	uint8_t byte;
	int ret;

	ARG_UNUSED(argc);

	ret = check_hw(sh);
	if (ret < 0) {
		return ret;
	}

	if (parse_u32(argv[1], &addr) < 0 || addr > SX1255_ADDR_MASK ||
	    parse_u32(argv[2], &val) < 0 || val > 0xFFU) {
		shell_error(sh, "Usage: sx1255 reg write <addr> <value>");
		return -EINVAL;
	}

	byte = (uint8_t)val;
	ret = sx1255_write_regs((uint8_t)addr, &byte, 1);
	if (ret < 0) {
		shell_error(sh, "Write failed (%d)", ret);
	}

	return ret;
}

static int cmd_sx_mode(const struct shell *sh, size_t argc, char **argv)
{
	static const struct {
		const char *name;
		bool rx, tx, drv;
	} modes[] = {
		{"off", false, false, false},
		{"rx", true, false, false},
		{"tx", false, true, false},
		{"rxtx", true, true, false},
	};
	int ret;

	ARG_UNUSED(argc);

	ret = check_ready(sh);
	if (ret < 0) {
		return ret;
	}

	for (int i = 0; i < ARRAY_SIZE(modes); i++) {
		if (strcmp(argv[1], modes[i].name) == 0) {
			bool rx, tx, drv;

			/* Keep the TX driver state, it is controlled separately */
			sx1255_get_mode(&rx, &tx, &drv);
			return sx1255_set_mode(modes[i].rx, modes[i].tx, drv && modes[i].tx);
		}
	}

	shell_error(sh, "Usage: sx1255 mode <off|rx|tx|rxtx>");
	return -EINVAL;
}

static int cmd_sx_txdrv(const struct shell *sh, size_t argc, char **argv)
{
	bool rx, tx, drv;
	int ret;

	ARG_UNUSED(argc);

	ret = check_ready(sh);
	if (ret < 0) {
		return ret;
	}

	sx1255_get_mode(&rx, &tx, &drv);

	if (strcmp(argv[1], "on") == 0) {
		if (!tx) {
			shell_error(sh, "Transmitter is off (use 'sx1255 mode tx|rxtx')");
			return -EINVAL;
		}
		shell_warn(sh, "TX driver on: RF is emitted at the TX frequency");
		return sx1255_set_mode(rx, tx, true);
	} else if (strcmp(argv[1], "off") == 0) {
		return sx1255_set_mode(rx, tx, false);
	}

	shell_error(sh, "Usage: sx1255 txdrv <on|off>");
	return -EINVAL;
}

static int cmd_sx_freq(const struct shell *sh, size_t argc, char **argv)
{
	enum sx1255_dir dir;
	uint32_t hz;
	uint32_t actual;
	int ret;

	ARG_UNUSED(argc);

	ret = check_ready(sh);
	if (ret < 0) {
		return ret;
	}

	if (strcmp(argv[1], "rx") == 0) {
		dir = SX1255_RX;
	} else if (strcmp(argv[1], "tx") == 0) {
		dir = SX1255_TX;
	} else {
		shell_error(sh, "Usage: sx1255 freq <rx|tx> <hz>");
		return -EINVAL;
	}

	if (parse_u32(argv[2], &hz) < 0) {
		shell_error(sh, "Invalid frequency '%s'", argv[2]);
		return -EINVAL;
	}

	ret = sx1255_set_frequency(dir, hz, &actual);
	if (ret == -ERANGE) {
		shell_error(sh, "Out of range (%u..%u Hz)", SX1255_FREQ_MIN_HZ,
			    SX1255_FREQ_MAX_HZ);
		return ret;
	} else if (ret < 0) {
		shell_error(sh, "Failed (%d)", ret);
		return ret;
	}

	shell_print(sh, "%s frequency set to %u Hz", argv[1], actual);

	return 0;
}

static int cmd_sx_rate(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t rate;
	int ret;

	ARG_UNUSED(argc);

	ret = check_ready(sh);
	if (ret < 0) {
		return ret;
	}

	if (stream_is_running()) {
		shell_error(sh, "Stop the stream before changing the sample rate");
		return -EBUSY;
	}

	if (parse_u32(argv[1], &rate) < 0 || sx1255_set_sample_rate(rate) < 0) {
		shell_error(sh, "Unsupported sample rate. Supported:");
		for (int i = 0; i < SX1255_NUM_SAMPLE_RATES; i++) {
			shell_error(sh, "  %u", sx1255_sample_rate(i));
		}
		return -EINVAL;
	}

	shell_print(sh, "Sample rate %u S/s, I2S bit clock %u Hz", rate, sx1255_bit_clock(rate));

	return 0;
}

static int cmd_sx_gain(const struct shell *sh, size_t argc, char **argv)
{
	char *end;
	long db;
	int actual;
	int ret;

	ARG_UNUSED(argc);

	ret = check_ready(sh);
	if (ret < 0) {
		return ret;
	}

	db = strtol(argv[2], &end, 10);
	if (*argv[2] == '\0' || *end != '\0') {
		shell_error(sh, "Invalid gain '%s'", argv[2]);
		return -EINVAL;
	}

	for (int g = 0; g < SX1255_GAIN_COUNT; g++) {
		if (strcasecmp(argv[1], sx1255_gain_name(g)) == 0) {
			ret = sx1255_set_gain(g, (int)db, &actual);
			if (ret < 0) {
				shell_error(sh, "Failed (%d)", ret);
				return ret;
			}
			shell_print(sh, "%s gain set to %d dB", sx1255_gain_name(g), actual);
			return 0;
		}
	}

	shell_error(sh, "Usage: sx1255 gain <lna|pga|dac|mixer> <dB>");
	return -EINVAL;
}

static int cmd_sx_rxin(const struct shell *sh, size_t argc, char **argv)
{
	int ret;

	ARG_UNUSED(argc);

	ret = check_ready(sh);
	if (ret < 0) {
		return ret;
	}

	if (strcmp(argv[1], "antenna") == 0) {
		return sx1255_set_rx_input(SX1255_RX_INPUT_ANTENNA);
	} else if (strcmp(argv[1], "loopback") == 0) {
		return sx1255_set_rx_input(SX1255_RX_INPUT_LOOPBACK);
	}

	shell_error(sh, "Usage: sx1255 rxin <antenna|loopback>");
	return -EINVAL;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_sx_reg,
	SHELL_CMD_ARG(read, NULL, "Read registers: <addr> [count]", cmd_sx_reg_read, 2, 1),
	SHELL_CMD_ARG(write, NULL, "Write register: <addr> <value>", cmd_sx_reg_write, 3, 0),
	SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_sx,
	SHELL_CMD(info, NULL, "Show chip configuration and status", cmd_sx_info),
	SHELL_CMD(reset, NULL, "Reset and re-initialize the chip", cmd_sx_reset),
	SHELL_CMD(dump, NULL, "Dump registers 0x00..0x13", cmd_sx_dump),
	SHELL_CMD(reg, &sub_sx_reg, "Raw register access", NULL),
	SHELL_CMD_ARG(mode, NULL, "Enable blocks: <off|rx|tx|rxtx>", cmd_sx_mode, 2, 0),
	SHELL_CMD_ARG(txdrv, NULL, "TX driver output: <on|off>", cmd_sx_txdrv, 2, 0),
	SHELL_CMD_ARG(freq, NULL, "Set frequency: <rx|tx> <hz>", cmd_sx_freq, 3, 0),
	SHELL_CMD_ARG(rate, NULL, "Set sample rate: <S/s>", cmd_sx_rate, 2, 0),
	SHELL_CMD_ARG(gain, NULL, "Set gain: <lna|pga|dac|mixer> <dB>", cmd_sx_gain, 3, 0),
	SHELL_CMD_ARG(rxin, NULL, "RX input: <antenna|loopback>", cmd_sx_rxin, 2, 0),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(sx1255, &sub_sx, "SX1255 diagnostics", NULL);

/* Sample stream diagnostics */

#define RXSTATS_MAX_SAMPLES (STREAM_RX_RING_SAMPLES / 2U)
#define FULL_SCALE          2147483648.0f
#define TWO_PI_F            6.28318530717958647692f

static struct stream_iq rxstats_buf[RXSTATS_MAX_SAMPLES];

static int cmd_stream_start(const struct shell *sh, size_t argc, char **argv)
{
	int ret;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	ret = stream_start();
	if (ret < 0) {
		shell_error(sh, "Start failed (%d)", ret);
	}

	return ret;
}

static int cmd_stream_stop(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(sh);
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	return stream_stop();
}

static int cmd_stream_status(const struct shell *sh, size_t argc, char **argv)
{
	struct stream_stats st;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	stream_get_stats(&st);
	shell_print(sh, "running:        %s", st.running ? "yes" : "no");
	shell_print(sh, "rate:           %u S/s", st.rate);
	shell_print(sh, "counter:        %llu", st.counter);
	shell_print(sh, "tx_block:       %u samples", st.tx_block_samples);
	shell_print(sh, "rx_fifo_errors: %u", st.rx_fifo_errors);
	shell_print(sh, "tx_fifo_errors: %u", st.tx_fifo_errors);
	shell_print(sh, "sync_errors:    rx %u, tx %u", st.rx_sync_errors, st.tx_sync_errors);
	shell_print(sh, "tx_refill_late: %u", st.tx_refill_late);
	shell_print(sh, "tx_blocks:      %llu", st.tx_blocks);
	shell_print(sh, "tx_late/early:  %u / %u", st.tx_late, st.tx_early);

	struct sample_io_stats io;

	sample_io_get_stats(&io);
	shell_print(sh, "rx_packets:     %u (dropped samples %u)", io.rx_packets, io.rx_dropped);
	shell_print(sh, "tx_packets:     %u (sequence gaps %u, invalid %u)", io.tx_packets,
		    io.tx_seq_gaps, io.tx_invalid);

	return 0;
}

static int cmd_stream_regs(const struct shell *sh, size_t argc, char **argv)
{
	struct stream_sai_regs r;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	stream_get_sai_regs(&r);
	shell_print(sh, "TCSR %08x TCR2 %08x TCR3 %08x TCR4 %08x TCR5 %08x TFR0 %08x", r.tcsr,
		    r.tcr2, r.tcr3, r.tcr4, r.tcr5, r.tfr0);
	shell_print(sh, "RCSR %08x RCR2 %08x RCR3 %08x RCR4 %08x RCR5 %08x RFR0 %08x", r.rcsr,
		    r.rcr2, r.rcr3, r.rcr4, r.rcr5, r.rfr0);

	return 0;
}

/* Measure the counter rate against the CPU cycle counter */
static int cmd_stream_rate(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t ms = 1000U;
	uint64_t c0, c1, t0, t1;
	double measured;
	double nominal;

	if (argc > 1 && (parse_u32(argv[1], &ms) < 0 || ms == 0U || ms > 60000U)) {
		shell_error(sh, "Usage: stream rate [ms]");
		return -EINVAL;
	}
	if (!stream_is_running()) {
		shell_error(sh, "Stream not running");
		return -EAGAIN;
	}

	c0 = stream_counter();
	t0 = k_cycle_get_64();
	k_msleep(ms);
	c1 = stream_counter();
	t1 = k_cycle_get_64();

	measured = (double)(c1 - c0) * (double)sys_clock_hw_cycles_per_sec() / (double)(t1 - t0);
	nominal = (double)sx1255_get_sample_rate();
	shell_print(sh, "samples %llu in %llu cycles: %.1f S/s (%+.0f ppm vs CPU clock)", c1 - c0,
		    t1 - t0, measured, (measured / nominal - 1.0) * 1e6);

	return 0;
}

/*
 * Read the counter in a tight loop and check that it never decreases and
 * never advances by more than a DMA request per read beyond the elapsed
 * time. Detects races between the DMA position and the half/major interrupts.
 */
static int cmd_stream_check(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t ms = 1000U;
	uint64_t prev, now, max_step = 0, reads = 0, backwards = 0;
	int64_t end;

	if (argc > 1 && (parse_u32(argv[1], &ms) < 0 || ms == 0U || ms > 10000U)) {
		shell_error(sh, "Usage: stream check [ms]");
		return -EINVAL;
	}
	if (!stream_is_running()) {
		shell_error(sh, "Stream not running");
		return -EAGAIN;
	}

	prev = stream_counter();
	end = k_uptime_get() + ms;
	while (k_uptime_get() < end) {
		now = stream_counter();
		reads++;
		if (now < prev) {
			backwards++;
		} else {
			max_step = MAX(max_step, now - prev);
		}
		prev = now;
	}

	shell_print(sh, "%llu reads, %llu backwards, largest step %llu samples", reads, backwards,
		    max_step);

	return 0;
}

static int cmd_stream_rxstats(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t n = RXSTATS_MAX_SAMPLES;
	uint64_t first;
	float sum_i = 0, sum_q = 0, pow_i = 0, pow_q = 0;
	float acc_re = 0, acc_im = 0;
	float corr_re = 0, corr_im = 0;
	float peak = 0;
	float fs = (float)sx1255_get_sample_rate();
	float w, freq, total, tone;
	int ret;

	long probe_hz = 0;
	char *end;

	if (argc > 1 && (parse_u32(argv[1], &n) < 0 || n < 16U || n > RXSTATS_MAX_SAMPLES)) {
		shell_error(sh, "Usage: stream rxstats [16..%u] [probe_hz]", RXSTATS_MAX_SAMPLES);
		return -EINVAL;
	}
	if (argc > 2) {
		probe_hz = strtol(argv[2], &end, 10);
		if (*argv[2] == '\0' || *end != '\0') {
			shell_error(sh, "Invalid probe frequency '%s'", argv[2]);
			return -EINVAL;
		}
	}

	ret = stream_rx_snapshot(rxstats_buf, n, &first);
	if (ret < 0) {
		shell_error(sh, "Snapshot failed (%d)", ret);
		return ret;
	}

	for (uint32_t k = 0; k < n; k++) {
		float i = (float)rxstats_buf[k].i / FULL_SCALE;
		float q = (float)rxstats_buf[k].q / FULL_SCALE;

		sum_i += i;
		sum_q += q;
		pow_i += i * i;
		pow_q += q * q;
		peak = MAX(peak, MAX(fabsf(i), fabsf(q)));
	}

	float mean_i = sum_i / (float)n;
	float mean_q = sum_q / (float)n;

	/*
	 * Dominant frequency from the mean phase increment of the signal with
	 * the DC component (LO leakage) removed; DC would bias it towards 0 Hz.
	 */
	for (uint32_t k = 1; k < n; k++) {
		float i = (float)rxstats_buf[k].i / FULL_SCALE - mean_i;
		float q = (float)rxstats_buf[k].q / FULL_SCALE - mean_q;
		float pi = (float)rxstats_buf[k - 1].i / FULL_SCALE - mean_i;
		float pq = (float)rxstats_buf[k - 1].q / FULL_SCALE - mean_q;

		/* z[k] * conj(z[k-1]) */
		acc_re += i * pi + q * pq;
		acc_im += q * pi - i * pq;
	}
	/*
	 * The phase increment estimate is biased by small spurs, which is
	 * significant over a long window. Refine it by maximizing the tone
	 * correlation within +-3 DFT bins in steps of 0.05 bins.
	 */
	float w_est = atan2f(acc_im, acc_re);
	float bin = TWO_PI_F / (float)n;

	tone = -1.0f;
	w = w_est;
	for (int d = -60; d <= 60; d++) {
		float wt = w_est + (float)d * 0.05f * bin;
		/* Rotating phasor exp(-j wt k) */
		float rot_c = cosf(wt), rot_s = sinf(wt);
		float c = 1.0f, s = 0.0f;
		float p;

		corr_re = 0;
		corr_im = 0;
		for (uint32_t k = 0; k < n; k++) {
			float i = (float)rxstats_buf[k].i / FULL_SCALE - mean_i;
			float q = (float)rxstats_buf[k].q / FULL_SCALE - mean_q;
			float nc = c * rot_c - s * rot_s;

			/* z[k] * exp(-j wt k) */
			corr_re += i * c + q * s;
			corr_im += q * c - i * s;
			s = s * rot_c + c * rot_s;
			c = nc;
		}
		p = (corr_re * corr_re + corr_im * corr_im) / ((float)n * (float)n);
		if (p > tone) {
			tone = p;
			w = wt;
		}
	}
	freq = w * fs / TWO_PI_F;

	total = (pow_i + pow_q) / (float)n - (mean_i * mean_i + mean_q * mean_q);

	shell_print(sh, "samples:     %u from index %llu", n, first);
	shell_print(sh, "mean:        I %+.5f  Q %+.5f", (double)mean_i, (double)mean_q);
	shell_print(sh, "rms:         I %.5f  Q %.5f (%.1f dBFS)", (double)sqrtf(pow_i / n),
		    (double)sqrtf(pow_q / n), (double)(10.0f * log10f(total + 1e-20f)));
	shell_print(sh, "peak:        %.5f", (double)peak);
	shell_print(sh, "tone:        %+.1f Hz, %.1f dB of AC power", (double)freq,
		    (double)(10.0f * log10f(tone / (total + 1e-20f) + 1e-20f)));
	/* Power at +probe and -probe (signal and its mirror image) */
	if (probe_hz != 0) {
		for (int sign = 1; sign >= -1; sign -= 2) {
			float wp = TWO_PI_F * (float)(sign * probe_hz) / fs;
			float re = 0, im = 0;

			for (uint32_t k = 0; k < n; k++) {
				float i = (float)rxstats_buf[k].i / FULL_SCALE - mean_i;
				float q = (float)rxstats_buf[k].q / FULL_SCALE - mean_q;
				float c = cosf(wp * (float)k);
				float s = sinf(wp * (float)k);

				re += i * c + q * s;
				im += q * c - i * s;
			}
			shell_print(sh, "at %+ld Hz:  %.1f dB of AC power", sign * probe_hz,
				    (double)(10.0f * log10f((re * re + im * im) / ((float)n * (float)n) /
							    (total + 1e-20f) + 1e-20f)));
		}
	}

	shell_print(sh, "raw[0..3]:   %08x %08x  %08x %08x  %08x %08x  %08x %08x",
		    (uint32_t)rxstats_buf[0].i, (uint32_t)rxstats_buf[0].q,
		    (uint32_t)rxstats_buf[1].i, (uint32_t)rxstats_buf[1].q,
		    (uint32_t)rxstats_buf[2].i, (uint32_t)rxstats_buf[2].q,
		    (uint32_t)rxstats_buf[3].i, (uint32_t)rxstats_buf[3].q);

	return 0;
}

static int cmd_stream_tone(const struct shell *sh, size_t argc, char **argv)
{
	char *end;
	long freq;
	uint32_t amp = 500U;

	if (strcmp(argv[1], "off") == 0) {
		stream_tx_set_tone(0, 0);
		return 0;
	}

	freq = strtol(argv[1], &end, 10);
	if (*argv[1] == '\0' || *end != '\0' ||
	    (argc > 2 && (parse_u32(argv[2], &amp) < 0 || amp > 1000U))) {
		shell_error(sh, "Usage: stream tone <hz|off> [amplitude permille]");
		return -EINVAL;
	}

	stream_tx_set_tone((int32_t)freq, (uint16_t)amp);
	shell_print(sh, "TX tone %ld Hz, amplitude %u permille", freq, amp);

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_stream,
	SHELL_CMD(start, NULL, "Start RX/TX streaming", cmd_stream_start),
	SHELL_CMD(stop, NULL, "Stop streaming", cmd_stream_stop),
	SHELL_CMD(status, NULL, "Counter and error statistics", cmd_stream_status),
	SHELL_CMD(regs, NULL, "Dump SAI registers", cmd_stream_regs),
	SHELL_CMD_ARG(rate, NULL, "Measure sample rate: [ms]", cmd_stream_rate, 1, 1),
	SHELL_CMD_ARG(check, NULL, "Check counter monotonicity: [ms]", cmd_stream_check, 1, 1),
	SHELL_CMD_ARG(rxstats, NULL, "RX signal statistics: [samples] [probe_hz]",
		      cmd_stream_rxstats, 1, 2),
	SHELL_CMD_ARG(tone, NULL, "TX test tone: <hz|off> [amplitude permille]",
		      cmd_stream_tone, 2, 1),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(stream, &sub_stream, "Sample stream diagnostics", NULL);
