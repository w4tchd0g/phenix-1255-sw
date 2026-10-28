/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Phenix-1255 contributors
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/drivers/usb/udc.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/usb/usbd.h>

#include <phenix1255/protocol.h>

#include "control/control.h"
#include "stream/sample_io.h"
#include "usb/transport_usb.h"

LOG_MODULE_REGISTER(transport_usb, LOG_LEVEL_INF);

/*
 * VID:PID 1209:000A is a test PID from pid.codes. It is shared by everybody
 * who uses it for private testing, so it does not identify this device
 * uniquely and must not be used for devices that are distributed. A product
 * needs its own PID (free for open source hardware from pid.codes). See
 * firmware/src/usb/_doc/usb.md.
 */
#define USB_VID PHX_USB_VID
#define USB_PID PHX_USB_PID

#define USB_MANUFACTURER "Phenix-1255"
#define USB_PRODUCT      "Phenix-1255 SX1255 SDR"
/* Bus powered, 500 mA (bMaxPower in 2 mA units) */
#define USB_MAX_POWER    250U

#define USB_HS_BULK_MPS 512U
#define USB_FS_BULK_MPS 64U

#define CTRL_STACK_SIZE 3072
#define CTRL_PRIORITY   K_PRIO_PREEMPT(6)

/* Largest control message in either direction */
#define CTRL_MSG_MAX                                                                              \
	(sizeof(struct phx_ctrl_hdr) + sizeof(struct phx_resp_status) + PHX_CTRL_MAX_PAYLOAD)

/*
 * Every message and every sample packet is one bulk transfer. OUT
 * transfers end with a short packet (or a zero-length packet after a
 * transfer of whole packets), so OUT buffers cover whole HS packets.
 */
#define CTRL_OUT_BUF_SIZE   ROUND_UP(CTRL_MSG_MAX, USB_HS_BULK_MPS)
#define CTRL_IN_BUF_SIZE    CTRL_MSG_MAX
#define SAMPLE_OUT_BUF_SIZE ROUND_UP(PHX_MAX_SAMPLE_PACKET_BYTES, USB_HS_BULK_MPS)
#define SAMPLE_IN_BUF_SIZE  PHX_MAX_SAMPLE_PACKET_BYTES

/* One control OUT transfer is queued, the others wait for the control thread */
#define CTRL_OUT_BUFS   4
#define CTRL_IN_BUFS    4
/* TX packets are consumed in the USB stack thread */
#define SAMPLE_OUT_BUFS 3
/* About 5 ms of RX packets at 600 kHz */
#define SAMPLE_IN_BUFS  8

/* Control responses wait this long for a free IN buffer */
#define CTRL_SEND_TIMEOUT K_MSEC(100)

UDC_BUF_POOL_DEFINE(ctrl_out_pool, CTRL_OUT_BUFS, CTRL_OUT_BUF_SIZE, sizeof(struct udc_buf_info),
		    NULL);
UDC_BUF_POOL_DEFINE(ctrl_in_pool, CTRL_IN_BUFS, CTRL_IN_BUF_SIZE, sizeof(struct udc_buf_info),
		    NULL);
UDC_BUF_POOL_DEFINE(sample_out_pool, SAMPLE_OUT_BUFS, SAMPLE_OUT_BUF_SIZE,
		    sizeof(struct udc_buf_info), NULL);
UDC_BUF_POOL_DEFINE(sample_in_pool, SAMPLE_IN_BUFS, SAMPLE_IN_BUF_SIZE,
		    sizeof(struct udc_buf_info), NULL);

/* usb_state bits */
#define STATE_ENABLED         0 /* Configured by the host, endpoints usable */
#define STATE_SESSION         1 /* Host session opened on this transport */
#define STATE_CTRL_OUT_QUEUED 2 /* Control OUT transfer queued */

static atomic_t usb_state;
static struct usbd_class_data *usb_cdata;
static K_FIFO_DEFINE(ctrl_rx_fifo);

static void link_lost_handler(struct k_work *work);
static K_WORK_DEFINE(link_lost_work, link_lost_handler);

/* ------------------------------------------------------------------------ */
/* Descriptors                                                              */
/* ------------------------------------------------------------------------ */

struct phx_usb_desc {
	struct usb_if_descriptor if0;
	struct usb_ep_descriptor fs_ctrl_out;
	struct usb_ep_descriptor fs_ctrl_in;
	struct usb_ep_descriptor fs_sample_out;
	struct usb_ep_descriptor fs_sample_in;
	struct usb_ep_descriptor hs_ctrl_out;
	struct usb_ep_descriptor hs_ctrl_in;
	struct usb_ep_descriptor hs_sample_out;
	struct usb_ep_descriptor hs_sample_in;
	struct usb_desc_header nil_desc;
};

