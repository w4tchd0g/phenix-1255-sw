/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Phenix-1255 contributors
 *
 * Phenix-1255 host protocol, shared by the firmware and the SoapySDR module.
 *
 * Two logical channels exist on both transports:
 *
 *   Transport   Control channel              Sample channel
 *   Ethernet    TCP, device port ctrl_port   UDP, device port sample_port
 *   HS USB      bulk EP 0x01 OUT / 0x81 IN   bulk EP 0x02 OUT / 0x82 IN
 *
 * All multi-byte fields are little endian. Structures are packed and have
 * fixed sizes; see common/_doc/protocol.md for the full description.
 */

#ifndef PHENIX1255_PROTOCOL_H_
#define PHENIX1255_PROTOCOL_H_

#include <stdint.h>

#ifdef __cplusplus
#define PHX_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define PHX_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif

#if defined(__GNUC__) || defined(__clang__)
#define PHX_PACKED __attribute__((packed))
#else
#error "Packed structure attribute required"
#endif

#define PHX_PROTO_VERSION 1

/* USB identification. 1209:000A is a pid.codes test PID, see firmware/src/usb/_doc/usb.md. */
#define PHX_USB_VID 0x1209
#define PHX_USB_PID 0x000A

#define PHX_USB_EP_CTRL_OUT   0x01
#define PHX_USB_EP_CTRL_IN    0x81
#define PHX_USB_EP_SAMPLE_OUT 0x02
#define PHX_USB_EP_SAMPLE_IN  0x82

/* Default Ethernet ports (configurable on the device) */
#define PHX_DEFAULT_CTRL_PORT   1255
#define PHX_DEFAULT_SAMPLE_PORT 1256

/* mDNS host name */
#define PHX_MDNS_HOSTNAME "phenix-1255"

/* The link is considered lost when no control message arrives in this time */
#define PHX_LINK_TIMEOUT_MS 1000
/* Recommended host keepalive interval */
#define PHX_KEEPALIVE_INTERVAL_MS 250

/* ------------------------------------------------------------------------ */
/* Control channel                                                          */
/* ------------------------------------------------------------------------ */

#define PHX_CTRL_MAGIC 0x4350 /* "PC" */

/* Bit set in the opcode of a response */
#define PHX_OP_RESPONSE 0x80

/* Largest control payload in either direction */
#define PHX_CTRL_MAX_PAYLOAD 256

enum phx_opcode {
	PHX_OP_HELLO = 0x01,         /* Open a session */
	PHX_OP_KEEPALIVE = 0x02,     /* No payload */
	PHX_OP_GET_INFO = 0x03,      /* Device information */
	PHX_OP_GET_STATUS = 0x04,    /* Counters and state */
	PHX_OP_SET_FREQ = 0x10,      /* RX or TX synthesizer frequency */
	PHX_OP_SET_RATE = 0x11,      /* Sample rate, streams must be stopped */
	PHX_OP_SET_GAIN = 0x12,      /* One gain element */
	PHX_OP_SET_RX_INPUT = 0x13,  /* Antenna or RF loopback */
	PHX_OP_SET_TX_OUTPUT = 0x14, /* TX driver enable while TX streams */
	PHX_OP_READ_REG = 0x20,      /* Raw SX1255 register read */
	PHX_OP_WRITE_REG = 0x21,     /* Raw SX1255 register write */
	PHX_OP_STREAM_RX = 0x30,     /* Enable/disable RX sample packets */
	PHX_OP_STREAM_TX = 0x31,     /* Enable/disable TX (PA_EN, TX driver) */
	PHX_OP_EVENT = 0x7F,         /* Unsolicited device to host message */
};

/* Common header of every control message */
struct phx_ctrl_hdr {
	uint16_t magic;   /* PHX_CTRL_MAGIC */
	uint8_t version;  /* PHX_PROTO_VERSION */
	uint8_t opcode;   /* enum phx_opcode, | PHX_OP_RESPONSE in responses */
	uint16_t seq;     /* Chosen by the host, echoed in the response */
	uint16_t length;  /* Payload bytes following the header */
} PHX_PACKED;
PHX_STATIC_ASSERT(sizeof(struct phx_ctrl_hdr) == 8, "phx_ctrl_hdr size");

