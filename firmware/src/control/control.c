/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Phenix-1255 contributors
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/app_version.h>

#include "board/board_io.h"
#include "config/config_store.h"
#include "control/control.h"
#include "radio/sx1255.h"
#include "radio/sx1255_regs.h"
#include "stream/sample_io.h"
#include "stream/stream.h"

LOG_MODULE_REGISTER(control, LOG_LEVEL_INF);

BUILD_ASSERT((int)PHX_GAIN_LNA == (int)SX1255_GAIN_LNA &&
		     (int)PHX_GAIN_PGA == (int)SX1255_GAIN_PGA &&
		     (int)PHX_GAIN_DAC == (int)SX1255_GAIN_DAC &&
		     (int)PHX_GAIN_MIXER == (int)SX1255_GAIN_MIXER,
	     "gain element numbering");
BUILD_ASSERT(SX1255_NUM_SAMPLE_RATES <= PHX_MAX_RATES, "rate list size");

/*
 * Serializes command execution, session open/close and sending. Commands
 * that switch the PA must not interleave with a session close.
 */
static K_MUTEX_DEFINE(session_lock);

static const struct control_transport *transport;
static struct k_work_delayable link_timeout_work;

static bool rx_streaming;
static bool tx_streaming;
/* SET_TX_OUTPUT: TX driver enable while TX streams (SoapySX antenna "TX") */
static bool tx_output = true;

/* Response buffer: header + status + largest payload */
static uint8_t resp_buf[sizeof(struct phx_ctrl_hdr) + sizeof(struct phx_resp_status) +
			PHX_CTRL_MAX_PAYLOAD];

static void stop_all_streams(void)
{
	rx_streaming = false;
	tx_streaming = false;
	sample_io_rx_enable(false);
	stream_tx_enable(false, false);
	stream_stop();
}

static int send_message(uint8_t opcode, uint16_t seq, const void *payload, size_t len)
{
	struct phx_ctrl_hdr *hdr = (struct phx_ctrl_hdr *)resp_buf;

	if (transport == NULL || len > sizeof(resp_buf) - sizeof(*hdr)) {
		return -EINVAL;
	}

	hdr->magic = sys_cpu_to_le16(PHX_CTRL_MAGIC);
	hdr->version = PHX_PROTO_VERSION;
	hdr->opcode = opcode;
	hdr->seq = sys_cpu_to_le16(seq);
	hdr->length = sys_cpu_to_le16((uint16_t)len);
	if (payload != NULL && len > 0U && payload != &resp_buf[sizeof(*hdr)]) {
		memmove(&resp_buf[sizeof(*hdr)], payload, len);
	}

	return transport->send_ctrl(resp_buf, sizeof(*hdr) + len);
}

static int send_response(uint8_t opcode, uint16_t seq, int32_t status, const void *data,
			 size_t len)
{
	uint8_t *payload = &resp_buf[sizeof(struct phx_ctrl_hdr)];
	struct phx_resp_status st = {.status = (int32_t)sys_cpu_to_le32((uint32_t)status)};

	if (status != 0) {
		len = 0;
	}
	if (len > PHX_CTRL_MAX_PAYLOAD) {
		return -EINVAL;
	}

	memcpy(payload, &st, sizeof(st));
	if (len > 0U) {
		memcpy(payload + sizeof(st), data, len);
	}

	return send_message(opcode | PHX_OP_RESPONSE, seq, payload, sizeof(st) + len);
}

static void send_event(uint16_t code, int32_t detail)
{
	struct phx_event ev = {
		.code = sys_cpu_to_le16(code),
		.detail = (int32_t)sys_cpu_to_le32((uint32_t)detail),
	};

	if (transport != NULL) {
		send_message(PHX_OP_EVENT, 0, &ev, sizeof(ev));
	}
}