#define BULK_EP(addr, mps)                                                                        \
	{                                                                                         \
		.bLength = sizeof(struct usb_ep_descriptor),                                      \
		.bDescriptorType = USB_DESC_ENDPOINT,                                             \
		.bEndpointAddress = (addr),                                                       \
		.bmAttributes = USB_EP_TYPE_BULK,                                                 \
		.wMaxPacketSize = sys_cpu_to_le16(mps),                                           \
		.bInterval = 0,                                                                   \
	}

/*
 * The stack assigns endpoint numbers in descriptor order, which yields the
 * addresses of protocol.h. enable() verifies the result.
 */
static struct phx_usb_desc usb_desc = {
	.if0 = {
		.bLength = sizeof(struct usb_if_descriptor),
		.bDescriptorType = USB_DESC_INTERFACE,
		.bInterfaceNumber = 0,
		.bAlternateSetting = 0,
		.bNumEndpoints = 4,
		.bInterfaceClass = USB_BCC_VENDOR,
		.bInterfaceSubClass = 0,
		.bInterfaceProtocol = 0,
		.iInterface = 0,
	},
	.fs_ctrl_out = BULK_EP(PHX_USB_EP_CTRL_OUT, USB_FS_BULK_MPS),
	.fs_ctrl_in = BULK_EP(PHX_USB_EP_CTRL_IN, USB_FS_BULK_MPS),
	.fs_sample_out = BULK_EP(PHX_USB_EP_SAMPLE_OUT, USB_FS_BULK_MPS),
	.fs_sample_in = BULK_EP(PHX_USB_EP_SAMPLE_IN, USB_FS_BULK_MPS),
	.hs_ctrl_out = BULK_EP(PHX_USB_EP_CTRL_OUT, USB_HS_BULK_MPS),
	.hs_ctrl_in = BULK_EP(PHX_USB_EP_CTRL_IN, USB_HS_BULK_MPS),
	.hs_sample_out = BULK_EP(PHX_USB_EP_SAMPLE_OUT, USB_HS_BULK_MPS),
	.hs_sample_in = BULK_EP(PHX_USB_EP_SAMPLE_IN, USB_HS_BULK_MPS),
	.nil_desc = {
		.bLength = 0,
		.bDescriptorType = 0,
	},
};

static const struct usb_desc_header *const usb_fs_desc[] = {
	(struct usb_desc_header *)&usb_desc.if0,
	(struct usb_desc_header *)&usb_desc.fs_ctrl_out,
	(struct usb_desc_header *)&usb_desc.fs_ctrl_in,
	(struct usb_desc_header *)&usb_desc.fs_sample_out,
	(struct usb_desc_header *)&usb_desc.fs_sample_in,
	(struct usb_desc_header *)&usb_desc.nil_desc,
};

static const struct usb_desc_header *const usb_hs_desc[] = {
	(struct usb_desc_header *)&usb_desc.if0,
	(struct usb_desc_header *)&usb_desc.hs_ctrl_out,
	(struct usb_desc_header *)&usb_desc.hs_ctrl_in,
	(struct usb_desc_header *)&usb_desc.hs_sample_out,
	(struct usb_desc_header *)&usb_desc.hs_sample_in,
	(struct usb_desc_header *)&usb_desc.nil_desc,
};

USBD_DEVICE_DEFINE(phx_usbd, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)), USB_VID, USB_PID);

USBD_DESC_LANG_DEFINE(phx_lang);
USBD_DESC_MANUFACTURER_DEFINE(phx_mfr, USB_MANUFACTURER);
USBD_DESC_PRODUCT_DEFINE(phx_product, USB_PRODUCT);
/* Serial number: MCU unique ID in hex, the same ID as in GET_INFO */
USBD_DESC_SERIAL_NUMBER_DEFINE(phx_sn);

USBD_DESC_CONFIG_DEFINE(phx_fs_cfg_desc, "FS Configuration");
USBD_DESC_CONFIG_DEFINE(phx_hs_cfg_desc, "HS Configuration");
USBD_CONFIGURATION_DEFINE(phx_fs_config, 0, USB_MAX_POWER, &phx_fs_cfg_desc);
USBD_CONFIGURATION_DEFINE(phx_hs_config, 0, USB_MAX_POWER, &phx_hs_cfg_desc);

