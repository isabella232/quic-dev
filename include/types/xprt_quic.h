/*
 * include/types/xprt_quic.h
 * This file contains applet function prototypes
 *
 * Copyright 2019 HAProxy Technologies, Frédéric Lécaille <flecaille@haproxy.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation, version 2.1
 * exclusively.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifndef _TYPES_XPRT_QUIC_H
#define _TYPES_XPRT_QUIC_H

#include <sys/socket.h>

#include <common/mini-clist.h>

#include <types/quic.h>
#include <types/quic_tls.h>

#include <eb64tree.h>
#include <ebmbtree.h>

/* The maximum number of QUIC packets stored by the fd I/O handler by QUIC
 * connection. Must be a power of two.
 */
#define QUIC_CONN_MAX_PACKET  64

#define QUIC_STATELESS_RESET_TOKEN_LEN 16

/*
 * This struct is used by ebmb_node structs as last member of flexible array.
 * So do not change the order of the member of quic_cid struct.
 * <data> member must be the first one.
 */
struct quic_cid {
	unsigned char data[QUIC_CID_MAXLEN + sizeof(struct sockaddr_storage)];
	unsigned char len;
};

/* The data structure used to build a set of connection IDs for each connection. */
struct quic_connection_id {
	struct eb64_node seq_num;
	uint64_t retire_prior_to;
	struct quic_cid cid;
	unsigned char stateless_reset_token[QUIC_STATELESS_RESET_TOKEN_LEN];
};

struct preferred_address {
	uint16_t ipv4_port;
	uint16_t ipv6_port;
	uint8_t ipv4_addr[4];
	uint8_t ipv6_addr[16];
	struct quic_cid cid;
	uint8_t stateless_reset_token[QUIC_STATELESS_RESET_TOKEN_LEN];
};

/* Default values for some of transport parameters */
#define QUIC_DFLT_MAX_PACKET_SIZE     65527
#define QUIC_DFLT_ACK_DELAY_COMPONENT     3 /* milliseconds */
#define QUIC_DFLT_MAX_ACK_DELAY          25 /* milliseconds */

/* Types of QUIC transport parameters */
#define QUIC_TP_ORIGINAL_CONNECTION_ID               0
#define QUIC_TP_IDLE_TIMEOUT                         1
#define QUIC_TP_STATELESS_RESET_TOKEN                2
#define QUIC_TP_MAX_PACKET_SIZE                      3
#define QUIC_TP_INITIAL_MAX_DATA                     4
#define QUIC_TP_INITIAL_MAX_STREAM_DATA_BIDI_LOCAL   5
#define QUIC_TP_INITIAL_MAX_STREAM_DATA_BIDI_REMOTE  6
#define QUIC_TP_INITIAL_MAX_STREAM_DATA_UNI          7
#define QUIC_TP_INITIAL_MAX_STREAMS_BIDI             8
#define QUIC_TP_INITIAL_MAX_STREAMS_UNI              9
#define QUIC_TP_ACK_DELAY_EXPONENT                  10
#define QUIC_TP_MAX_ACK_DELAY                       11
#define QUIC_TP_DISABLE_ACTIVE_MIGRATION            12
#define QUIC_TP_PREFERRED_ADDRESS                   13
#define QUIC_TP_ACTIVE_CONNECTION_ID_LIMIT          14

/*
 * These defines are not for transport parameter type, but the maximum accepted value for
 * transport parameter types.
 */
#define QUIC_TP_ACK_DELAY_EXPONENT_LIMIT 20
#define QUIC_TP_MAX_ACK_DELAY_LIMIT      (1UL << 14)

/* The maximum length of encoded transport parameters for any QUIC peer. */
#define QUIC_TP_MAX_ENCLEN    128
/*
 * QUIC transport parameters.
 * Note that forbidden parameters sent by clients MUST generate TRANSPORT_PARAMETER_ERROR errors.
 */
struct quic_transport_params {
	uint64_t idle_timeout;
	uint64_t max_packet_size;                                      /* Default: 65527 (max of UDP payload for IPv6) */
	uint64_t initial_max_data;
	uint64_t initial_max_stream_data_bidi_local;
	uint64_t initial_max_stream_data_bidi_remote;
	uint64_t initial_max_stream_data_uni;
	uint64_t initial_max_streams_bidi;
	uint64_t initial_max_streams_uni;
	uint64_t ack_delay_exponent;                                   /* Default: 3, max: 20 */
	uint64_t max_ack_delay;                                        /* Default: 3ms, max: 2^14ms*/
	uint64_t active_connection_id_limit;

	/* Booleans */
	uint8_t disable_active_migration;
	uint8_t with_stateless_reset_token;
	uint8_t with_preferred_address;
	uint8_t with_original_connection_id;

	uint8_t stateless_reset_token[QUIC_STATELESS_RESET_TOKEN_LEN]; /* Forbidden for clients */
	struct quic_cid original_connection_id;                        /* Forbidden for clients */
	struct preferred_address preferred_address;                    /* Forbidden for clients */
};

/* Structure for ACK ranges sent in ACK frames. */
struct quic_ack_range {
	struct list list;
	int64_t first;
	int64_t last;
};

struct quic_ack_ranges {
	/* list of ACK ranges. */
	struct list list;
	/* The number of ACK ranges is this lists */
	size_t sz;
};

/* Flag the packet number space as requiring an ACK frame to be sent. */
#define QUIC_FL_PKTNS_ACK_REQUIRED  (1UL << 0)