static void fill_info(struct phx_info *info)
{
	uint8_t id[16];
	ssize_t id_len;

	memset(info, 0, sizeof(*info));
	info->proto_version = sys_cpu_to_le16(PHX_PROTO_VERSION);
	info->transport = (uint8_t)transport->type;
	info->num_rates = SX1255_NUM_SAMPLE_RATES;
	snprintf(info->fw_version, sizeof(info->fw_version), "%s", APP_VERSION_EXTENDED_STRING);

	/* Hex digits, NUL terminated only if shorter than the field */
	id_len = hwinfo_get_device_id(id, sizeof(id));
	for (ssize_t i = 0; i < id_len && (size_t)(2 * i + 1) < sizeof(info->serial); i++) {
		static const char hex[] = "0123456789abcdef";

		info->serial[2 * i] = hex[id[i] >> 4];
		info->serial[2 * i + 1] = hex[id[i] & 0xFU];
	}

	info->xtal_hz = sys_cpu_to_le32(sx1255_xtal_hz());
	info->freq_min_hz = sys_cpu_to_le32(SX1255_FREQ_MIN_HZ);
	info->freq_max_hz = sys_cpu_to_le32(SX1255_FREQ_MAX_HZ);
	info->max_packet_samples = sys_cpu_to_le16(PHX_MAX_PACKET_SAMPLES);
	info->tx_buffer_samples = sys_cpu_to_le32(STREAM_TX_BUFFER_SAMPLES);
	for (int i = 0; i < SX1255_NUM_SAMPLE_RATES; i++) {
		info->rates[i] = sys_cpu_to_le32(sx1255_sample_rate(i));
	}
}

static void fill_status(struct phx_status *st)
{
	struct stream_stats s;
	uint8_t lock = 0;
	uint32_t state = 0;

	stream_get_stats(&s);
	sx1255_get_pll_lock(&lock);

	if (rx_streaming) {
		state |= PHX_STATE_RX_STREAMING;
	}
	if (s.tx_enabled) {
		state |= PHX_STATE_TX_STREAMING;
	}
	if (board_pa_is_enabled()) {
		state |= PHX_STATE_PA_ENABLED;
	}
	if (lock & SX1255_STAT_PLL_LOCK_RX) {
		state |= PHX_STATE_PLL_LOCK_RX;
	}
	if (lock & SX1255_STAT_PLL_LOCK_TX) {
		state |= PHX_STATE_PLL_LOCK_TX;
	}

	st->counter = (int64_t)sys_cpu_to_le64(s.counter);
	st->rate = sys_cpu_to_le32(sx1255_get_sample_rate());
	st->state = sys_cpu_to_le32(state);
	st->rx_dropped = sys_cpu_to_le32(sample_io_rx_dropped());
	st->tx_late = sys_cpu_to_le32(s.tx_late);
	st->tx_early = sys_cpu_to_le32(s.tx_early);
	st->fifo_errors = sys_cpu_to_le32(s.rx_fifo_errors + s.tx_fifo_errors);
}

static int ensure_running(void)
{
	int ret;

	if (stream_is_running()) {
		return 0;
	}

	ret = stream_start();
	if (ret == 0) {
		board_led_set(BOARD_LED_ERR, BOARD_LED_OFF);
	}

	return ret;
}

static void stop_if_idle(void)
{
	if (!rx_streaming && !tx_streaming) {
		stream_stop();
	}
}

static int32_t cmd_stream(bool tx, bool enable, struct phx_stream_resp *resp)
{
	int ret = 0;

	if (enable) {
		ret = ensure_running();
		if (ret < 0) {
			return ret;
		}
		if (tx) {
			ret = stream_tx_enable(true, tx_output);
			tx_streaming = (ret == 0);
		} else {
			sample_io_rx_enable(true);
			rx_streaming = true;
		}
	} else {
		if (tx) {
			stream_tx_enable(false, false);
			tx_streaming = false;
		} else {
			sample_io_rx_enable(false);
			rx_streaming = false;
		}
		stop_if_idle();
	}

	resp->counter = (int64_t)sys_cpu_to_le64(stream_counter());

	return ret;
}