/* ------------------------------------------------------------------------ */
/* Endpoint helpers                                                         */
/* ------------------------------------------------------------------------ */

static bool is_high_speed(void)
{
	return USBD_SUPPORTS_HIGH_SPEED && usbd_bus_speed(&phx_usbd) == USBD_SPEED_HS;
}

static uint8_t ep_ctrl_out(void)
{
	return is_high_speed() ? usb_desc.hs_ctrl_out.bEndpointAddress
			       : usb_desc.fs_ctrl_out.bEndpointAddress;
}

static uint8_t ep_ctrl_in(void)
{
	return is_high_speed() ? usb_desc.hs_ctrl_in.bEndpointAddress
			       : usb_desc.fs_ctrl_in.bEndpointAddress;
}

static uint8_t ep_sample_out(void)
{
	return is_high_speed() ? usb_desc.hs_sample_out.bEndpointAddress
			       : usb_desc.fs_sample_out.bEndpointAddress;
}

static uint8_t ep_sample_in(void)
{
	return is_high_speed() ? usb_desc.hs_sample_in.bEndpointAddress
			       : usb_desc.fs_sample_in.bEndpointAddress;
}

static size_t bulk_mps(void)
{
	return is_high_speed() ? USB_HS_BULK_MPS : USB_FS_BULK_MPS;
}

static struct net_buf *buf_alloc(struct net_buf_pool *pool, uint8_t ep, k_timeout_t timeout)
{
	struct net_buf *buf = net_buf_alloc(pool, timeout);

	if (buf != NULL) {
		memset(udc_get_buf_info(buf), 0, sizeof(struct udc_buf_info));
		udc_get_buf_info(buf)->ep = ep;
	}

	return buf;
}

static int submit_out(struct net_buf_pool *pool, uint8_t ep)
{
	struct net_buf *buf;
	int err;

	buf = buf_alloc(pool, ep, K_NO_WAIT);
	if (buf == NULL) {
		return -ENOMEM;
	}

	err = usbd_ep_enqueue(usb_cdata, buf);
	if (err != 0) {
		net_buf_unref(buf);
	}

	return err;
}

/* Keep one control OUT transfer queued while a buffer is available */
static void submit_ctrl_out(void)
{
	if (!atomic_test_bit(&usb_state, STATE_ENABLED) ||
	    atomic_test_and_set_bit(&usb_state, STATE_CTRL_OUT_QUEUED)) {
		return;
	}

	if (submit_out(&ctrl_out_pool, ep_ctrl_out()) != 0) {
		/* Retried by the control thread after it released a buffer */
		atomic_clear_bit(&usb_state, STATE_CTRL_OUT_QUEUED);
	}
}

static int send_in(struct net_buf_pool *pool, uint8_t ep, const void *data, size_t len,
		   k_timeout_t timeout)
{
	struct net_buf *buf;
	int err;

	if (!atomic_test_bit(&usb_state, STATE_ENABLED)) {
		return -ENOTCONN;
	}

	buf = buf_alloc(pool, ep, timeout);
	if (buf == NULL) {
		return -ENOMEM;
	}

	net_buf_add_mem(buf, data, len);
	/* A transfer of whole packets is terminated by a zero-length packet */
	if (len % bulk_mps() == 0U) {
		udc_ep_buf_set_zlp(buf);
	}

	err = usbd_ep_enqueue(usb_cdata, buf);
	if (err != 0) {
		net_buf_unref(buf);
	}

	return err;
}

/* ------------------------------------------------------------------------ */
/* Control transport                                                        */
/* ------------------------------------------------------------------------ */

static int usb_send_ctrl(const void *msg, size_t len)
{
	return send_in(&ctrl_in_pool, ep_ctrl_in(), msg, len, CTRL_SEND_TIMEOUT);
}

static int usb_send_sample(const void *pkt, size_t len)
{
	/* Never blocks the RX pump: a full queue drops the packet */
	return send_in(&sample_in_pool, ep_sample_in(), pkt, len, K_NO_WAIT);
}

/*
 * Session ends on the device side. IN transfers that the host has not read
 * yet stay queued: cancelling primed transfers on an enabled EHCI endpoint
 * leaves the endpoint in an inconsistent state. A new host ignores them
 * (RX packets before STREAM_RX, responses with a foreign sequence number).
 */
static void usb_close(void)
{
	atomic_clear_bit(&usb_state, STATE_SESSION);
}