/* QUIC packet number space */
struct quic_pktns {
	struct {
		/* Next packet number to use for transmissions. */
		int64_t next_pn;
	} tx;
	struct {
		/* Largest packet number */
		int64_t largest_pn;
		/* Largest acked packet number */
		int64_t largest_acked_pn;
		/* Number of ack-eliciting packets. */
		uint64_t nb_ack_eliciting;
		struct quic_ack_ranges ack_ranges;
	} rx;
	unsigned int flags;
};

/* The QUIC packet numbers are 62-bits integers */
#define QUIC_MAX_PACKET_NUM      ((1ULL << 62) - 1)

/* Default QUIC connection transport parameters */
extern struct quic_transport_params quid_dflt_transport_params;

/* Flag a received packet as being an ack-eliciting packet. */
#define QUIC_FL_RX_PACKET_ACK_ELICITING (1UL << 0)

struct quic_rx_packet {
	struct list list;
	int from_server;
	int long_header;
	unsigned char type;
	uint32_t version;
	/* Initial desctination connection ID. */
	struct quic_cid dcid;
	struct quic_cid scid;
	size_t pn_offset;
	/* Packet number */
	uint64_t pn;
	/* Packet number length */
	uint32_t pnl;
	uint64_t token_len;
	/* Packet length */
	uint64_t len;
	/* Additional authenticated data length */
	size_t aad_len;
	unsigned char data[QUIC_PACKET_MAXLEN];
	struct eb64_node pn_node;
	unsigned int flags;
};

/*
 * This structure is to store the CRYPTO frames information for outoing
 * QUIC packets. We make the assumption there is only one CRYPTO frame
 * by packet.
 */
struct quic_tx_crypto_frm {
	uint64_t offset;
	size_t len;
	/* Packet number this CRYPTO frame is attached to. */
	struct eb64_node pn;
};

#define QUIC_CRYPTO_BUF_SHIFT  14
#define QUIC_CRYPTO_BUF_MASK   ((1UL << QUIC_CRYPTO_BUF_SHIFT) - 1)
/* The maximum allowed size of CRYPTO data buffer provided by the TLS stack. */
#define QUIC_CRYPTO_BUF_SZ    (1UL << QUIC_CRYPTO_BUF_SHIFT) /* 16 KB */

/* The maximum number of bytes of CRYPTO data in flight during handshakes. */
#define QUIC_CRYPTO_IN_FLIGHT_MAX 4096

/*
 * CRYPTO buffer struct.
 * Such buffers are used to send CRYPTO data.
 */
struct quic_crypto_buf {
	unsigned char data[QUIC_CRYPTO_BUF_SZ];
	size_t sz;
};

/* QUIC buffer structure used to build outgoing packets. */
struct q_buf {
	/* Points to the data in this buffer. */
	unsigned char *area;
	/* Points to the current position to write into this buffer. */
	unsigned char *pos;
	/* Point to the end of this buffer past one. */
	const unsigned char *end;
	/* The number of data bytes in this buffer. */
	size_t data;
};

struct quic_enc_level {
	struct quic_tls_ctx tls_ctx;
	struct {
		/* The packets received by the listener I/O handler
		   with header protection removed. */
		struct eb_root qpkts;
		/* Liste of QUIC packets with protected header. */
		struct list pqpkts;
		/* Crypto frames */
		struct {
			uint64_t offset;
			struct eb_root frms; /* XXX TO CHECK XXX */
		} crypto;
	} rx;
	struct {
		struct {
			struct quic_crypto_buf **bufs;
			/* The number of element in use in the previous array. */
			size_t nb_buf;
			/* The total size of the CRYPTO data stored in the CRYPTO buffers. */
			size_t sz;
			/* The offset of the CRYPT0 data stream. */
			uint64_t offset;
			/* The outgoing CRYPTO frames ordered by packet number. */
			struct eb_root frms;
			/* The outgoing CRYPTO frames to be retransmitted ordered by packet number. */
			struct eb_root retransmit_frms;
		} crypto;
	} tx;
	struct quic_pktns *pktns;
};

/* The number of buffers for outgoing packets (must be a power of two). */
#define QUIC_CONN_TX_BUFS_NB 8
#define QUIC_CONN_TX_BUF_SZ  QUIC_PACKET_MAXLEN

struct quic_conn {
	uint32_t version;

	/* Initial DCID (comming with first Initial packets) */
	struct ebmb_node idcid_node;
	struct quic_cid idcid;

	struct quic_cid dcid;
	struct ebmb_node scid_node;
	struct quic_cid scid;
	struct eb_root cids;

	struct quic_enc_level enc_levels[QUIC_TLS_ENC_LEVEL_MAX];

	struct quic_transport_params *tx_tps;
	struct quic_transport_params rx_tps;

	struct quic_pktns pktns[QUIC_TLS_PKTNS_MAX];

	/* Used only to reach the tasklet for the I/O handler from this quic_conn object. */
	struct connection *conn;
	/* Output buffer used during the handshakes. */
	struct {
		unsigned char data[QUIC_PACKET_MAXLEN];
		unsigned char *pos;
	} obuf;

	struct {
		/* The packets which have been sent. */
		struct eb_root pkts;
		/* The remaining frames to send. */
		struct list frms_to_send;

		/* Array of buffers. */
		struct q_buf **bufs;
		/* The size of the previous array. */
		size_t nb_buf;
		/* Writer index. */
		int wbuf;
		/* Reader index. */
		int rbuf;
	} tx;

	size_t crypto_in_flight;
	int retransmit;
};

#endif /* _TYPES_XPRT_QUIC_H */