static void dispatch(uint8_t op, uint16_t seq, const uint8_t *p, size_t len)
{
	union {
		struct phx_info info;
		struct phx_status status;
		struct phx_set_freq_resp freq;
		struct phx_set_gain_resp gain;
		struct phx_stream_resp stream;
		uint8_t regs[PHX_MAX_REG_COUNT];
	} r;
	size_t rlen = 0;
	int32_t st = 0;

	memset(&r, 0, sizeof(r));

	switch (op) {
	case PHX_OP_HELLO: {
		struct phx_hello_req req;

		if (len < sizeof(req)) {
			st = PHX_EINVAL;
			break;
		}
		memcpy(&req, p, sizeof(req));
		if (sys_le16_to_cpu(req.proto_version) != PHX_PROTO_VERSION) {
			LOG_WRN("Host protocol version %u not supported",
				sys_le16_to_cpu(req.proto_version));
			st = PHX_ENOTSUP;
			break;
		}
		if (transport->set_rx_port != NULL) {
			transport->set_rx_port(sys_le16_to_cpu(req.rx_udp_port));
		}
		fill_info(&r.info);
		rlen = sizeof(r.info);
		break;
	}
	case PHX_OP_KEEPALIVE:
		break;
	case PHX_OP_GET_INFO:
		fill_info(&r.info);
		rlen = sizeof(r.info);
		break;
	case PHX_OP_GET_STATUS:
		fill_status(&r.status);
		rlen = sizeof(r.status);
		break;
	case PHX_OP_SET_FREQ: {
		struct phx_set_freq_req req;
		uint32_t actual;
		enum sx1255_dir dir;

		if (len < sizeof(req)) {
			st = PHX_EINVAL;
			break;
		}
		memcpy(&req, p, sizeof(req));
		dir = (req.dir == PHX_DIR_TX) ? SX1255_TX : SX1255_RX;
		st = sx1255_set_frequency(dir, sys_le32_to_cpu(req.freq_hz), &actual);
		r.freq.frf = sys_cpu_to_le32(sx1255_get_frf(dir));
		r.freq.freq_hz = sys_cpu_to_le32(actual);
		rlen = sizeof(r.freq);
		break;
	}
	case PHX_OP_SET_RATE: {
		struct phx_set_rate_req req;

		if (len < sizeof(req)) {
			st = PHX_EINVAL;
			break;
		}
		memcpy(&req, p, sizeof(req));
		st = stream_is_running() ? PHX_EBUSY
					 : sx1255_set_sample_rate(sys_le32_to_cpu(req.rate));
		break;
	}
	case PHX_OP_SET_GAIN: {
		struct phx_set_gain_req req;
		int actual;

		if (len < sizeof(req)) {
			st = PHX_EINVAL;
			break;
		}
		memcpy(&req, p, sizeof(req));
		if (req.element >= SX1255_GAIN_COUNT) {
			st = PHX_EINVAL;
			break;
		}
		st = sx1255_set_gain((enum sx1255_gain)req.element,
				     (int16_t)sys_le16_to_cpu((uint16_t)req.gain_db), &actual);
		r.gain.gain_db = (int16_t)sys_cpu_to_le16((uint16_t)actual);
		rlen = sizeof(r.gain);
		break;
	}
	case PHX_OP_SET_RX_INPUT:
		if (len < sizeof(struct phx_u8_req) || p[0] > PHX_RX_INPUT_LOOPBACK) {
			st = PHX_EINVAL;
			break;
		}
		st = sx1255_set_rx_input((p[0] == PHX_RX_INPUT_LOOPBACK)
						 ? SX1255_RX_INPUT_LOOPBACK
						 : SX1255_RX_INPUT_ANTENNA);
		break;
	case PHX_OP_SET_TX_OUTPUT:
		if (len < sizeof(struct phx_u8_req)) {
			st = PHX_EINVAL;
			break;
		}
		tx_output = (p[0] != 0U);
		if (tx_streaming) {
			st = sx1255_set_mode(true, true, tx_output);
		}
		break;
	case PHX_OP_READ_REG: {
		struct phx_reg_req req;

		if (len < sizeof(req)) {
			st = PHX_EINVAL;
			break;
		}
		memcpy(&req, p, sizeof(req));
		if (req.count == 0U || req.count > PHX_MAX_REG_COUNT) {
			st = PHX_EINVAL;
			break;
		}
		st = sx1255_read_regs(req.addr, r.regs, req.count);
		rlen = req.count;
		break;
	}
	case PHX_OP_WRITE_REG: {
		struct phx_reg_req req;

		if (len < sizeof(req)) {
			st = PHX_EINVAL;
			break;
		}
		memcpy(&req, p, sizeof(req));
		if (req.count == 0U || req.count > PHX_MAX_REG_COUNT ||
		    len < sizeof(req) + req.count) {
			st = PHX_EINVAL;
			break;
		}
		st = sx1255_write_regs(req.addr, p + sizeof(req), req.count);
		break;
	}
	case PHX_OP_STREAM_RX:
	case PHX_OP_STREAM_TX:
		if (len < sizeof(struct phx_u8_req)) {
			st = PHX_EINVAL;
			break;
		}
		st = cmd_stream(op == PHX_OP_STREAM_TX, p[0] != 0U, &r.stream);
		rlen = sizeof(r.stream);
		break;
	default:
		st = PHX_ENOTSUP;
		break;
	}

	if (st != 0) {
		LOG_WRN("Command 0x%02x failed (%d)", op, st);
	}
	send_response(op, seq, st, &r, rlen);
}