static const struct control_transport usb_transport = {
	.type = PHX_TRANSPORT_USB,
	.send_ctrl = usb_send_ctrl,
	.send_sample = usb_send_sample,
	.set_rx_port = NULL,
	.close = usb_close,
};

static void link_lost_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (atomic_test_bit(&usb_state, STATE_SESSION)) {
		LOG_WRN("USB link lost, closing session");
		control_session_terminate();
	}
}

static void handle_ctrl_message(const uint8_t *msg, size_t len)
{
	struct phx_ctrl_hdr hdr;

	if (len < sizeof(hdr)) {
		return;
	}
	memcpy(&hdr, msg, sizeof(hdr));

	/*
	 * USB has no connection setup: HELLO opens the session. A HELLO
	 * within a session comes from a new host instance (the previous one
	 * ended without closing), so the old session is closed first.
	 */
	if (hdr.opcode == PHX_OP_HELLO && sys_le16_to_cpu(hdr.magic) == PHX_CTRL_MAGIC) {
		if (atomic_test_bit(&usb_state, STATE_SESSION)) {
			LOG_INF("HELLO within a session, restarting session");
			usb_close();
			control_session_close();
		}
		if (control_session_open(&usb_transport) == 0) {
			atomic_set_bit(&usb_state, STATE_SESSION);
			LOG_INF("Host connected (USB %s speed)", is_high_speed() ? "high" : "full");
		}
	}

	/* Messages outside a session are dropped by the control module */
	control_handle_message(msg, len);
}

static void ctrl_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		struct net_buf *buf = k_fifo_get(&ctrl_rx_fifo, K_FOREVER);

		handle_ctrl_message(buf->data, buf->len);
		net_buf_unref(buf);
		submit_ctrl_out();
	}
}

K_THREAD_DEFINE(usb_ctrl_tid, CTRL_STACK_SIZE, ctrl_thread, NULL, NULL, NULL, CTRL_PRIORITY, 0,
		0);

/* ------------------------------------------------------------------------ */
/* USB function                                                             */
/* ------------------------------------------------------------------------ */

/* Transfer completion, called from the USB device stack thread */
static int phx_request(struct usbd_class_data *const c_data, struct net_buf *buf, int err)
{
	const uint8_t ep = udc_get_buf_info(buf)->ep;

	ARG_UNUSED(c_data);

	if (ep == ep_ctrl_out()) {
		atomic_clear_bit(&usb_state, STATE_CTRL_OUT_QUEUED);
		if (err == 0 && buf->len > 0U) {
			k_fifo_put(&ctrl_rx_fifo, buf);
		} else {
			net_buf_unref(buf);
		}
		submit_ctrl_out();
		return 0;
	}

	if (ep == ep_sample_out()) {
		/* Accept TX samples only within a session */
		if (err == 0 && atomic_test_bit(&usb_state, STATE_SESSION)) {
			sample_io_handle_tx_packet(buf->data, buf->len);
		}
		net_buf_unref(buf);
		if (atomic_test_bit(&usb_state, STATE_ENABLED)) {
			submit_out(&sample_out_pool, ep_sample_out());
		}
		return 0;
	}

	/* IN transfers: completed, or cancelled on session close or reset */
	if (err != 0 && err != -ECONNABORTED) {
		LOG_DBG("Transfer on ep 0x%02x failed (%d)", ep, err);
	}
	net_buf_unref(buf);

	return 0;
}

static void phx_enable(struct usbd_class_data *const c_data)
{
	ARG_UNUSED(c_data);

	if (ep_ctrl_out() != PHX_USB_EP_CTRL_OUT || ep_ctrl_in() != PHX_USB_EP_CTRL_IN ||
	    ep_sample_out() != PHX_USB_EP_SAMPLE_OUT || ep_sample_in() != PHX_USB_EP_SAMPLE_IN) {
		LOG_ERR("Unexpected endpoint assignment %02x %02x %02x %02x", ep_ctrl_out(),
			ep_ctrl_in(), ep_sample_out(), ep_sample_in());
	}

	atomic_set_bit(&usb_state, STATE_ENABLED);
	submit_ctrl_out();
	for (int i = 0; i < SAMPLE_OUT_BUFS; i++) {
		submit_out(&sample_out_pool, ep_sample_out());
	}

	LOG_INF("USB configured (%s speed)", is_high_speed() ? "high" : "full");
}

/* Deconfigured or bus reset; the stack cancels all queued transfers */
static void phx_disable(struct usbd_class_data *const c_data)
{
	ARG_UNUSED(c_data);

	atomic_clear_bit(&usb_state, STATE_ENABLED);
	k_work_submit(&link_lost_work);
	LOG_INF("USB deconfigured");
}

