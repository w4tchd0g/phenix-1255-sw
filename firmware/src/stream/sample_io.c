/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Phenix-1255 contributors
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>

#include <phenix1255/protocol.h>

#include "board/board_io.h"
#include "control/control.h"
#include "radio/sx1255.h"
#include "stream/sample_io.h"
#include "stream/stream.h"

LOG_MODULE_REGISTER(sample_io, LOG_LEVEL_INF);

#define RX_PUMP_STACK_SIZE 2048
/*
 * Below the Ethernet driver and the TX packet ingest, which are short per
 * packet; above the network RX thread (prj.conf), whose processing of a
 * TX packet burst takes longer than the RX ring holds.
 */
#define RX_PUMP_PRIORITY   K_PRIO_PREEMPT(4)

/* Polling interval of the RX pump */
#define RX_PUMP_INTERVAL K_USEC(500)

/*
 * RX packet duration. The number of samples per packet is chosen per sample
 * rate (360 at 600 kHz down to 16 at 25 kHz), so that the latency and the
 * resolution of the host time estimate do not depend on the rate.
 */
#define RX_PACKET_US      600U
#define RX_PACKET_MIN_LEN 16U

/*
 * When the pump falls behind by more than this, it skips ahead instead of
 * reading samples that the DMA is about to overwrite.
 */
#define RX_MAX_BACKLOG (STREAM_RX_RING_SAMPLES - 2U * PHX_MAX_PACKET_SAMPLES)

BUILD_ASSERT(RX_MAX_BACKLOG > PHX_MAX_PACKET_SAMPLES, "RX ring too small");

struct sample_packet {
	struct phx_sample_hdr hdr;
	struct phx_cs16 samples[PHX_MAX_PACKET_SAMPLES];
} __packed;

static struct sample_packet rx_pkt;

static K_SEM_DEFINE(rx_enable_sem, 0, 1);
static volatile bool rx_enabled;
static uint64_t rx_next;
static uint32_t rx_count = PHX_MAX_PACKET_SAMPLES;
static uint32_t rx_seq;
static bool rx_discontinuity;
static atomic_t rx_dropped;

static uint32_t tx_seq_expected;
static uint32_t prev_tx_late;
static uint32_t tx_packets;
static uint32_t tx_seq_gaps;
static uint32_t tx_invalid;

static void fill_rx_header(struct phx_sample_hdr *hdr, uint64_t timestamp)
{
	struct stream_stats st;

	stream_get_stats(&st);

	hdr->magic = sys_cpu_to_le16(PHX_SAMPLE_MAGIC);
	hdr->version = PHX_PROTO_VERSION;
	hdr->type = PHX_SAMPLE_RX;
	hdr->flags = sys_cpu_to_le16(rx_discontinuity ? PHX_RX_FLAG_DISCONTINUITY : 0U);
	hdr->count = sys_cpu_to_le16((uint16_t)rx_count);
	hdr->seq = sys_cpu_to_le32(rx_seq);
	hdr->rx_dropped = sys_cpu_to_le32((uint32_t)atomic_get(&rx_dropped));
	hdr->tx_late = sys_cpu_to_le32(st.tx_late);
	hdr->tx_early = sys_cpu_to_le32(st.tx_early);
	hdr->timestamp = (int64_t)sys_cpu_to_le64(timestamp);

	/* Short WARN flash on new late TX packets */
	if (st.tx_late != prev_tx_late) {
		prev_tx_late = st.tx_late;
		board_led_flash(BOARD_LED_WARN);
	}
}

static void skip_to(uint64_t index)
{
	atomic_add(&rx_dropped, (atomic_val_t)(index - rx_next));
	rx_next = index;
	rx_discontinuity = true;
	board_led_flash(BOARD_LED_WARN);
}