void control_handle_message(const uint8_t *msg, size_t len)
{
	struct phx_ctrl_hdr hdr;
	size_t plen;

	if (len < sizeof(hdr)) {
		return;
	}
	memcpy(&hdr, msg, sizeof(hdr));
	plen = sys_le16_to_cpu(hdr.length);

	if (sys_le16_to_cpu(hdr.magic) != PHX_CTRL_MAGIC || hdr.version != PHX_PROTO_VERSION ||
	    plen > PHX_CTRL_MAX_PAYLOAD || len < sizeof(hdr) + plen ||
	    (hdr.opcode & PHX_OP_RESPONSE) != 0U) {
		LOG_WRN("Malformed control message dropped");
		return;
	}

	k_mutex_lock(&session_lock, K_FOREVER);
	if (transport != NULL) {
		/* Every valid message keeps the link alive */
		k_work_reschedule(&link_timeout_work, K_MSEC(PHX_LINK_TIMEOUT_MS));
		dispatch(hdr.opcode, sys_le16_to_cpu(hdr.seq), msg + sizeof(hdr), plen);
	}
	k_mutex_unlock(&session_lock);
}

void control_session_terminate(void)
{
	const struct control_transport *t;

	/* PA off first, independent of any lock held by a blocked sender */
	stream_tx_enable(false, false);

	k_mutex_lock(&session_lock, K_FOREVER);
	t = transport;
	k_mutex_unlock(&session_lock);

	if (t != NULL && t->close != NULL) {
		t->close();
	}
	control_session_close();
}

static void link_timeout_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	LOG_WRN("Host link timeout, closing session");
	control_session_terminate();
}

static void stream_fault(int err)
{
	k_mutex_lock(&session_lock, K_FOREVER);
	rx_streaming = false;
	tx_streaming = false;
	sample_io_rx_enable(false);
	board_led_set(BOARD_LED_ERR, BOARD_LED_ON);
	send_event(PHX_EVENT_STREAM_ERROR, err);
	send_event(PHX_EVENT_PA_SHUTDOWN, err);
	k_mutex_unlock(&session_lock);
}

int control_init(void)
{
	k_work_init_delayable(&link_timeout_work, link_timeout_handler);
	stream_set_fault_handler(stream_fault);

	return 0;
}

int control_session_open(const struct control_transport *t)
{
	int ret = 0;

	k_mutex_lock(&session_lock, K_FOREVER);
	if (transport != NULL) {
		ret = -EBUSY;
	} else {
		transport = t;
		tx_output = true;
		sample_io_reset();
		k_work_reschedule(&link_timeout_work, K_MSEC(PHX_LINK_TIMEOUT_MS));
		board_led_set(BOARD_LED_OK, BOARD_LED_ON);
		LOG_INF("Session opened");
	}
	k_mutex_unlock(&session_lock);

	return ret;
}

void control_session_close(void)
{
	k_mutex_lock(&session_lock, K_FOREVER);
	if (transport != NULL) {
		transport = NULL;
		k_work_cancel_delayable(&link_timeout_work);
		stop_all_streams();
		board_led_set(BOARD_LED_OK, BOARD_LED_BLINK_SLOW);
		LOG_INF("Session closed");
	}
	k_mutex_unlock(&session_lock);
}

bool control_session_active(void)
{
	return transport != NULL;
}

void control_reboot(bool factory_reset)
{
	int ret;

	/* The host sees the session end before the reset */
	control_session_terminate();
	stream_stop();
	board_pa_set(false);

	if (factory_reset) {
		ret = config_store_erase();
		if (ret < 0) {
			LOG_ERR("Erasing settings partition failed (%d)", ret);
		} else {
			LOG_WRN("Factory reset: all settings erased");
		}
	}

	LOG_WRN("Rebooting");
	/* Lets the TCP close and the deferred log output complete */
	k_sleep(K_MSEC(200));
	sys_reboot(SYS_REBOOT_COLD);
}

int control_send_sample(const void *pkt, size_t len)
{
	const struct control_transport *t = transport;

	if (t == NULL || t->send_sample == NULL) {
		return -ENOTCONN;
	}

	return t->send_sample(pkt, len);
}