/* Every response payload starts with this status word */
struct phx_resp_status {
	int32_t status; /* 0 or a negative errno value (enum phx_status_code) */
} PHX_PACKED;

/* Status codes (negative errno values, fixed across platforms) */
enum phx_status_code {
	PHX_OK = 0,
	PHX_EIO = -5,
	PHX_EAGAIN = -11,
	PHX_EBUSY = -16,
	PHX_ENODEV = -19,
	PHX_EINVAL = -22,
	PHX_ERANGE = -34,
	PHX_ENOTSUP = -134,
};

/* HELLO request */
struct phx_hello_req {
	uint16_t proto_version;  /* PHX_PROTO_VERSION of the host */
	uint16_t rx_udp_port;    /* Ethernet: host UDP port for RX packets; USB: 0 */
} PHX_PACKED;

/* GET_INFO response (after phx_resp_status); also returned by HELLO */
#define PHX_INFO_STR_LEN  32
#define PHX_MAX_RATES     8

enum phx_transport {
	PHX_TRANSPORT_ETHERNET = 0,
	PHX_TRANSPORT_USB = 1,
};

struct phx_info {
	uint16_t proto_version;
	uint8_t transport;             /* enum phx_transport */
	uint8_t num_rates;
	char fw_version[PHX_INFO_STR_LEN]; /* NUL terminated */
	char serial[PHX_INFO_STR_LEN];     /* Unique ID, hex, NUL terminated if shorter */
	uint32_t xtal_hz;              /* SX1255 reference frequency */
	uint32_t freq_min_hz;
	uint32_t freq_max_hz;
	uint16_t max_packet_samples;   /* Samples per sample packet */
	uint16_t reserved;
	uint32_t tx_buffer_samples;    /* Device TX buffer depth */
	uint32_t rates[PHX_MAX_RATES]; /* Supported sample rates, highest first */
} PHX_PACKED;
PHX_STATIC_ASSERT(sizeof(struct phx_info) == 120, "phx_info size");

/* GET_STATUS response (after phx_resp_status) */
enum phx_state_flags {
	PHX_STATE_RX_STREAMING = 1u << 0,
	PHX_STATE_TX_STREAMING = 1u << 1,
	PHX_STATE_PA_ENABLED = 1u << 2,
	PHX_STATE_PLL_LOCK_RX = 1u << 3,
	PHX_STATE_PLL_LOCK_TX = 1u << 4,
};

struct phx_status {
	int64_t counter;           /* Current sample counter */
	uint32_t rate;             /* Current sample rate */
	uint32_t state;            /* enum phx_state_flags */
	uint32_t rx_dropped;       /* RX samples dropped in the device */
	uint32_t tx_late;          /* TX packets discarded as too late */
	uint32_t tx_early;         /* TX packets discarded as too early */
	uint32_t fifo_errors;      /* SAI FIFO errors (RX + TX) */
} PHX_PACKED;
PHX_STATIC_ASSERT(sizeof(struct phx_status) == 32, "phx_status size");

/* SET_FREQ */
enum phx_dir {
	PHX_DIR_RX = 0,
	PHX_DIR_TX = 1,
};

struct phx_set_freq_req {
	uint8_t dir;      /* enum phx_dir */
	uint8_t reserved[3];
	uint32_t freq_hz;
} PHX_PACKED;

struct phx_set_freq_resp {
	uint32_t frf;     /* Synthesizer word; f = frf * xtal_hz / 2^20 */
	uint32_t freq_hz; /* Rounded actual frequency */
} PHX_PACKED;

/* SET_RATE */
struct phx_set_rate_req {
	uint32_t rate;
} PHX_PACKED;

/* SET_GAIN */
enum phx_gain {
	PHX_GAIN_LNA = 0,   /* RX, 0..48 dB, 6 dB steps */
	PHX_GAIN_PGA = 1,   /* RX, 0..30 dB, 2 dB steps */
	PHX_GAIN_DAC = 2,   /* TX, 0..9 dB, 3 dB steps */
	PHX_GAIN_MIXER = 3, /* TX, 0..30 dB, 2 dB steps */
};