static void rx_pump(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		uint64_t now;
		int ret;

		if (!rx_enabled || !stream_is_running()) {
			k_sem_take(&rx_enable_sem, K_FOREVER);
			continue;
		}

		now = stream_counter();
		if (now - rx_next > RX_MAX_BACKLOG) {
			/* Host or link too slow: resume at the most recent packet */
			skip_to(now - rx_count);
		}

		while (rx_enabled && rx_next + rx_count <= now) {
			ret = stream_rx_read(rx_next, rx_pkt.samples, rx_count);
			if (ret == -EOVERFLOW) {
				skip_to(stream_counter() - rx_count);
				break;
			} else if (ret < 0) {
				break;
			}

			fill_rx_header(&rx_pkt.hdr, rx_next);
			ret = control_send_sample(&rx_pkt, sizeof(rx_pkt.hdr) +
							  rx_count * sizeof(struct phx_cs16));
			if (ret < 0) {
				/* Packet lost on the way out: report as dropped */
				atomic_add(&rx_dropped, (atomic_val_t)rx_count);
				rx_discontinuity = true;
			} else {
				rx_discontinuity = false;
			}
			rx_seq++;
			rx_next += rx_count;
		}

		k_sleep(RX_PUMP_INTERVAL);
	}
}

K_THREAD_DEFINE(rx_pump_tid, RX_PUMP_STACK_SIZE, rx_pump, NULL, NULL, NULL, RX_PUMP_PRIORITY,
		0, 0);

int sample_io_init(void)
{
	return 0;
}

void sample_io_reset(void)
{
	rx_seq = 0;
	tx_seq_expected = 0;
	prev_tx_late = 0;
	tx_packets = 0;
	tx_seq_gaps = 0;
	tx_invalid = 0;
	atomic_clear(&rx_dropped);
}

void sample_io_get_stats(struct sample_io_stats *stats)
{
	stats->rx_packets = rx_seq;
	stats->rx_dropped = (uint32_t)atomic_get(&rx_dropped);
	stats->tx_packets = tx_packets;
	stats->tx_seq_gaps = tx_seq_gaps;
	stats->tx_invalid = tx_invalid;
}

void sample_io_rx_enable(bool enable)
{
	if (enable) {
		uint32_t n = (uint32_t)(((uint64_t)sx1255_get_sample_rate() * RX_PACKET_US +
					 500000U) / 1000000U);

		rx_count = CLAMP(n, RX_PACKET_MIN_LEN, PHX_MAX_PACKET_SAMPLES);
		rx_next = stream_counter();
		rx_discontinuity = false;
		rx_enabled = true;
		k_sem_give(&rx_enable_sem);
	} else {
		rx_enabled = false;
	}
}

uint32_t sample_io_rx_dropped(void)
{
	return (uint32_t)atomic_get(&rx_dropped);
}

void sample_io_handle_tx_packet(const uint8_t *buf, size_t len)
{
	const struct phx_sample_hdr *hdr = (const struct phx_sample_hdr *)buf;
	uint16_t count;
	uint32_t seq;
	int ret;

	if (len < sizeof(*hdr)) {
		return;
	}

	count = sys_le16_to_cpu(hdr->count);
	if (sys_le16_to_cpu(hdr->magic) != PHX_SAMPLE_MAGIC || hdr->version != PHX_PROTO_VERSION ||
	    hdr->type != PHX_SAMPLE_TX || count == 0U || count > PHX_MAX_PACKET_SAMPLES ||
	    len < sizeof(*hdr) + count * sizeof(struct phx_cs16)) {
		LOG_WRN_ONCE("Malformed TX packet dropped");
		tx_invalid++;
		return;
	}

	seq = sys_le32_to_cpu(hdr->seq);
	if (tx_packets > 0U && seq != tx_seq_expected) {
		/* Packets lost on the way in (counted forward gaps only) */
		if ((int32_t)(seq - tx_seq_expected) > 0) {
			tx_seq_gaps += seq - tx_seq_expected;
		}
	}
	tx_seq_expected = seq + 1U;
	tx_packets++;

	/* Samples follow the 32-byte header, CS16 is naturally aligned there */
	ret = stream_tx_write((int64_t)sys_le64_to_cpu((uint64_t)hdr->timestamp),
			      (const struct phx_cs16 *)(buf + sizeof(*hdr)), count);
	if (ret == -ETIME || ret == -EFBIG) {
		board_led_flash(BOARD_LED_WARN);
	}
}
