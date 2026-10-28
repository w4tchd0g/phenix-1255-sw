/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Phenix-1255 contributors
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/byteorder.h>

#include <phenix1255/protocol.h>

#include "control/control.h"
#include "ethernet/transport_eth.h"
#include "stream/sample_io.h"

LOG_MODULE_REGISTER(transport_eth, LOG_LEVEL_INF);

#define CTRL_STACK_SIZE   3072
#define CTRL_PRIORITY     K_PRIO_PREEMPT(6)
#define UDP_RX_STACK_SIZE 2048
#define UDP_RX_PRIORITY   K_PRIO_PREEMPT(3)

/* Receive poll interval of the control thread, bounds the close latency */
#define RECV_POLL_MS 50

static uint16_t ctrl_port;
static uint16_t sample_port;

static int udp_sock = -1;
static int client_sock = -1;
static struct net_sockaddr_in peer;      /* Session host address */
static struct net_sockaddr_in rx_dest;   /* Host UDP endpoint for RX packets */
static volatile bool rx_dest_valid;
static volatile bool close_requested;
static K_MUTEX_DEFINE(tcp_send_lock);

static K_SEM_DEFINE(start_sem, 0, 2);

static uint8_t ctrl_buf[sizeof(struct phx_ctrl_hdr) + PHX_CTRL_MAX_PAYLOAD];
static uint8_t udp_buf[PHX_MAX_SAMPLE_PACKET_BYTES] __aligned(4);

static int eth_send_ctrl(const void *msg, size_t len)
{
	const uint8_t *p = msg;
	int ret = 0;

	k_mutex_lock(&tcp_send_lock, K_FOREVER);
	while (len > 0U && client_sock >= 0) {
		ssize_t n = zsock_send(client_sock, p, len, 0);

		if (n < 0) {
			ret = -errno;
			break;
		}
		p += n;
		len -= (size_t)n;
	}
	k_mutex_unlock(&tcp_send_lock);

	return ret;
}

static int eth_send_sample(const void *pkt, size_t len)
{
	ssize_t n;

	if (!rx_dest_valid) {
		return -ENOTCONN;
	}

	n = zsock_sendto(udp_sock, pkt, len, 0, (const struct net_sockaddr *)&rx_dest,
			 sizeof(rx_dest));

	return (n == (ssize_t)len) ? 0 : -EIO;
}

static void eth_set_rx_port(uint16_t port)
{
	rx_dest = peer;
	rx_dest.sin_port = net_htons(port);
	rx_dest_valid = (port != 0U);
}

static void eth_close(void)
{
	/*
	 * A shutdown from another thread does not wake up a blocking receive,
	 * so the control thread polls with a timeout and checks this flag.
	 */
	close_requested = true;
}

static const struct control_transport eth_transport = {
	.type = PHX_TRANSPORT_ETHERNET,
	.send_ctrl = eth_send_ctrl,
	.send_sample = eth_send_sample,
	.set_rx_port = eth_set_rx_port,
	.close = eth_close,
};

static int recv_all(int sock, uint8_t *buf, size_t len)
{
	while (len > 0U) {
		struct zsock_pollfd pfd = {.fd = sock, .events = ZSOCK_POLLIN};
		ssize_t n;
		int ret;

		if (close_requested) {
			return -ECONNABORTED;
		}

		ret = zsock_poll(&pfd, 1, RECV_POLL_MS);
		if (ret < 0) {
			return -errno;
		}
		if (ret == 0) {
			continue;
		}

		n = zsock_recv(sock, buf, len, 0);

		if (n <= 0) {
			return (n == 0) ? -ECONNRESET : -errno;
		}
		buf += n;
		len -= (size_t)n;
	}

	return 0;
}

static void serve_client(int sock)
{
	while (true) {
		struct phx_ctrl_hdr hdr;
		size_t plen;

		if (recv_all(sock, ctrl_buf, sizeof(hdr)) < 0) {
			return;
		}
		memcpy(&hdr, ctrl_buf, sizeof(hdr));
		plen = sys_le16_to_cpu(hdr.length);

		if (sys_le16_to_cpu(hdr.magic) != PHX_CTRL_MAGIC || plen > PHX_CTRL_MAX_PAYLOAD) {
			/* Framing lost on a stream socket: drop the connection */
			LOG_WRN("Invalid control header, closing connection");
			return;
		}
		if (plen > 0U && recv_all(sock, ctrl_buf + sizeof(hdr), plen) < 0) {
			return;
		}

		control_handle_message(ctrl_buf, sizeof(hdr) + plen);
	}
}