struct phx_set_gain_req {
	uint8_t element;  /* enum phx_gain */
	uint8_t reserved;
	int16_t gain_db;
} PHX_PACKED;

struct phx_set_gain_resp {
	int16_t gain_db;  /* Actual gain after quantization */
	uint16_t reserved;
} PHX_PACKED;

/* SET_RX_INPUT */
enum phx_rx_input {
	PHX_RX_INPUT_ANTENNA = 0,
	PHX_RX_INPUT_LOOPBACK = 1, /* SX1255 RF loopback */
};

struct phx_u8_req {
	uint8_t value;
	uint8_t reserved[3];
} PHX_PACKED;

/* READ_REG / WRITE_REG */
#define PHX_MAX_REG_COUNT 32

struct phx_reg_req {
	uint8_t addr;
	uint8_t count;    /* 1..PHX_MAX_REG_COUNT */
	uint8_t reserved[2];
} PHX_PACKED;
/*
 * WRITE_REG: the request is followed by count data bytes.
 * READ_REG response: count data bytes after phx_resp_status.
 */

/* STREAM_RX / STREAM_TX request: phx_u8_req, value 1 = enable, 0 = disable */
struct phx_stream_resp {
	int64_t counter;  /* Sample counter when the command was executed */
} PHX_PACKED;

/* EVENT (device to host, seq 0, no response) */
enum phx_event_code {
	PHX_EVENT_STREAM_ERROR = 1, /* Stream stopped by an internal error */
	PHX_EVENT_PA_SHUTDOWN = 2,  /* PA switched off by a safety condition */
};

struct phx_event {
	uint16_t code;    /* enum phx_event_code */
	uint16_t reserved;
	int32_t detail;   /* Event specific, e.g. a negative errno value */
} PHX_PACKED;

/* ------------------------------------------------------------------------ */
/* Sample channel                                                           */
/* ------------------------------------------------------------------------ */

#define PHX_SAMPLE_MAGIC 0x5350 /* "PS" */

enum phx_sample_type {
	PHX_SAMPLE_RX = 1, /* Device to host */
	PHX_SAMPLE_TX = 2, /* Host to device */
};

/* RX flags */
#define PHX_RX_FLAG_DISCONTINUITY (1u << 0) /* Samples were dropped before */

/*
 * Samples per packet. 360 samples of CS16 plus the header give a UDP
 * payload of 1472 bytes, the maximum without IP fragmentation on a
 * 1500 byte MTU.
 */
#define PHX_MAX_PACKET_SAMPLES 360

/* One complex sample, CS16: full scale +-32767 */
struct phx_cs16 {
	int16_t i;
	int16_t q;
} PHX_PACKED;

struct phx_sample_hdr {
	uint16_t magic;      /* PHX_SAMPLE_MAGIC */
	uint8_t version;     /* PHX_PROTO_VERSION */
	uint8_t type;        /* enum phx_sample_type */
	uint16_t flags;      /* PHX_RX_FLAG_* for RX, 0 for TX */
	uint16_t count;      /* Samples in this packet, 1..PHX_MAX_PACKET_SAMPLES */
	uint32_t seq;        /* Packet sequence number per direction */
	/* RX packets: cumulative device counters, 0 in TX packets */
	uint32_t rx_dropped; /* RX samples dropped in the device */
	uint32_t tx_late;    /* TX packets discarded as too late */
	uint32_t tx_early;   /* TX packets discarded as too early */
	int64_t timestamp;   /* Sample counter of the first sample */
} PHX_PACKED;
PHX_STATIC_ASSERT(sizeof(struct phx_sample_hdr) == 32, "phx_sample_hdr size");

#define PHX_MAX_SAMPLE_PACKET_BYTES                                                               \
	(sizeof(struct phx_sample_hdr) + PHX_MAX_PACKET_SAMPLES * sizeof(struct phx_cs16))
PHX_STATIC_ASSERT(PHX_MAX_SAMPLE_PACKET_BYTES == 1472, "sample packet size");

#endif /* PHENIX1255_PROTOCOL_H_ */