static void phx_suspended(struct usbd_class_data *const c_data)
{
	ARG_UNUSED(c_data);

	/* Host asleep or cable removed: no keepalives can arrive */
	k_work_submit(&link_lost_work);
}

static const void *phx_get_desc(struct usbd_class_data *const c_data, const enum usbd_speed speed)
{
	ARG_UNUSED(c_data);

	if (USBD_SUPPORTS_HIGH_SPEED && speed == USBD_SPEED_HS) {
		return usb_hs_desc;
	}

	return usb_fs_desc;
}

static int phx_init(struct usbd_class_data *c_data)
{
	usb_cdata = c_data;

	return 0;
}

static const struct usbd_class_api phx_class_api = {
	.request = phx_request,
	.enable = phx_enable,
	.disable = phx_disable,
	.suspended = phx_suspended,
	.get_desc = phx_get_desc,
	.init = phx_init,
};

USBD_DEFINE_CLASS(phenix_1255, &phx_class_api, NULL, NULL);

static bool controller_enabled;

static int controller_enable(struct usbd_context *const ctx)
{
	int err = 0;

	if (!controller_enabled) {
		err = usbd_enable(ctx);
		/* -EALREADY: enabled concurrently by the start or the handler */
		if (err == -EALREADY) {
			err = 0;
		}
		controller_enabled = (err == 0);
	}

	return err;
}

/* Called from the system work queue (USBD_MSG_DEFERRED_MODE) */
static void usbd_msg_handler(struct usbd_context *const ctx, const struct usbd_msg *msg)
{
	switch (msg->type) {
	case USBD_MSG_VBUS_READY:
		if (controller_enable(ctx) != 0) {
			LOG_ERR("USB enable failed");
		}
		break;
	case USBD_MSG_VBUS_REMOVED:
		if (controller_enabled) {
			usbd_disable(ctx);
			controller_enabled = false;
		}
		k_work_submit(&link_lost_work);
		break;
	case USBD_MSG_UDC_ERROR:
	case USBD_MSG_STACK_ERROR:
		LOG_ERR("USB error %s (%d)", usbd_msg_type_string(msg->type), msg->status);
		break;
	default:
		break;
	}
}

static int add_speed(enum usbd_speed speed, struct usbd_config_node *cfg)
{
	int err;

	err = usbd_add_configuration(&phx_usbd, speed, cfg);
	if (err == 0) {
		err = usbd_register_class(&phx_usbd, "phenix_1255", speed, 1);
	}
	if (err == 0) {
		/* Class information is in the interface descriptor */
		err = usbd_device_set_code_triple(&phx_usbd, speed, 0, 0, 0);
	}

	return err;
}

int transport_usb_start(void)
{
	int err;

	err = usbd_add_descriptor(&phx_usbd, &phx_lang);
	if (err == 0) {
		err = usbd_add_descriptor(&phx_usbd, &phx_mfr);
	}
	if (err == 0) {
		err = usbd_add_descriptor(&phx_usbd, &phx_product);
	}
	if (err == 0) {
		err = usbd_add_descriptor(&phx_usbd, &phx_sn);
	}
	if (err == 0 && USBD_SUPPORTS_HIGH_SPEED && usbd_caps_speed(&phx_usbd) == USBD_SPEED_HS) {
		err = add_speed(USBD_SPEED_HS, &phx_hs_config);
	}
	if (err == 0) {
		err = add_speed(USBD_SPEED_FS, &phx_fs_config);
	}
	if (err == 0) {
		usbd_self_powered(&phx_usbd, false);
		err = usbd_msg_register_cb(&phx_usbd, usbd_msg_handler);
	}
	if (err == 0) {
		err = usbd_init(&phx_usbd);
	}
	if (err != 0) {
		LOG_ERR("USB device setup failed (%d)", err);
		return err;
	}

	/*
	 * The controller is enabled right away, also when it can detect VBUS:
	 * the EHCI driver reports VBUS only on a change, so a cable that is
	 * already plugged in at boot produces no VBUS_READY message. Enabling
	 * without VBUS is harmless; the VBUS messages handle later changes.
	 */
	err = controller_enable(&phx_usbd);
	if (err != 0) {
		LOG_ERR("USB enable failed (%d)", err);
		return err;
	}

	LOG_INF("USB function %04x:%04x ready", USB_VID, USB_PID);

	return 0;
}