static void ctrl_thread(void *p1, void *p2, void *p3)
{
	struct net_sockaddr_in addr = {
		.sin_family = NET_AF_INET,
		.sin_addr = {.s_addr = NET_INADDR_ANY},
	};
	int listen_sock;
	int one = 1;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	k_sem_take(&start_sem, K_FOREVER);

	listen_sock = zsock_socket(NET_AF_INET, NET_SOCK_STREAM, NET_IPPROTO_TCP);
	if (listen_sock < 0) {
		LOG_ERR("TCP socket failed (%d)", errno);
		return;
	}
	zsock_setsockopt(listen_sock, ZSOCK_SOL_SOCKET, ZSOCK_SO_REUSEADDR, &one, sizeof(one));

	addr.sin_port = net_htons(ctrl_port);
	if (zsock_bind(listen_sock, (struct net_sockaddr *)&addr, sizeof(addr)) < 0 ||
	    zsock_listen(listen_sock, 1) < 0) {
		LOG_ERR("TCP bind/listen on port %u failed (%d)", ctrl_port, errno);
		zsock_close(listen_sock);
		return;
	}
	LOG_INF("Control channel on TCP port %u", ctrl_port);

	while (true) {
		struct net_sockaddr_in client_addr;
		net_socklen_t addr_len = sizeof(client_addr);
		char addr_str[NET_INET_ADDRSTRLEN];
		int sock;

		sock = zsock_accept(listen_sock, (struct net_sockaddr *)&client_addr, &addr_len);
		if (sock < 0) {
			LOG_ERR("accept failed (%d)", errno);
			k_msleep(100);
			continue;
		}

		zsock_inet_ntop(NET_AF_INET, &client_addr.sin_addr, addr_str, sizeof(addr_str));

		/* Only one host session at a time */
		peer = client_addr;
		rx_dest_valid = false;
		close_requested = false;
		if (control_session_open(&eth_transport) < 0) {
			LOG_WRN("Rejected connection from %s: session active", addr_str);
			zsock_close(sock);
			continue;
		}

		zsock_setsockopt(sock, NET_IPPROTO_TCP, ZSOCK_TCP_NODELAY, &one, sizeof(one));
		client_sock = sock;
		LOG_INF("Host %s connected", addr_str);

		serve_client(sock);

		control_session_close();
		k_mutex_lock(&tcp_send_lock, K_FOREVER);
		client_sock = -1;
		k_mutex_unlock(&tcp_send_lock);
		rx_dest_valid = false;
		zsock_close(sock);
		LOG_INF("Host %s disconnected", addr_str);
	}
}

static void udp_rx_thread(void *p1, void *p2, void *p3)
{
	struct net_sockaddr_in addr = {
		.sin_family = NET_AF_INET,
		.sin_addr = {.s_addr = NET_INADDR_ANY},
	};

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	k_sem_take(&start_sem, K_FOREVER);

	udp_sock = zsock_socket(NET_AF_INET, NET_SOCK_DGRAM, NET_IPPROTO_UDP);
	if (udp_sock < 0) {
		LOG_ERR("UDP socket failed (%d)", errno);
		return;
	}

	addr.sin_port = net_htons(sample_port);
	if (zsock_bind(udp_sock, (struct net_sockaddr *)&addr, sizeof(addr)) < 0) {
		LOG_ERR("UDP bind on port %u failed (%d)", sample_port, errno);
		return;
	}
	LOG_INF("Sample channel on UDP port %u", sample_port);

	while (true) {
		struct net_sockaddr_in src;
		net_socklen_t src_len = sizeof(src);
		ssize_t n;

		n = zsock_recvfrom(udp_sock, udp_buf, sizeof(udp_buf), 0,
				   (struct net_sockaddr *)&src, &src_len);
		if (n < 0) {
			LOG_ERR("UDP receive failed (%d)", errno);
			k_msleep(10);
			continue;
		}

		/* Accept TX samples only from the session host */
		if (!control_session_active() ||
		    src.sin_addr.s_addr != peer.sin_addr.s_addr) {
			continue;
		}

		sample_io_handle_tx_packet(udp_buf, (size_t)n);
	}
}

K_THREAD_DEFINE(eth_ctrl_tid, CTRL_STACK_SIZE, ctrl_thread, NULL, NULL, NULL, CTRL_PRIORITY, 0,
		0);
K_THREAD_DEFINE(eth_udp_tid, UDP_RX_STACK_SIZE, udp_rx_thread, NULL, NULL, NULL,
		UDP_RX_PRIORITY, 0, 0);

int transport_eth_start(uint16_t ctrl, uint16_t sample)
{
	ctrl_port = ctrl;
	sample_port = sample;

	/* Release both threads */
	k_sem_give(&start_sem);
	k_sem_give(&start_sem);

	return 0;
}
