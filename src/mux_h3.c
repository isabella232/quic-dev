/*
 * HTTP/2 mux-demux for connections
 *
 * Copyright 2017 Willy Tarreau <w@1wt.eu>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <common/cfgparse.h>
#include <common/config.h>
#include <common/h1.h>
#include <common/h3.h>
#include <common/hpack-dec.h>
#include <common/hpack-enc.h>
#include <common/hpack-tbl.h>
#include <common/htx.h>
#include <common/initcall.h>
#include <common/net_helper.h>
#include <proto/connection.h>
#include <proto/http_htx.h>
#include <proto/trace.h>
#include <proto/session.h>
#include <proto/stream.h>
#include <proto/stream_interface.h>
#include <types/session.h>
#include <eb32tree.h>


/* dummy streams returned for closed, error, refused, idle and states */
static const struct h3s *h3_closed_stream;
static const struct h3s *h3_error_stream;
static const struct h3s *h3_refused_stream;
static const struct h3s *h3_idle_stream;

/* Connection flags (32 bit), in h3c->flags */
#define H3_CF_NONE              0x00000000

/* Flags indicating why writing to the mux is blocked. */
#define H3_CF_MUX_MALLOC        0x00000001  // mux blocked on lack of connection's mux buffer
#define H3_CF_MUX_MFULL         0x00000002  // mux blocked on connection's mux buffer full
#define H3_CF_MUX_BLOCK_ANY     0x00000003  // aggregate of the mux flags above

/* Flags indicating why writing to the demux is blocked.
 * The first two ones directly affect the ability for the mux to receive data
 * from the connection. The other ones affect the mux's ability to demux
 * received data.
 */
#define H3_CF_DEM_DALLOC        0x00000004  // demux blocked on lack of connection's demux buffer
#define H3_CF_DEM_DFULL         0x00000008  // demux blocked on connection's demux buffer full

#define H3_CF_DEM_MBUSY         0x00000010  // demux blocked on connection's mux side busy
#define H3_CF_DEM_MROOM         0x00000020  // demux blocked on lack of room in mux buffer
#define H3_CF_DEM_SALLOC        0x00000040  // demux blocked on lack of stream's request buffer
#define H3_CF_DEM_SFULL         0x00000080  // demux blocked on stream request buffer full
#define H3_CF_DEM_TOOMANY       0x00000100  // demux blocked waiting for some conn_streams to leave
#define H3_CF_DEM_BLOCK_ANY     0x000001F0  // aggregate of the demux flags above except DALLOC/DFULL

/* other flags */
#define H3_CF_GOAWAY_SENT       0x00001000  // a GOAWAY frame was successfully sent
#define H3_CF_GOAWAY_FAILED     0x00002000  // a GOAWAY frame failed to be sent
#define H3_CF_WAIT_FOR_HS       0x00004000  // We did check that at least a stream was waiting for handshake
#define H3_CF_IS_BACK           0x00008000  // this is an outgoing connection
#define H3_CF_WINDOW_OPENED     0x00010000 // demux increased window already advertised

/* H3 connection state, in h3c->st0 */
enum h3_cs {
	H3_CS_PREFACE,   // init done, waiting for connection preface
	H3_CS_SETTINGS1, // preface OK, waiting for first settings frame
	H3_CS_FRAME_H,   // first settings frame ok, waiting for frame header
	H3_CS_FRAME_P,   // frame header OK, waiting for frame payload
	H3_CS_FRAME_A,   // frame payload OK, trying to send ACK frame
	H3_CS_FRAME_E,   // frame payload OK, trying to send RST frame
	H3_CS_ERROR,     // send GOAWAY(errcode) and close the connection ASAP
	H3_CS_ERROR2,    // GOAWAY(errcode) sent, close the connection ASAP
	H3_CS_ENTRIES    // must be last
} __attribute__((packed));


/* 32 buffers: one for the ring's root, rest for the mbuf itself */
#define H3C_MBUF_CNT 32

/* H3 connection descriptor */
struct h3c {
	struct connection *conn;

	enum h3_cs st0; /* mux state */
	enum h3_err errcode; /* H3 err code (H3_ERR_*) */

	/* 16 bit hole here */
	uint32_t flags; /* connection flags: H3_CF_* */
	uint32_t streams_limit; /* maximum number of concurrent streams the peer supports */
	int32_t max_id; /* highest ID known on this connection, <0 before preface */
	uint32_t rcvd_c; /* newly received data to ACK for the connection */
	uint32_t rcvd_s; /* newly received data to ACK for the current stream (dsi) */

	/* states for the demux direction */
	struct hpack_dht *ddht; /* demux dynamic header table */
	struct buffer dbuf;    /* demux buffer */

	int32_t dsi; /* demux stream ID (<0 = idle) */
	int32_t dfl; /* demux frame length (if dsi >= 0) */
	int8_t  dft; /* demux frame type   (if dsi >= 0) */
	int8_t  dff; /* demux frame flags  (if dsi >= 0) */
	uint8_t dpl; /* demux pad length (part of dfl), init to 0 */
	/* 8 bit hole here */
	int32_t last_sid; /* last processed stream ID for GOAWAY, <0 before preface */

	/* states for the mux direction */
	struct buffer mbuf[H3C_MBUF_CNT];   /* mux buffers (ring) */
	int32_t msi; /* mux stream ID (<0 = idle) */
	int32_t mfl; /* mux frame length (if dsi >= 0) */
	int8_t  mft; /* mux frame type   (if dsi >= 0) */
	int8_t  mff; /* mux frame flags  (if dsi >= 0) */
	/* 16 bit hole here */
	int32_t miw; /* mux initial window size for all new streams */
	int32_t mws; /* mux window size. Can be negative. */
	int32_t mfs; /* mux's max frame size */

	int timeout;        /* idle timeout duration in ticks */
	int shut_timeout;   /* idle timeout duration in ticks after GOAWAY was sent */
	unsigned int nb_streams;  /* number of streams in the tree */
	unsigned int nb_cs;       /* number of attached conn_streams */
	unsigned int nb_reserved; /* number of reserved streams */
	unsigned int stream_cnt;  /* total number of streams seen */
	struct proxy *proxy; /* the proxy this connection was created for */
	struct task *task;  /* timeout management task */
	struct eb_root streams_by_id; /* all active streams by their ID */
	struct list send_list; /* list of blocked streams requesting to send */
	struct list fctl_list; /* list of streams blocked by connection's fctl */
	struct list blocked_list; /* list of streams blocked for other reasons (e.g. sfctl, dep) */
	struct buffer_wait buf_wait; /* wait list for buffer allocations */
	struct wait_event wait_event;  /* To be used if we're waiting for I/Os */
};

/* H3 stream state, in h3s->st */
enum h3_ss {
	H3_SS_IDLE = 0, // idle
	H3_SS_RLOC,     // reserved(local)
	H3_SS_RREM,     // reserved(remote)
	H3_SS_OPEN,     // open
	H3_SS_HREM,     // half-closed(remote)
	H3_SS_HLOC,     // half-closed(local)
	H3_SS_ERROR,    // an error needs to be sent using RST_STREAM
	H3_SS_CLOSED,   // closed
	H3_SS_ENTRIES   // must be last
} __attribute__((packed));

#define H3_SS_MASK(state) (1UL << (state))
#define H3_SS_IDLE_BIT    (1UL << H3_SS_IDLE)
#define H3_SS_RLOC_BIT    (1UL << H3_SS_RLOC)
#define H3_SS_RREM_BIT    (1UL << H3_SS_RREM)
#define H3_SS_OPEN_BIT    (1UL << H3_SS_OPEN)
#define H3_SS_HREM_BIT    (1UL << H3_SS_HREM)
#define H3_SS_HLOC_BIT    (1UL << H3_SS_HLOC)
#define H3_SS_ERROR_BIT   (1UL << H3_SS_ERROR)
#define H3_SS_CLOSED_BIT  (1UL << H3_SS_CLOSED)

/* HTTP/2 stream flags (32 bit), in h3s->flags */
#define H3_SF_NONE              0x00000000
#define H3_SF_ES_RCVD           0x00000001
#define H3_SF_ES_SENT           0x00000002

#define H3_SF_RST_RCVD          0x00000004 // received RST_STREAM
#define H3_SF_RST_SENT          0x00000008 // sent RST_STREAM

/* stream flags indicating the reason the stream is blocked */
#define H3_SF_BLK_MBUSY         0x00000010 // blocked waiting for mux access (transient)
#define H3_SF_BLK_MROOM         0x00000020 // blocked waiting for room in the mux (must be in send list)
#define H3_SF_BLK_MFCTL         0x00000040 // blocked due to mux fctl (must be in fctl list)
#define H3_SF_BLK_SFCTL         0x00000080 // blocked due to stream fctl (must be in blocked list)
#define H3_SF_BLK_ANY           0x000000F0 // any of the reasons above

/* stream flags indicating how data is supposed to be sent */
#define H3_SF_DATA_CLEN         0x00000100 // data sent using content-length
/* unused flags: 0x00000200, 0x00000400 */

#define H3_SF_NOTIFIED          0x00000800  // a paused stream was notified to try to send again
#define H3_SF_HEADERS_SENT      0x00001000  // a HEADERS frame was sent for this stream
#define H3_SF_OUTGOING_DATA     0x00002000  // set whenever we've seen outgoing data

#define H3_SF_HEADERS_RCVD      0x00004000  // a HEADERS frame was received for this stream

#define H3_SF_WANT_SHUTR        0x00008000  // a stream couldn't shutr() (mux full/busy)
#define H3_SF_WANT_SHUTW        0x00010000  // a stream couldn't shutw() (mux full/busy)
#define H3_SF_KILL_CONN         0x00020000  // kill the whole connection with this stream


/* H3 stream descriptor, describing the stream as it appears in the H3C, and as
 * it is being processed in the internal HTTP representation (H1 for now).
 */
struct h3s {
	struct conn_stream *cs;
	struct session *sess;
	struct h3c *h3c;
	struct h1m h1m;         /* request or response parser state for H1 */
	struct eb32_node by_id; /* place in h3c's streams_by_id */
	int32_t id; /* stream ID */
	uint32_t flags;      /* H3_SF_* */
	int sws;             /* stream window size, to be added to the mux's initial window size */
	enum h3_err errcode; /* H3 err code (H3_ERR_*) */
	enum h3_ss st;
	uint16_t status;     /* HTTP response status */
	unsigned long long body_len; /* remaining body length according to content-length if H3_SF_DATA_CLEN */
	struct buffer rxbuf; /* receive buffer, always valid (buf_empty or real buffer) */
	struct wait_event *subs;      /* recv wait_event the conn_stream associated is waiting on (via h3_subscribe) */
	struct list list; /* To be used when adding in h3c->send_list or h3c->fctl_lsit */
	struct tasklet *shut_tl;  /* deferred shutdown tasklet, to retry to send an RST after we failed to,
				   * in case there's no other subscription to do it */
};

/* descriptor for an h3 frame header */
struct h3_fh {
	uint32_t len;       /* length, host order, 24 bits */
	uint32_t sid;       /* stream id, host order, 31 bits */
	uint8_t ft;         /* frame type */
	uint8_t ff;         /* frame flags */
};

/* trace source and events */
static void h3_trace(enum trace_level level, uint64_t mask, \
                     const struct trace_source *src,
                     const struct ist where, const struct ist func,
                     const void *a1, const void *a2, const void *a3, const void *a4);

/* The event representation is split like this :
 *   strm  - application layer
 *   h3s   - internal H3 stream
 *   h3c   - internal H3 connection
 *   conn  - external connection
 *
 */
static const struct trace_event h3_trace_events[] = {
#define           H3_EV_H3C_NEW       (1ULL <<  0)
	{ .mask = H3_EV_H3C_NEW,      .name = "h3c_new",     .desc = "new H3 connection" },
#define           H3_EV_H3C_RECV      (1ULL <<  1)
	{ .mask = H3_EV_H3C_RECV,     .name = "h3c_recv",    .desc = "Rx on H3 connection" },
#define           H3_EV_H3C_SEND      (1ULL <<  2)
	{ .mask = H3_EV_H3C_SEND,     .name = "h3c_send",    .desc = "Tx on H3 connection" },
#define           H3_EV_H3C_FCTL      (1ULL <<  3)
	{ .mask = H3_EV_H3C_FCTL,     .name = "h3c_fctl",    .desc = "H3 connection flow-controlled" },
#define           H3_EV_H3C_BLK       (1ULL <<  4)
	{ .mask = H3_EV_H3C_BLK,      .name = "h3c_blk",     .desc = "H3 connection blocked" },
#define           H3_EV_H3C_WAKE      (1ULL <<  5)
	{ .mask = H3_EV_H3C_WAKE,     .name = "h3c_wake",    .desc = "H3 connection woken up" },
#define           H3_EV_H3C_END       (1ULL <<  6)
	{ .mask = H3_EV_H3C_END,      .name = "h3c_end",     .desc = "H3 connection terminated" },
#define           H3_EV_H3C_ERR       (1ULL <<  7)
	{ .mask = H3_EV_H3C_ERR,      .name = "h3c_err",     .desc = "error on H3 connection" },
#define           H3_EV_RX_FHDR       (1ULL <<  8)
	{ .mask = H3_EV_RX_FHDR,      .name = "rx_fhdr",     .desc = "H3 frame header received" },
#define           H3_EV_RX_FRAME      (1ULL <<  9)
	{ .mask = H3_EV_RX_FRAME,     .name = "rx_frame",    .desc = "receipt of any H3 frame" },
#define           H3_EV_RX_EOI        (1ULL << 10)
	{ .mask = H3_EV_RX_EOI,       .name = "rx_eoi",      .desc = "receipt of end of H3 input (ES or RST)" },
#define           H3_EV_RX_PREFACE    (1ULL << 11)
	{ .mask = H3_EV_RX_PREFACE,   .name = "rx_preface",  .desc = "receipt of H3 preface" },
#define           H3_EV_RX_DATA       (1ULL << 12)
	{ .mask = H3_EV_RX_DATA,      .name = "rx_data",     .desc = "receipt of H3 DATA frame" },
#define           H3_EV_RX_HDR        (1ULL << 13)
	{ .mask = H3_EV_RX_HDR,       .name = "rx_hdr",      .desc = "receipt of H3 HEADERS frame" },
#define           H3_EV_RX_PRIO       (1ULL << 14)
	{ .mask = H3_EV_RX_PRIO,      .name = "rx_prio",     .desc = "receipt of H3 PRIORITY frame" },
#define           H3_EV_RX_RST        (1ULL << 15)
	{ .mask = H3_EV_RX_RST,       .name = "rx_rst",      .desc = "receipt of H3 RST_STREAM frame" },
#define           H3_EV_RX_SETTINGS   (1ULL << 16)
	{ .mask = H3_EV_RX_SETTINGS,  .name = "rx_settings", .desc = "receipt of H3 SETTINGS frame" },
#define           H3_EV_RX_PUSH       (1ULL << 17)
	{ .mask = H3_EV_RX_PUSH,      .name = "rx_push",     .desc = "receipt of H3 PUSH_PROMISE frame" },
#define           H3_EV_RX_PING       (1ULL << 18)
	{ .mask = H3_EV_RX_PING,      .name = "rx_ping",     .desc = "receipt of H3 PING frame" },
#define           H3_EV_RX_GOAWAY     (1ULL << 19)
	{ .mask = H3_EV_RX_GOAWAY,    .name = "rx_goaway",   .desc = "receipt of H3 GOAWAY frame" },
#define           H3_EV_RX_WU         (1ULL << 20)
	{ .mask = H3_EV_RX_WU,        .name = "rx_wu",       .desc = "receipt of H3 WINDOW_UPDATE frame" },
#define           H3_EV_RX_CONT       (1ULL << 21)
	{ .mask = H3_EV_RX_CONT,      .name = "rx_cont",     .desc = "receipt of H3 CONTINUATION frame" },
#define           H3_EV_TX_FRAME      (1ULL << 22)
	{ .mask = H3_EV_TX_FRAME,     .name = "tx_frame",    .desc = "transmission of any H3 frame" },
#define           H3_EV_TX_EOI        (1ULL << 23)
	{ .mask = H3_EV_TX_EOI,       .name = "tx_eoi",      .desc = "transmission of H3 end of input (ES or RST)" },
#define           H3_EV_TX_PREFACE    (1ULL << 24)
	{ .mask = H3_EV_TX_PREFACE,   .name = "tx_preface",  .desc = "transmission of H3 preface" },
#define           H3_EV_TX_DATA       (1ULL << 25)
	{ .mask = H3_EV_TX_DATA,      .name = "tx_data",     .desc = "transmission of H3 DATA frame" },
#define           H3_EV_TX_HDR        (1ULL << 26)
	{ .mask = H3_EV_TX_HDR,       .name = "tx_hdr",      .desc = "transmission of H3 HEADERS frame" },
#define           H3_EV_TX_PRIO       (1ULL << 27)
	{ .mask = H3_EV_TX_PRIO,      .name = "tx_prio",     .desc = "transmission of H3 PRIORITY frame" },
#define           H3_EV_TX_RST        (1ULL << 28)
	{ .mask = H3_EV_TX_RST,       .name = "tx_rst",      .desc = "transmission of H3 RST_STREAM frame" },
#define           H3_EV_TX_SETTINGS   (1ULL << 29)
	{ .mask = H3_EV_TX_SETTINGS,  .name = "tx_settings", .desc = "transmission of H3 SETTINGS frame" },
#define           H3_EV_TX_PUSH       (1ULL << 30)
	{ .mask = H3_EV_TX_PUSH,      .name = "tx_push",     .desc = "transmission of H3 PUSH_PROMISE frame" },
#define           H3_EV_TX_PING       (1ULL << 31)
	{ .mask = H3_EV_TX_PING,      .name = "tx_ping",     .desc = "transmission of H3 PING frame" },
#define           H3_EV_TX_GOAWAY     (1ULL << 32)
	{ .mask = H3_EV_TX_GOAWAY,    .name = "tx_goaway",   .desc = "transmission of H3 GOAWAY frame" },
#define           H3_EV_TX_WU         (1ULL << 33)
	{ .mask = H3_EV_TX_WU,        .name = "tx_wu",       .desc = "transmission of H3 WINDOW_UPDATE frame" },
#define           H3_EV_TX_CONT       (1ULL << 34)
	{ .mask = H3_EV_TX_CONT,      .name = "tx_cont",     .desc = "transmission of H3 CONTINUATION frame" },
#define           H3_EV_H3S_NEW       (1ULL << 35)
	{ .mask = H3_EV_H3S_NEW,      .name = "h3s_new",     .desc = "new H3 stream" },
#define           H3_EV_H3S_RECV      (1ULL << 36)
	{ .mask = H3_EV_H3S_RECV,     .name = "h3s_recv",    .desc = "Rx for H3 stream" },
#define           H3_EV_H3S_SEND      (1ULL << 37)
	{ .mask = H3_EV_H3S_SEND,     .name = "h3s_send",    .desc = "Tx for H3 stream" },
#define           H3_EV_H3S_FCTL      (1ULL << 38)
	{ .mask = H3_EV_H3S_FCTL,     .name = "h3s_fctl",    .desc = "H3 stream flow-controlled" },
#define           H3_EV_H3S_BLK       (1ULL << 39)
	{ .mask = H3_EV_H3S_BLK,      .name = "h3s_blk",     .desc = "H3 stream blocked" },
#define           H3_EV_H3S_WAKE      (1ULL << 40)
	{ .mask = H3_EV_H3S_WAKE,     .name = "h3s_wake",    .desc = "H3 stream woken up" },
#define           H3_EV_H3S_END       (1ULL << 41)
	{ .mask = H3_EV_H3S_END,      .name = "h3s_end",     .desc = "H3 stream terminated" },
#define           H3_EV_H3S_ERR       (1ULL << 42)
	{ .mask = H3_EV_H3S_ERR,      .name = "h3s_err",     .desc = "error on H3 stream" },
#define           H3_EV_STRM_NEW      (1ULL << 43)
	{ .mask = H3_EV_STRM_NEW,     .name = "strm_new",    .desc = "app-layer stream creation" },
#define           H3_EV_STRM_RECV     (1ULL << 44)
	{ .mask = H3_EV_STRM_RECV,    .name = "strm_recv",   .desc = "receiving data for stream" },
#define           H3_EV_STRM_SEND     (1ULL << 45)
	{ .mask = H3_EV_STRM_SEND,    .name = "strm_send",   .desc = "sending data for stream" },
#define           H3_EV_STRM_FULL     (1ULL << 46)
	{ .mask = H3_EV_STRM_FULL,    .name = "strm_full",   .desc = "stream buffer full" },
#define           H3_EV_STRM_WAKE     (1ULL << 47)
	{ .mask = H3_EV_STRM_WAKE,    .name = "strm_wake",   .desc = "stream woken up" },
#define           H3_EV_STRM_SHUT     (1ULL << 48)
	{ .mask = H3_EV_STRM_SHUT,    .name = "strm_shut",   .desc = "stream shutdown" },
#define           H3_EV_STRM_END      (1ULL << 49)
	{ .mask = H3_EV_STRM_END,     .name = "strm_end",    .desc = "detaching app-layer stream" },
#define           H3_EV_STRM_ERR      (1ULL << 50)
	{ .mask = H3_EV_STRM_ERR,     .name = "strm_err",    .desc = "stream error" },
#define           H3_EV_PROTO_ERR     (1ULL << 51)
	{ .mask = H3_EV_PROTO_ERR,    .name = "proto_err",   .desc = "protocol error" },
	{ }
};

static const struct name_desc h3_trace_lockon_args[4] = {
	/* arg1 */ { /* already used by the connection */ },
	/* arg2 */ { .name="h3s", .desc="H3 stream" },
	/* arg3 */ { },
	/* arg4 */ { }
};

static const struct name_desc h3_trace_decoding[] = {
#define H3_VERB_CLEAN    1
	{ .name="clean",    .desc="only user-friendly stuff, generally suitable for level \"user\"" },
#define H3_VERB_MINIMAL  2
	{ .name="minimal",  .desc="report only h3c/h3s state and flags, no real decoding" },
#define H3_VERB_SIMPLE   3
	{ .name="simple",   .desc="add request/response status line or frame info when available" },
#define H3_VERB_ADVANCED 4
	{ .name="advanced", .desc="add header fields or frame decoding when available" },
#define H3_VERB_COMPLETE 5
	{ .name="complete", .desc="add full data dump when available" },
	{ /* end */ }
};

static struct trace_source trace_h3 = {
	.name = IST("h3"),
	.desc = "HTTP/2 multiplexer",
	.arg_def = TRC_ARG1_CONN,  // TRACE()'s first argument is always a connection
	.default_cb = h3_trace,
	.known_events = h3_trace_events,
	.lockon_args = h3_trace_lockon_args,
	.decoding = h3_trace_decoding,
	.report_events = ~0,  // report everything by default
};

#define TRACE_SOURCE &trace_h3
INITCALL1(STG_REGISTER, trace_register_source, TRACE_SOURCE);

/* the h3c connection pool */
DECLARE_STATIC_POOL(pool_head_h3c, "h3c", sizeof(struct h3c));

/* the h3s stream pool */
DECLARE_STATIC_POOL(pool_head_h3s, "h3s", sizeof(struct h3s));

/* The default connection window size is 65535, it may only be enlarged using
 * a WINDOW_UPDATE message. Since the window must never be larger than 2G-1,
 * we'll pretend we already received the difference between the two to send
 * an equivalent window update to enlarge it to 2G-1.
 */
#define H3_INITIAL_WINDOW_INCREMENT ((1U<<31)-1 - 65535)

/* maximum amount of data we're OK with re-aligning for buffer optimizations */
#define MAX_DATA_REALIGN 1024

/* a few settings from the global section */
static int h3_settings_header_table_size      =  4096; /* initial value */
static int h3_settings_initial_window_size    = 65535; /* initial value */
static unsigned int h3_settings_max_concurrent_streams = 100;
static int h3_settings_max_frame_size         = 0;     /* unset */

/* a dmumy closed stream */
static const struct h3s *h3_closed_stream = &(const struct h3s){
	.cs        = NULL,
	.h3c       = NULL,
	.st        = H3_SS_CLOSED,
	.errcode   = H3_ERR_STREAM_CLOSED,
	.flags     = H3_SF_RST_RCVD,
	.id        = 0,
};

/* a dmumy closed stream returning a PROTOCOL_ERROR error */
static const struct h3s *h3_error_stream = &(const struct h3s){
	.cs        = NULL,
	.h3c       = NULL,
	.st        = H3_SS_CLOSED,
	.errcode   = H3_ERR_PROTOCOL_ERROR,
	.flags     = 0,
	.id        = 0,
};

/* a dmumy closed stream returning a REFUSED_STREAM error */
static const struct h3s *h3_refused_stream = &(const struct h3s){
	.cs        = NULL,
	.h3c       = NULL,
	.st        = H3_SS_CLOSED,
	.errcode   = H3_ERR_REFUSED_STREAM,
	.flags     = 0,
	.id        = 0,
};

/* and a dummy idle stream for use with any unannounced stream */
static const struct h3s *h3_idle_stream = &(const struct h3s){
	.cs        = NULL,
	.h3c       = NULL,
	.st        = H3_SS_IDLE,
	.errcode   = H3_ERR_STREAM_CLOSED,
	.id        = 0,
};

static struct task *h3_timeout_task(struct task *t, void *context, unsigned short state);
static int h3_send(struct h3c *h3c);
static int h3_recv(struct h3c *h3c);
static int h3_process(struct h3c *h3c);
static struct task *h3_io_cb(struct task *t, void *ctx, unsigned short state);
static inline struct h3s *h3c_st_by_id(struct h3c *h3c, int id);
static int h3c_decode_headers(struct h3c *h3c, struct buffer *rxbuf, uint32_t *flags, unsigned long long *body_len);
static int h3_frt_transfer_data(struct h3s *h3s);
static struct task *h3_deferred_shut(struct task *t, void *ctx, unsigned short state);
static struct h3s *h3c_bck_stream_new(struct h3c *h3c, struct conn_stream *cs, struct session *sess);
static void h3s_alert(struct h3s *h3s);

/* returns a h3c state as an abbreviated 3-letter string, or "???" if unknown */
static inline const char *h3c_st_to_str(enum h3_cs st)
{
	switch (st) {
	case H3_CS_PREFACE:   return "PRF";
	case H3_CS_SETTINGS1: return "STG";
	case H3_CS_FRAME_H:   return "FRH";
	case H3_CS_FRAME_P:   return "FRP";
	case H3_CS_FRAME_A:   return "FRA";
	case H3_CS_FRAME_E:   return "FRE";
	case H3_CS_ERROR:     return "ERR";
	case H3_CS_ERROR2:    return "ER2";
	default:              return "???";
	}
}

/* returns a h3s state as an abbreviated 3-letter string, or "???" if unknown */
static inline const char *h3s_st_to_str(enum h3_ss st)
{
	switch (st) {
	case H3_SS_IDLE:   return "IDL"; // idle
	case H3_SS_RLOC:   return "RSL"; // reserved local
	case H3_SS_RREM:   return "RSR"; // reserved remote
	case H3_SS_OPEN:   return "OPN"; // open
	case H3_SS_HREM:   return "HCR"; // half-closed remote
	case H3_SS_HLOC:   return "HCL"; // half-closed local
	case H3_SS_ERROR : return "ERR"; // error
	case H3_SS_CLOSED: return "CLO"; // closed
	default:           return "???";
	}
}

/* the H3 traces always expect that arg1, if non-null, is of type connection
 * (from which we can derive h3c), that arg2, if non-null, is of type h3s, and
 * that arg3, if non-null, is either of type htx for tx headers, or of type
 * buffer for everything else.
 */
static void h3_trace(enum trace_level level, uint64_t mask, const struct trace_source *src,
                     const struct ist where, const struct ist func,
                     const void *a1, const void *a2, const void *a3, const void *a4)
{
	const struct connection *conn = a1;
	const struct h3c *h3c    = conn ? conn->ctx : NULL;
	const struct h3s *h3s    = a2;
	const struct buffer *buf = a3;
	const struct htx *htx;
	int pos;

	if (!h3c) // nothing to add
		return;

	if (src->verbosity > H3_VERB_CLEAN) {
		chunk_appendf(&trace_buf, " : h3c=%p(%c,%s)", h3c, conn_is_back(conn) ? 'B' : 'F', h3c_st_to_str(h3c->st0));

		if (h3c->errcode)
			chunk_appendf(&trace_buf, " err=%s/%02x", h3_err_str(h3c->errcode), h3c->errcode);

		if (h3c->dsi >= 0 &&
		    (mask & (H3_EV_RX_FRAME|H3_EV_RX_FHDR)) == (H3_EV_RX_FRAME|H3_EV_RX_FHDR)) {
			chunk_appendf(&trace_buf, " dft=%s/%02x", h3_ft_str(h3c->dft), h3c->dff);
		}

		if (h3s) {
			if (h3s->id <= 0)
				chunk_appendf(&trace_buf, " dsi=%d", h3c->dsi);
			chunk_appendf(&trace_buf, " h3s=%p(%d,%s)", h3s, h3s->id, h3s_st_to_str(h3s->st));
			if (h3s->id && h3s->errcode)
				chunk_appendf(&trace_buf, " err=%s/%02x", h3_err_str(h3s->errcode), h3s->errcode);
		}
	}

	/* Let's dump decoded requests and responses right after parsing. They
	 * are traced at level USER with a few recognizable flags.
	 */
	if ((mask == (H3_EV_RX_FRAME|H3_EV_RX_HDR|H3_EV_STRM_NEW) ||
	     mask == (H3_EV_RX_FRAME|H3_EV_RX_HDR)) && buf)
		htx = htxbuf(buf); // recv req/res
	else if (mask == (H3_EV_TX_FRAME|H3_EV_TX_HDR))
		htx = a3; // send req/res
	else
		htx = NULL;

	if (level == TRACE_LEVEL_USER && src->verbosity != H3_VERB_MINIMAL && htx && (pos = htx_get_head(htx)) != -1) {
		const struct htx_blk    *blk  = htx_get_blk(htx, pos);
		const struct htx_sl     *sl   = htx_get_blk_ptr(htx, blk);
		enum htx_blk_type        type = htx_get_blk_type(blk);

		if (type == HTX_BLK_REQ_SL)
			chunk_appendf(&trace_buf, " : [%d] H3 REQ: %.*s %.*s %.*s",
				      h3s ? h3s->id : h3c->dsi,
				      HTX_SL_P1_LEN(sl), HTX_SL_P1_PTR(sl),
				      HTX_SL_P2_LEN(sl), HTX_SL_P2_PTR(sl),
				      HTX_SL_P3_LEN(sl), HTX_SL_P3_PTR(sl));
		else if (type == HTX_BLK_RES_SL)
			chunk_appendf(&trace_buf, " : [%d] H3 RES: %.*s %.*s %.*s",
				      h3s ? h3s->id : h3c->dsi,
				      HTX_SL_P1_LEN(sl), HTX_SL_P1_PTR(sl),
				      HTX_SL_P2_LEN(sl), HTX_SL_P2_PTR(sl),
				      HTX_SL_P3_LEN(sl), HTX_SL_P3_PTR(sl));
	}
}

/* returns true if the connection is allowed to expire, false otherwise. A
 * connection may expire when:
 *   - it has no stream
 *   - it has data in the mux buffer
 *   - it has streams in the blocked list
 *   - it has streams in the fctl list
 *   - it has streams in the send list
 * Otherwise it means some streams are waiting in the data layer and it should
 * not expire.
 */
static inline int h3c_may_expire(const struct h3c *h3c)
{
	return eb_is_empty(&h3c->streams_by_id) ||
	       br_data(h3c->mbuf) ||
	       !LIST_ISEMPTY(&h3c->blocked_list) ||
	       !LIST_ISEMPTY(&h3c->fctl_list) ||
	       !LIST_ISEMPTY(&h3c->send_list);
}

static __inline int
h3c_is_dead(const struct h3c *h3c)
{
	if (eb_is_empty(&h3c->streams_by_id) &&     /* don't close if streams exist */
	    ((h3c->conn->flags & CO_FL_ERROR) ||    /* errors close immediately */
	     (h3c->st0 >= H3_CS_ERROR && !h3c->task) || /* a timeout stroke earlier */
	     (!(h3c->conn->owner)) || /* Nobody's left to take care of the connection, drop it now */
	     (!br_data(h3c->mbuf) &&  /* mux buffer empty, also process clean events below */
	      (conn_xprt_read0_pending(h3c->conn) ||
	       (h3c->last_sid >= 0 && h3c->max_id >= h3c->last_sid)))))
		return 1;

	return 0;
}

/*****************************************************/
/* functions below are for dynamic buffer management */
/*****************************************************/

/* indicates whether or not the we may call the h3_recv() function to attempt
 * to receive data into the buffer and/or demux pending data. The condition is
 * a bit complex due to some API limits for now. The rules are the following :
 *   - if an error or a shutdown was detected on the connection and the buffer
 *     is empty, we must not attempt to receive
 *   - if the demux buf failed to be allocated, we must not try to receive and
 *     we know there is nothing pending
 *   - if no flag indicates a blocking condition, we may attempt to receive,
 *     regardless of whether the demux buffer is full or not, so that only
 *     de demux part decides whether or not to block. This is needed because
 *     the connection API indeed prevents us from re-enabling receipt that is
 *     already enabled in a polled state, so we must always immediately stop
 *     as soon as the demux can't proceed so as never to hit an end of read
 *     with data pending in the buffers.
 *   - otherwise must may not attempt
 */
static inline int h3_recv_allowed(const struct h3c *h3c)
{
	if (b_data(&h3c->dbuf) == 0 &&
	    (h3c->st0 >= H3_CS_ERROR ||
	     h3c->conn->flags & CO_FL_ERROR ||
	     conn_xprt_read0_pending(h3c->conn)))
		return 0;

	if (!(h3c->flags & H3_CF_DEM_DALLOC) &&
	    !(h3c->flags & H3_CF_DEM_BLOCK_ANY))
		return 1;

	return 0;
}

/* restarts reading on the connection if it was not enabled */
static inline void h3c_restart_reading(const struct h3c *h3c, int consider_buffer)
{
	if (!h3_recv_allowed(h3c))
		return;
	if ((!consider_buffer || !b_data(&h3c->dbuf))
	    && (h3c->wait_event.events & SUB_RETRY_RECV))
		return;
	//tasklet_wakeup(h3c->wait_event.tasklet);
}


/* returns true if the front connection has too many conn_streams attached */
static inline int h3_frt_has_too_many_cs(const struct h3c *h3c)
{
	return h3c->nb_cs > h3_settings_max_concurrent_streams;
}

/* Tries to grab a buffer and to re-enable processing on mux <target>. The h3c
 * flags are used to figure what buffer was requested. It returns 1 if the
 * allocation succeeds, in which case the connection is woken up, or 0 if it's
 * impossible to wake up and we prefer to be woken up later.
 */
static int h3_buf_available(void *target)
{
	struct h3c *h3c = target;
	struct h3s *h3s;

	if ((h3c->flags & H3_CF_DEM_DALLOC) && b_alloc_margin(&h3c->dbuf, 0)) {
		h3c->flags &= ~H3_CF_DEM_DALLOC;
		h3c_restart_reading(h3c, 1);
		return 1;
	}

	if ((h3c->flags & H3_CF_MUX_MALLOC) && b_alloc_margin(br_tail(h3c->mbuf), 0)) {
		h3c->flags &= ~H3_CF_MUX_MALLOC;

		if (h3c->flags & H3_CF_DEM_MROOM) {
			h3c->flags &= ~H3_CF_DEM_MROOM;
			h3c_restart_reading(h3c, 1);
		}
		return 1;
	}

	if ((h3c->flags & H3_CF_DEM_SALLOC) &&
	    (h3s = h3c_st_by_id(h3c, h3c->dsi)) && h3s->cs &&
	    b_alloc_margin(&h3s->rxbuf, 0)) {
		h3c->flags &= ~H3_CF_DEM_SALLOC;
		h3c_restart_reading(h3c, 1);
		return 1;
	}

	return 0;
}

static inline struct buffer *h3_get_buf(struct h3c *h3c, struct buffer *bptr)
{
	struct buffer *buf = NULL;

	if (likely(!MT_LIST_ADDED(&h3c->buf_wait.list)) &&
	    unlikely((buf = b_alloc_margin(bptr, 0)) == NULL)) {
		h3c->buf_wait.target = h3c;
		h3c->buf_wait.wakeup_cb = h3_buf_available;
		MT_LIST_ADDQ(&buffer_wq, &h3c->buf_wait.list);
	}
	return buf;
}

static inline void h3_release_buf(struct h3c *h3c, struct buffer *bptr)
{
	if (bptr->size) {
		b_free(bptr);
		offer_buffers(NULL, tasks_run_queue);
	}
}

static inline void h3_release_mbuf(struct h3c *h3c)
{
	struct buffer *buf;
	unsigned int count = 0;

	while (b_size(buf = br_head_pick(h3c->mbuf))) {
		b_free(buf);
		count++;
	}
	if (count)
		offer_buffers(NULL, tasks_run_queue);
}

/* returns the number of allocatable outgoing streams for the connection taking
 * the last_sid and the reserved ones into account.
 */
static inline int h3_streams_left(const struct h3c *h3c)
{
	int ret;

	/* consider the number of outgoing streams we're allowed to create before
	 * reaching the last GOAWAY frame seen. max_id is the last assigned id,
	 * nb_reserved is the number of streams which don't yet have an ID.
	 */
	ret = (h3c->last_sid >= 0) ? h3c->last_sid : 0x7FFFFFFF;
	ret = (unsigned int)(ret - h3c->max_id) / 2 - h3c->nb_reserved - 1;
	if (ret < 0)
		ret = 0;
	return ret;
}

/* returns the number of streams in use on a connection to figure if it's
 * idle or not. We check nb_cs and not nb_streams as the caller will want
 * to know if it was the last one after a detach().
 */
static int h3_used_streams(struct connection *conn)
{
	struct h3c *h3c = conn->ctx;

	return h3c->nb_cs;
}

/* returns the number of concurrent streams available on the connection */
static int h3_avail_streams(struct connection *conn)
{
	struct server *srv = objt_server(conn->target);
	struct h3c *h3c = conn->ctx;
	int ret1, ret2;

	/* RFC7540#6.8: Receivers of a GOAWAY frame MUST NOT open additional
	 * streams on the connection.
	 */
	if (h3c->last_sid >= 0)
		return 0;

	if (h3c->st0 >= H3_CS_ERROR)
		return 0;

	/* note: may be negative if a SETTINGS frame changes the limit */
	ret1 = h3c->streams_limit - h3c->nb_streams;

	/* we must also consider the limit imposed by stream IDs */
	ret2 = h3_streams_left(h3c);
	ret1 = MIN(ret1, ret2);
	if (ret1 > 0 && srv && srv->max_reuse >= 0) {
		ret2 = h3c->stream_cnt <= srv->max_reuse ? srv->max_reuse - h3c->stream_cnt + 1: 0;
		ret1 = MIN(ret1, ret2);
	}
	return ret1;
}


/*****************************************************************/
/* functions below are dedicated to the mux setup and management */
/*****************************************************************/

/* Initialize the mux once it's attached. For outgoing connections, the context
 * is already initialized before installing the mux, so we detect incoming
 * connections from the fact that the context is still NULL (even during mux
 * upgrades). <input> is always used as Input buffer and may contain data. It is
 * the caller responsibility to not reuse it anymore. Returns < 0 on error.
 */
static int h3_init(struct connection *conn, struct proxy *prx, struct session *sess,
		   struct buffer *input)
{
	struct h3c *h3c;
	struct task *t = NULL;
	void *conn_ctx = conn->ctx;

	TRACE_ENTER(H3_EV_H3C_NEW);

	h3c = pool_alloc(pool_head_h3c);
	if (!h3c)
		goto fail_no_h3c;

	if (conn_is_back(conn)) {
		h3c->flags = H3_CF_IS_BACK;
		h3c->shut_timeout = h3c->timeout = prx->timeout.server;
		if (tick_isset(prx->timeout.serverfin))
			h3c->shut_timeout = prx->timeout.serverfin;
	} else {
		h3c->flags = H3_CF_NONE;
		h3c->shut_timeout = h3c->timeout = prx->timeout.client;
		if (tick_isset(prx->timeout.clientfin))
			h3c->shut_timeout = prx->timeout.clientfin;
	}

	h3c->proxy = prx;
	h3c->task = NULL;
	if (tick_isset(h3c->timeout)) {
		t = task_new(tid_bit);
		if (!t)
			goto fail;

		h3c->task = t;
		t->process = h3_timeout_task;
		t->context = h3c;
		t->expire = tick_add(now_ms, h3c->timeout);
	}

	h3c->wait_event.tasklet = tasklet_new();
	if (!h3c->wait_event.tasklet)
		goto fail;
	h3c->wait_event.tasklet->process = h3_io_cb;
	h3c->wait_event.tasklet->context = h3c;
	h3c->wait_event.events = 0;

	h3c->ddht = hpack_dht_alloc(h3_settings_header_table_size);
	if (!h3c->ddht)
		goto fail;

	/* Initialise the context. */
	h3c->st0 = H3_CS_PREFACE;
	h3c->conn = conn;
	h3c->streams_limit = h3_settings_max_concurrent_streams;
	h3c->max_id = -1;
	h3c->errcode = H3_ERR_NO_ERROR;
	h3c->rcvd_c = 0;
	h3c->rcvd_s = 0;
	h3c->nb_streams = 0;
	h3c->nb_cs = 0;
	h3c->nb_reserved = 0;
	h3c->stream_cnt = 0;

	h3c->dbuf = *input;
	h3c->dsi = -1;
	h3c->msi = -1;

	h3c->last_sid = -1;

	br_init(h3c->mbuf, sizeof(h3c->mbuf) / sizeof(h3c->mbuf[0]));
	h3c->miw = 65535; /* mux initial window size */
	h3c->mws = 65535; /* mux window size */
	h3c->mfs = 16384; /* initial max frame size */
	h3c->streams_by_id = EB_ROOT;
	LIST_INIT(&h3c->send_list);
	LIST_INIT(&h3c->fctl_list);
	LIST_INIT(&h3c->blocked_list);
	MT_LIST_INIT(&h3c->buf_wait.list);

	conn->ctx = h3c;

	if (t)
		task_queue(t);

	if (h3c->flags & H3_CF_IS_BACK) {
		/* FIXME: this is temporary, for outgoing connections we need
		 * to immediately allocate a stream until the code is modified
		 * so that the caller calls ->attach(). For now the outgoing cs
		 * is stored as conn->ctx by the caller and saved in conn_ctx.
		 */
		struct h3s *h3s;

		h3s = h3c_bck_stream_new(h3c, conn_ctx, sess);
		if (!h3s)
			goto fail_stream;
	}

	/* prepare to read something */
	h3c_restart_reading(h3c, 1);
	TRACE_LEAVE(H3_EV_H3C_NEW, conn);
	return 0;
  fail_stream:
	hpack_dht_free(h3c->ddht);
  fail:
	task_destroy(t);
	if (h3c->wait_event.tasklet)
		tasklet_free(h3c->wait_event.tasklet);
	pool_free(pool_head_h3c, h3c);
  fail_no_h3c:
	conn->ctx = conn_ctx; /* restore saved ctx */
	TRACE_DEVEL("leaving in error", H3_EV_H3C_NEW|H3_EV_H3C_END|H3_EV_H3C_ERR);
	return -1;
}

/* returns the next allocatable outgoing stream ID for the H3 connection, or
 * -1 if no more is allocatable.
 */
static inline int32_t h3c_get_next_sid(const struct h3c *h3c)
{
	int32_t id = (h3c->max_id + 1) | 1;

	if ((id & 0x80000000U) || (h3c->last_sid >= 0 && id > h3c->last_sid))
		id = -1;
	return id;
}

/* returns the stream associated with id <id> or NULL if not found */
static inline struct h3s *h3c_st_by_id(struct h3c *h3c, int id)
{
	struct eb32_node *node;

	if (id == 0)
		return (struct h3s *)h3_closed_stream;

	if (id > h3c->max_id)
		return (struct h3s *)h3_idle_stream;

	node = eb32_lookup(&h3c->streams_by_id, id);
	if (!node)
		return (struct h3s *)h3_closed_stream;

	return container_of(node, struct h3s, by_id);
}

/* release function. This one should be called to free all resources allocated
 * to the mux.
 */
static void h3_release(struct h3c *h3c)
{
	struct connection *conn = NULL;;

	TRACE_ENTER(H3_EV_H3C_END);

	if (h3c) {
		/* The connection must be aattached to this mux to be released */
		if (h3c->conn && h3c->conn->ctx == h3c)
			conn = h3c->conn;

		TRACE_DEVEL("freeing h3c", H3_EV_H3C_END, conn);
		hpack_dht_free(h3c->ddht);

		if (MT_LIST_ADDED(&h3c->buf_wait.list))
			MT_LIST_DEL(&h3c->buf_wait.list);

		h3_release_buf(h3c, &h3c->dbuf);
		h3_release_mbuf(h3c);

		if (h3c->task) {
			h3c->task->context = NULL;
			task_wakeup(h3c->task, TASK_WOKEN_OTHER);
			h3c->task = NULL;
		}
		if (h3c->wait_event.tasklet)
			tasklet_free(h3c->wait_event.tasklet);
		if (conn && h3c->wait_event.events != 0)
			conn->xprt->unsubscribe(conn, conn->xprt_ctx, h3c->wait_event.events,
						&h3c->wait_event);

		pool_free(pool_head_h3c, h3c);
	}

	if (conn) {
		conn->mux = NULL;
		conn->ctx = NULL;
		TRACE_DEVEL("freeing conn", H3_EV_H3C_END, conn);

		conn_stop_tracking(conn);
		conn_full_close(conn);
		if (conn->destroy_cb)
			conn->destroy_cb(conn);
		conn_free(conn);
	}

	TRACE_LEAVE(H3_EV_H3C_END);
}


/******************************************************/
/* functions below are for the H3 protocol processing */
/******************************************************/

/* returns the stream if of stream <h3s> or 0 if <h3s> is NULL */
static inline __maybe_unused int h3s_id(const struct h3s *h3s)
{
	return h3s ? h3s->id : 0;
}

/* returns the sum of the stream's own window size and the mux's initial
 * window, which together form the stream's effective window size.
 */
static inline int h3s_mws(const struct h3s *h3s)
{
	return h3s->sws + h3s->h3c->miw;
}

/* returns true of the mux is currently busy as seen from stream <h3s> */
static inline __maybe_unused int h3c_mux_busy(const struct h3c *h3c, const struct h3s *h3s)
{
	if (h3c->msi < 0)
		return 0;

	if (h3c->msi == h3s_id(h3s))
		return 0;

	return 1;
}

/* marks an error on the connection */
static inline __maybe_unused void h3c_error(struct h3c *h3c, enum h3_err err)
{
	TRACE_POINT(H3_EV_H3C_ERR, h3c->conn,,, (void *)(long)(err));
	h3c->errcode = err;
	h3c->st0 = H3_CS_ERROR;
}

/* marks an error on the stream. It may also update an already closed stream
 * (e.g. to report an error after an RST was received).
 */
static inline __maybe_unused void h3s_error(struct h3s *h3s, enum h3_err err)
{
	if (h3s->id && h3s->st != H3_SS_ERROR) {
		TRACE_POINT(H3_EV_H3S_ERR, h3s->h3c->conn, h3s,, (void *)(long)(err));
		h3s->errcode = err;
		if (h3s->st < H3_SS_ERROR)
			h3s->st = H3_SS_ERROR;
		if (h3s->cs)
			cs_set_error(h3s->cs);
	}
}

/* attempt to notify the data layer of recv availability */
static void __maybe_unused h3s_notify_recv(struct h3s *h3s)
{
	if (h3s->subs && h3s->subs->events & SUB_RETRY_RECV) {
		TRACE_POINT(H3_EV_STRM_WAKE, h3s->h3c->conn, h3s);
		//tasklet_wakeup(h3s->subs->tasklet);
		h3s->subs->events &= ~SUB_RETRY_RECV;
		if (!h3s->subs->events)
			h3s->subs = NULL;
	}
}

/* attempt to notify the data layer of send availability */
static void __maybe_unused h3s_notify_send(struct h3s *h3s)
{
	if (h3s->subs && h3s->subs->events & SUB_RETRY_SEND) {
		TRACE_POINT(H3_EV_STRM_WAKE, h3s->h3c->conn, h3s);
		h3s->flags |= H3_SF_NOTIFIED;
		//tasklet_wakeup(h3s->subs->tasklet);
		h3s->subs->events &= ~SUB_RETRY_SEND;
		if (!h3s->subs->events)
			h3s->subs = NULL;
	}
	else if (h3s->flags & (H3_SF_WANT_SHUTR | H3_SF_WANT_SHUTW)) {
		TRACE_POINT(H3_EV_STRM_WAKE, h3s->h3c->conn, h3s);
		//tasklet_wakeup(h3s->shut_tl);
	}
}

/* alerts the data layer, trying to wake it up by all means, following
 * this sequence :
 *   - if the h3s' data layer is subscribed to recv, then it's woken up for recv
 *   - if its subscribed to send, then it's woken up for send
 *   - if it was subscribed to neither, its ->wake() callback is called
 * It is safe to call this function with a closed stream which doesn't have a
 * conn_stream anymore.
 */
static void __maybe_unused h3s_alert(struct h3s *h3s)
{
	TRACE_ENTER(H3_EV_H3S_WAKE, h3s->h3c->conn, h3s);

	if (h3s->subs ||
	    (h3s->flags & (H3_SF_WANT_SHUTR | H3_SF_WANT_SHUTW))) {
		h3s_notify_recv(h3s);
		h3s_notify_send(h3s);
	}
	else if (h3s->cs && h3s->cs->data_cb->wake != NULL) {
		TRACE_POINT(H3_EV_STRM_WAKE, h3s->h3c->conn, h3s);
		h3s->cs->data_cb->wake(h3s->cs);
	}

	TRACE_LEAVE(H3_EV_H3S_WAKE, h3s->h3c->conn, h3s);
}

/* writes the 24-bit frame size <len> at address <frame> */
static inline __maybe_unused void h3_set_frame_size(void *frame, uint32_t len)
{
	uint8_t *out = frame;

	*out = len >> 16;
	write_n16(out + 1, len);
}

/* reads <bytes> bytes from buffer <b> starting at relative offset <o> from the
 * current pointer, dealing with wrapping, and stores the result in <dst>. It's
 * the caller's responsibility to verify that there are at least <bytes> bytes
 * available in the buffer's input prior to calling this function. The buffer
 * is assumed not to hold any output data.
 */
static inline __maybe_unused void h3_get_buf_bytes(void *dst, size_t bytes,
                                    const struct buffer *b, int o)
{
	readv_bytes(dst, bytes, b_peek(b, o), b_wrap(b) - b_peek(b, o), b_orig(b));
}

static inline __maybe_unused uint16_t h3_get_n16(const struct buffer *b, int o)
{
	return readv_n16(b_peek(b, o), b_wrap(b) - b_peek(b, o), b_orig(b));
}

static inline __maybe_unused uint32_t h3_get_n32(const struct buffer *b, int o)
{
	return readv_n32(b_peek(b, o), b_wrap(b) - b_peek(b, o), b_orig(b));
}

static inline __maybe_unused uint64_t h3_get_n64(const struct buffer *b, int o)
{
	return readv_n64(b_peek(b, o), b_wrap(b) - b_peek(b, o), b_orig(b));
}


/* Peeks an H3 frame header from offset <o> of buffer <b> into descriptor <h>.
 * The algorithm is not obvious. It turns out that H3 headers are neither
 * aligned nor do they use regular sizes. And to add to the trouble, the buffer
 * may wrap so each byte read must be checked. The header is formed like this :
 *
 *       b0         b1       b2     b3   b4         b5..b8
 *  +----------+---------+--------+----+----+----------------------+
 *  |len[23:16]|len[15:8]|len[7:0]|type|flag|sid[31:0] (big endian)|
 *  +----------+---------+--------+----+----+----------------------+
 *
 * Here we read a big-endian 64 bit word from h[1]. This way in a single read
 * we get the sid properly aligned and ordered, and 16 bits of len properly
 * ordered as well. The type and flags can be extracted using bit shifts from
 * the word, and only one extra read is needed to fetch len[16:23].
 * Returns zero if some bytes are missing, otherwise non-zero on success. The
 * buffer is assumed not to contain any output data.
 */
static __maybe_unused int h3_peek_frame_hdr(const struct buffer *b, int o, struct h3_fh *h)
{
	uint64_t w;

	if (b_data(b) < o + 9)
		return 0;

	w = h3_get_n64(b, o + 1);
	h->len = *(uint8_t*)b_peek(b, o) << 16;
	h->sid = w & 0x7FFFFFFF; /* RFC7540#4.1: R bit must be ignored */
	h->ff = w >> 32;
	h->ft = w >> 40;
	h->len += w >> 48;
	return 1;
}

/* skip the next 9 bytes corresponding to the frame header possibly parsed by
 * h3_peek_frame_hdr() above.
 */
static inline __maybe_unused void h3_skip_frame_hdr(struct buffer *b)
{
	b_del(b, 9);
}

/* same as above, automatically advances the buffer on success */
static inline __maybe_unused int h3_get_frame_hdr(struct buffer *b, struct h3_fh *h)
{
	int ret;

	ret = h3_peek_frame_hdr(b, 0, h);
	if (ret > 0)
		h3_skip_frame_hdr(b);
	return ret;
}


/* try to fragment the headers frame present at the beginning of buffer <b>,
 * enforcing a limit of <mfs> bytes per frame. Returns 0 on failure, 1 on
 * success. Typical causes of failure include a buffer not large enough to
 * add extra frame headers. The existing frame size is read in the current
 * frame. Its EH flag will be cleared if CONTINUATION frames need to be added,
 * and its length will be adjusted. The stream ID for continuation frames will
 * be copied from the initial frame's.
 */
static int h3_fragment_headers(struct buffer *b, uint32_t mfs)
{
	size_t remain    = b->data - 9;
	int extra_frames = (remain - 1) / mfs;
	size_t fsize;
	char *fptr;
	int frame;

	if (b->data <= mfs + 9)
		return 1;

	/* Too large a frame, we need to fragment it using CONTINUATION
	 * frames. We start from the end and move tails as needed.
	 */
	if (b->data + extra_frames * 9 > b->size)
		return 0;

	for (frame = extra_frames; frame; frame--) {
		fsize = ((remain - 1) % mfs) + 1;
		remain -= fsize;

		/* move data */
		fptr = b->area + 9 + remain + (frame - 1) * 9;
		memmove(fptr + 9, b->area + 9 + remain, fsize);
		b->data += 9;

		/* write new frame header */
		h3_set_frame_size(fptr, fsize);
		fptr[3] = H3_FT_CONTINUATION;
		fptr[4] = (frame == extra_frames) ? H3_F_HEADERS_END_HEADERS : 0;
		write_n32(fptr + 5, read_n32(b->area + 5));
	}

	b->area[4] &= ~H3_F_HEADERS_END_HEADERS;
	h3_set_frame_size(b->area, remain);
	return 1;
}


/* marks stream <h3s> as CLOSED and decrement the number of active streams for
 * its connection if the stream was not yet closed. Please use this exclusively
 * before closing a stream to ensure stream count is well maintained.
 */
static inline void h3s_close(struct h3s *h3s)
{
	if (h3s->st != H3_SS_CLOSED) {
		TRACE_ENTER(H3_EV_H3S_END, h3s->h3c->conn, h3s);
		h3s->h3c->nb_streams--;
		if (!h3s->id)
			h3s->h3c->nb_reserved--;
		if (h3s->cs) {
			if (!(h3s->cs->flags & CS_FL_EOS) && !b_data(&h3s->rxbuf))
				h3s_notify_recv(h3s);
		}
		TRACE_LEAVE(H3_EV_H3S_END, h3s->h3c->conn, h3s);
	}
	h3s->st = H3_SS_CLOSED;
}

/* detaches an H3 stream from its H3C and releases it to the H3S pool. */
/* h3s_destroy should only ever be called by the thread that owns the stream,
 * that means that a tasklet should be used if we want to destroy the h3s
 * from another thread
 */
static void h3s_destroy(struct h3s *h3s)
{
	struct connection *conn = h3s->h3c->conn;

	TRACE_ENTER(H3_EV_H3S_END, conn, h3s);

	h3s_close(h3s);
	eb32_delete(&h3s->by_id);
	if (b_size(&h3s->rxbuf)) {
		b_free(&h3s->rxbuf);
		offer_buffers(NULL, tasks_run_queue);
	}

	if (h3s->subs)
		h3s->subs->events = 0;

	/* There's no need to explicitly call unsubscribe here, the only
	 * reference left would be in the h3c send_list/fctl_list, and if
	 * we're in it, we're getting out anyway
	 */
	LIST_DEL_INIT(&h3s->list);

	/* ditto, calling tasklet_free() here should be ok */
	tasklet_free(h3s->shut_tl);
	pool_free(pool_head_h3s, h3s);

	TRACE_LEAVE(H3_EV_H3S_END, conn);
}

/* allocates a new stream <id> for connection <h3c> and adds it into h3c's
 * stream tree. In case of error, nothing is added and NULL is returned. The
 * causes of errors can be any failed memory allocation. The caller is
 * responsible for checking if the connection may support an extra stream
 * prior to calling this function.
 */
static struct h3s *h3s_new(struct h3c *h3c, int id)
{
	struct h3s *h3s;

	TRACE_ENTER(H3_EV_H3S_NEW, h3c->conn);

	h3s = pool_alloc(pool_head_h3s);
	if (!h3s)
		goto out;

	h3s->shut_tl = tasklet_new();
	if (!h3s->shut_tl) {
		pool_free(pool_head_h3s, h3s);
		goto out;
	}
	h3s->subs = NULL;
	h3s->shut_tl->process = h3_deferred_shut;
	h3s->shut_tl->context = h3s;
	LIST_INIT(&h3s->list);
	h3s->h3c       = h3c;
	h3s->cs        = NULL;
	h3s->sws       = 0;
	h3s->flags     = H3_SF_NONE;
	h3s->errcode   = H3_ERR_NO_ERROR;
	h3s->st        = H3_SS_IDLE;
	h3s->status    = 0;
	h3s->body_len  = 0;
	h3s->rxbuf     = BUF_NULL;

	if (h3c->flags & H3_CF_IS_BACK) {
		h1m_init_req(&h3s->h1m);
		h3s->h1m.err_pos = -1; // don't care about errors on the request path
		h3s->h1m.flags |= H1_MF_TOLOWER;
	} else {
		h1m_init_res(&h3s->h1m);
		h3s->h1m.err_pos = -1; // don't care about errors on the response path
		h3s->h1m.flags |= H1_MF_TOLOWER;
	}

	h3s->by_id.key = h3s->id = id;
	if (id > 0)
		h3c->max_id      = id;
	else
		h3c->nb_reserved++;

	eb32_insert(&h3c->streams_by_id, &h3s->by_id);
	h3c->nb_streams++;
	h3c->stream_cnt++;

	TRACE_LEAVE(H3_EV_H3S_NEW, h3c->conn, h3s);
	return h3s;
 out:
	TRACE_DEVEL("leaving in error", H3_EV_H3S_ERR|H3_EV_H3S_END, h3c->conn);
	return NULL;
}

/* creates a new stream <id> on the h3c connection and returns it, or NULL in
 * case of memory allocation error.
 */
static struct h3s *h3c_frt_stream_new(struct h3c *h3c, int id)
{
	struct session *sess = h3c->conn->owner;
	struct conn_stream *cs;
	struct h3s *h3s;

	TRACE_ENTER(H3_EV_H3S_NEW, h3c->conn);

	if (h3c->nb_streams >= h3_settings_max_concurrent_streams)
		goto out;

	h3s = h3s_new(h3c, id);
	if (!h3s)
		goto out;

	cs = cs_new(h3c->conn);
	if (!cs)
		goto out_close;

	cs->flags |= CS_FL_NOT_FIRST;
	h3s->cs = cs;
	cs->ctx = h3s;
	h3c->nb_cs++;

	if (stream_create_from_cs(cs) < 0)
		goto out_free_cs;

	/* We want the accept date presented to the next stream to be the one
	 * we have now, the handshake time to be null (since the next stream
	 * is not delayed by a handshake), and the idle time to count since
	 * right now.
	 */
	sess->accept_date = date;
	sess->tv_accept   = now;
	sess->t_handshake = 0;

	/* OK done, the stream lives its own life now */
	if (h3_frt_has_too_many_cs(h3c))
		h3c->flags |= H3_CF_DEM_TOOMANY;
	TRACE_LEAVE(H3_EV_H3S_NEW, h3c->conn);
	return h3s;

 out_free_cs:
	h3c->nb_cs--;
	cs_free(cs);
	h3s->cs = NULL;
 out_close:
	h3s_destroy(h3s);
 out:
	sess_log(sess);
	TRACE_LEAVE(H3_EV_H3S_NEW|H3_EV_H3S_ERR|H3_EV_H3S_END, h3c->conn);
	return NULL;
}

/* allocates a new stream associated to conn_stream <cs> on the h3c connection
 * and returns it, or NULL in case of memory allocation error or if the highest
 * possible stream ID was reached.
 */
static struct h3s *h3c_bck_stream_new(struct h3c *h3c, struct conn_stream *cs, struct session *sess)
{
	struct h3s *h3s = NULL;

	TRACE_ENTER(H3_EV_H3S_NEW, h3c->conn);

	if (h3c->nb_streams >= h3c->streams_limit)
		goto out;

	if (h3_streams_left(h3c) < 1)
		goto out;

	/* Defer choosing the ID until we send the first message to create the stream */
	h3s = h3s_new(h3c, 0);
	if (!h3s)
		goto out;

	h3s->cs = cs;
	h3s->sess = sess;
	cs->ctx = h3s;
	h3c->nb_cs++;

 out:
	if (likely(h3s))
		TRACE_LEAVE(H3_EV_H3S_NEW, h3c->conn, h3s);
	else
		TRACE_LEAVE(H3_EV_H3S_NEW|H3_EV_H3S_ERR|H3_EV_H3S_END, h3c->conn, h3s);
	return h3s;
}

/* try to send a settings frame on the connection. Returns > 0 on success, 0 if
 * it couldn't do anything. It may return an error in h3c. See RFC7540#11.3 for
 * the various settings codes.
 */
static int h3c_send_settings(struct h3c *h3c)
{
	struct buffer *res;
	char buf_data[100]; // enough for 15 settings
	struct buffer buf;
	int mfs;
	int ret = 0;

	TRACE_ENTER(H3_EV_TX_FRAME|H3_EV_TX_SETTINGS, h3c->conn);

	if (h3c_mux_busy(h3c, NULL)) {
		h3c->flags |= H3_CF_DEM_MBUSY;
		goto out;
	}

	chunk_init(&buf, buf_data, sizeof(buf_data));
	chunk_memcpy(&buf,
	       "\x00\x00\x00"      /* length    : 0 for now */
	       "\x04\x00"          /* type      : 4 (settings), flags : 0 */
	       "\x00\x00\x00\x00", /* stream ID : 0 */
	       9);

	if (h3c->flags & H3_CF_IS_BACK) {
		/* send settings_enable_push=0 */
		chunk_memcat(&buf, "\x00\x02\x00\x00\x00\x00", 6);
	}

	if (h3_settings_header_table_size != 4096) {
		char str[6] = "\x00\x01"; /* header_table_size */

		write_n32(str + 2, h3_settings_header_table_size);
		chunk_memcat(&buf, str, 6);
	}

	if (h3_settings_initial_window_size != 65535) {
		char str[6] = "\x00\x04"; /* initial_window_size */

		write_n32(str + 2, h3_settings_initial_window_size);
		chunk_memcat(&buf, str, 6);
	}

	if (h3_settings_max_concurrent_streams != 0) {
		char str[6] = "\x00\x03"; /* max_concurrent_streams */

		/* Note: 0 means "unlimited" for haproxy's config but not for
		 * the protocol, so never send this value!
		 */
		write_n32(str + 2, h3_settings_max_concurrent_streams);
		chunk_memcat(&buf, str, 6);
	}

	mfs = h3_settings_max_frame_size;
	if (mfs > global.tune.bufsize)
		mfs = global.tune.bufsize;

	if (!mfs)
		mfs = global.tune.bufsize;

	if (mfs != 16384) {
		char str[6] = "\x00\x05"; /* max_frame_size */

		/* note: similarly we could also emit MAX_HEADER_LIST_SIZE to
		 * match bufsize - rewrite size, but at the moment it seems
		 * that clients don't take care of it.
		 */
		write_n32(str + 2, mfs);
		chunk_memcat(&buf, str, 6);
	}

	h3_set_frame_size(buf.area, buf.data - 9);

	res = br_tail(h3c->mbuf);
 retry:
	if (!h3_get_buf(h3c, res)) {
		h3c->flags |= H3_CF_MUX_MALLOC;
		h3c->flags |= H3_CF_DEM_MROOM;
		goto out;
	}

	ret = b_istput(res, ist2(buf.area, buf.data));
	if (unlikely(ret <= 0)) {
		if (!ret) {
			if ((res = br_tail_add(h3c->mbuf)) != NULL)
				goto retry;
			h3c->flags |= H3_CF_MUX_MFULL;
			h3c->flags |= H3_CF_DEM_MROOM;
		}
		else {
			h3c_error(h3c, H3_ERR_INTERNAL_ERROR);
			ret = 0;
		}
	}
 out:
	TRACE_LEAVE(H3_EV_TX_FRAME|H3_EV_TX_SETTINGS, h3c->conn);
	return ret;
}

/* Try to receive a connection preface, then upon success try to send our
 * preface which is a SETTINGS frame. Returns > 0 on success or zero on
 * missing data. It may return an error in h3c.
 */
static int h3c_frt_recv_preface(struct h3c *h3c)
{
	int ret1;
	int ret2;

	TRACE_ENTER(H3_EV_RX_FRAME|H3_EV_RX_PREFACE, h3c->conn);

	ret1 = b_isteq(&h3c->dbuf, 0, b_data(&h3c->dbuf), ist(H3_CONN_PREFACE));

	if (unlikely(ret1 <= 0)) {
		if (ret1 < 0)
			sess_log(h3c->conn->owner);

		if (ret1 < 0 || conn_xprt_read0_pending(h3c->conn))
			h3c_error(h3c, H3_ERR_PROTOCOL_ERROR);
		ret2 = 0;
		goto out;
	}

	ret2 = h3c_send_settings(h3c);
	if (ret2 > 0)
		b_del(&h3c->dbuf, ret1);
 out:
	TRACE_LEAVE(H3_EV_RX_FRAME|H3_EV_RX_PREFACE, h3c->conn);
	return ret2;
}

/* Try to send a connection preface, then upon success try to send our
 * preface which is a SETTINGS frame. Returns > 0 on success or zero on
 * missing data. It may return an error in h3c.
 */
static int h3c_bck_send_preface(struct h3c *h3c)
{
	struct buffer *res;
	int ret = 0;

	TRACE_ENTER(H3_EV_TX_FRAME|H3_EV_TX_PREFACE, h3c->conn);

	if (h3c_mux_busy(h3c, NULL)) {
		h3c->flags |= H3_CF_DEM_MBUSY;
		goto out;
	}

	res = br_tail(h3c->mbuf);
 retry:
	if (!h3_get_buf(h3c, res)) {
		h3c->flags |= H3_CF_MUX_MALLOC;
		h3c->flags |= H3_CF_DEM_MROOM;
		goto out;
	}

	if (!b_data(res)) {
		/* preface not yet sent */
		ret = b_istput(res, ist(H3_CONN_PREFACE));
		if (unlikely(ret <= 0)) {
			if (!ret) {
				if ((res = br_tail_add(h3c->mbuf)) != NULL)
					goto retry;
				h3c->flags |= H3_CF_MUX_MFULL;
				h3c->flags |= H3_CF_DEM_MROOM;
				goto out;
			}
			else {
				h3c_error(h3c, H3_ERR_INTERNAL_ERROR);
				ret = 0;
				goto out;
			}
		}
	}
	ret = h3c_send_settings(h3c);
 out:
	TRACE_LEAVE(H3_EV_TX_FRAME|H3_EV_TX_PREFACE, h3c->conn);
	return ret;
}

/* try to send a GOAWAY frame on the connection to report an error or a graceful
 * shutdown, with h3c->errcode as the error code. Returns > 0 on success or zero
 * if nothing was done. It uses h3c->last_sid as the advertised ID, or copies it
 * from h3c->max_id if it's not set yet (<0). In case of lack of room to write
 * the message, it subscribes the requester (either <h3s> or <h3c>) to future
 * notifications. It sets H3_CF_GOAWAY_SENT on success, and H3_CF_GOAWAY_FAILED
 * on unrecoverable failure. It will not attempt to send one again in this last
 * case so that it is safe to use h3c_error() to report such errors.
 */
static int h3c_send_goaway_error(struct h3c *h3c, struct h3s *h3s)
{
	struct buffer *res;
	char str[17];
	int ret = 0;

	TRACE_ENTER(H3_EV_TX_FRAME|H3_EV_TX_GOAWAY, h3c->conn);

	if (h3c->flags & H3_CF_GOAWAY_FAILED) {
		ret = 1; // claim that it worked
		goto out;
	}

	if (h3c_mux_busy(h3c, h3s)) {
		if (h3s)
			h3s->flags |= H3_SF_BLK_MBUSY;
		else
			h3c->flags |= H3_CF_DEM_MBUSY;
		goto out;
	}

	/* len: 8, type: 7, flags: none, sid: 0 */
	memcpy(str, "\x00\x00\x08\x07\x00\x00\x00\x00\x00", 9);

	if (h3c->last_sid < 0)
		h3c->last_sid = h3c->max_id;

	write_n32(str + 9, h3c->last_sid);
	write_n32(str + 13, h3c->errcode);

	res = br_tail(h3c->mbuf);
 retry:
	if (!h3_get_buf(h3c, res)) {
		h3c->flags |= H3_CF_MUX_MALLOC;
		if (h3s)
			h3s->flags |= H3_SF_BLK_MROOM;
		else
			h3c->flags |= H3_CF_DEM_MROOM;
		goto out;
	}

	ret = b_istput(res, ist2(str, 17));
	if (unlikely(ret <= 0)) {
		if (!ret) {
			if ((res = br_tail_add(h3c->mbuf)) != NULL)
				goto retry;
			h3c->flags |= H3_CF_MUX_MFULL;
			if (h3s)
				h3s->flags |= H3_SF_BLK_MROOM;
			else
				h3c->flags |= H3_CF_DEM_MROOM;
			goto out;
		}
		else {
			/* we cannot report this error using GOAWAY, so we mark
			 * it and claim a success.
			 */
			h3c_error(h3c, H3_ERR_INTERNAL_ERROR);
			h3c->flags |= H3_CF_GOAWAY_FAILED;
			ret = 1;
			goto out;
		}
	}
	h3c->flags |= H3_CF_GOAWAY_SENT;
 out:
	TRACE_LEAVE(H3_EV_TX_FRAME|H3_EV_TX_GOAWAY, h3c->conn);
	return ret;
}

/* Try to send an RST_STREAM frame on the connection for the indicated stream
 * during mux operations. This stream must be valid and cannot be closed
 * already. h3s->id will be used for the stream ID and h3s->errcode will be
 * used for the error code. h3s->st will be update to H3_SS_CLOSED if it was
 * not yet.
 *
 * Returns > 0 on success or zero if nothing was done. In case of lack of room
 * to write the message, it subscribes the stream to future notifications.
 */
static int h3s_send_rst_stream(struct h3c *h3c, struct h3s *h3s)
{
	struct buffer *res;
	char str[13];
	int ret = 0;

	TRACE_ENTER(H3_EV_TX_FRAME|H3_EV_TX_RST, h3c->conn, h3s);

	if (!h3s || h3s->st == H3_SS_CLOSED) {
		ret = 1;
		goto out;
	}

	/* RFC7540#5.4.2: To avoid looping, an endpoint MUST NOT send a
	 * RST_STREAM in response to a RST_STREAM frame.
	 */
	if (h3c->dsi == h3s->id && h3c->dft == H3_FT_RST_STREAM) {
		ret = 1;
		goto ignore;
	}

	if (h3c_mux_busy(h3c, h3s)) {
		h3s->flags |= H3_SF_BLK_MBUSY;
		goto out;
	}

	/* len: 4, type: 3, flags: none */
	memcpy(str, "\x00\x00\x04\x03\x00", 5);
	write_n32(str + 5, h3s->id);
	write_n32(str + 9, h3s->errcode);

	res = br_tail(h3c->mbuf);
 retry:
	if (!h3_get_buf(h3c, res)) {
		h3c->flags |= H3_CF_MUX_MALLOC;
		h3s->flags |= H3_SF_BLK_MROOM;
		goto out;
	}

	ret = b_istput(res, ist2(str, 13));
	if (unlikely(ret <= 0)) {
		if (!ret) {
			if ((res = br_tail_add(h3c->mbuf)) != NULL)
				goto retry;
			h3c->flags |= H3_CF_MUX_MFULL;
			h3s->flags |= H3_SF_BLK_MROOM;
			goto out;
		}
		else {
			h3c_error(h3c, H3_ERR_INTERNAL_ERROR);
			ret = 0;
			goto out;
		}
	}

 ignore:
	h3s->flags |= H3_SF_RST_SENT;
	h3s_close(h3s);
 out:
	TRACE_LEAVE(H3_EV_TX_FRAME|H3_EV_TX_RST, h3c->conn, h3s);
	return ret;
}

/* Try to send an RST_STREAM frame on the connection for the stream being
 * demuxed using h3c->dsi for the stream ID. It will use h3s->errcode as the
 * error code, even if the stream is one of the dummy ones, and will update
 * h3s->st to H3_SS_CLOSED if it was not yet.
 *
 * Returns > 0 on success or zero if nothing was done. In case of lack of room
 * to write the message, it blocks the demuxer and subscribes it to future
 * notifications. It's worth mentioning that an RST may even be sent for a
 * closed stream.
 */
static int h3c_send_rst_stream(struct h3c *h3c, struct h3s *h3s)
{
	struct buffer *res;
	char str[13];
	int ret = 0;

	TRACE_ENTER(H3_EV_TX_FRAME|H3_EV_TX_RST, h3c->conn, h3s);

	/* RFC7540#5.4.2: To avoid looping, an endpoint MUST NOT send a
	 * RST_STREAM in response to a RST_STREAM frame.
	 */
	if (h3c->dft == H3_FT_RST_STREAM) {
		ret = 1;
		goto ignore;
	}

	if (h3c_mux_busy(h3c, h3s)) {
		h3c->flags |= H3_CF_DEM_MBUSY;
		goto out;
	}

	/* len: 4, type: 3, flags: none */
	memcpy(str, "\x00\x00\x04\x03\x00", 5);

	write_n32(str + 5, h3c->dsi);
	write_n32(str + 9, h3s->errcode);

	res = br_tail(h3c->mbuf);
 retry:
	if (!h3_get_buf(h3c, res)) {
		h3c->flags |= H3_CF_MUX_MALLOC;
		h3c->flags |= H3_CF_DEM_MROOM;
		goto out;
	}

	ret = b_istput(res, ist2(str, 13));
	if (unlikely(ret <= 0)) {
		if (!ret) {
			if ((res = br_tail_add(h3c->mbuf)) != NULL)
				goto retry;
			h3c->flags |= H3_CF_MUX_MFULL;
			h3c->flags |= H3_CF_DEM_MROOM;
			goto out;
		}
		else {
			h3c_error(h3c, H3_ERR_INTERNAL_ERROR);
			ret = 0;
			goto out;
		}
	}

 ignore:
	if (h3s->id) {
		h3s->flags |= H3_SF_RST_SENT;
		h3s_close(h3s);
	}

 out:
	TRACE_LEAVE(H3_EV_TX_FRAME|H3_EV_TX_RST, h3c->conn, h3s);
	return ret;
}

/* try to send an empty DATA frame with the ES flag set to notify about the
 * end of stream and match a shutdown(write). If an ES was already sent as
 * indicated by HLOC/ERROR/RESET/CLOSED states, nothing is done. Returns > 0
 * on success or zero if nothing was done. In case of lack of room to write the
 * message, it subscribes the requesting stream to future notifications.
 */
static int h3_send_empty_data_es(struct h3s *h3s)
{
	struct h3c *h3c = h3s->h3c;
	struct buffer *res;
	char str[9];
	int ret = 0;

	TRACE_ENTER(H3_EV_TX_FRAME|H3_EV_TX_DATA|H3_EV_TX_EOI, h3c->conn, h3s);

	if (h3s->st == H3_SS_HLOC || h3s->st == H3_SS_ERROR || h3s->st == H3_SS_CLOSED) {
		ret = 1;
		goto out;
	}

	if (h3c_mux_busy(h3c, h3s)) {
		h3s->flags |= H3_SF_BLK_MBUSY;
		goto out;
	}

	/* len: 0x000000, type: 0(DATA), flags: ES=1 */
	memcpy(str, "\x00\x00\x00\x00\x01", 5);
	write_n32(str + 5, h3s->id);

	res = br_tail(h3c->mbuf);
 retry:
	if (!h3_get_buf(h3c, res)) {
		h3c->flags |= H3_CF_MUX_MALLOC;
		h3s->flags |= H3_SF_BLK_MROOM;
		goto out;
	}

	ret = b_istput(res, ist2(str, 9));
	if (likely(ret > 0)) {
		h3s->flags |= H3_SF_ES_SENT;
	}
	else if (!ret) {
		if ((res = br_tail_add(h3c->mbuf)) != NULL)
			goto retry;
		h3c->flags |= H3_CF_MUX_MFULL;
		h3s->flags |= H3_SF_BLK_MROOM;
	}
	else {
		h3c_error(h3c, H3_ERR_INTERNAL_ERROR);
		ret = 0;
	}
 out:
	TRACE_LEAVE(H3_EV_TX_FRAME|H3_EV_TX_DATA|H3_EV_TX_EOI, h3c->conn, h3s);
	return ret;
}

/* wake a specific stream and assign its conn_stream som CS_FL_* flags among
 * CS_FL_ERR_PENDING and CS_FL_ERROR if needed. The stream's state
 * is automatically updated accordingly. If the stream is orphaned, it is
 * destroyed.
 */
static void h3s_wake_one_stream(struct h3s *h3s)
{
	struct h3c *h3c = h3s->h3c;

	TRACE_ENTER(H3_EV_H3S_WAKE, h3c->conn, h3s);

	if (!h3s->cs) {
		/* this stream was already orphaned */
		h3s_destroy(h3s);
		TRACE_DEVEL("leaving with no h3s", H3_EV_H3S_WAKE, h3c->conn);
		return;
	}

	if (conn_xprt_read0_pending(h3s->h3c->conn)) {
		if (h3s->st == H3_SS_OPEN)
			h3s->st = H3_SS_HREM;
		else if (h3s->st == H3_SS_HLOC)
			h3s_close(h3s);
	}

	if ((h3s->h3c->st0 >= H3_CS_ERROR || h3s->h3c->conn->flags & CO_FL_ERROR) ||
	    (h3s->h3c->last_sid > 0 && (!h3s->id || h3s->id > h3s->h3c->last_sid))) {
		h3s->cs->flags |= CS_FL_ERR_PENDING;
		if (h3s->cs->flags & CS_FL_EOS)
			h3s->cs->flags |= CS_FL_ERROR;

		if (h3s->st < H3_SS_ERROR)
			h3s->st = H3_SS_ERROR;
	}

	h3s_alert(h3s);
	TRACE_LEAVE(H3_EV_H3S_WAKE, h3c->conn);
}

/* wake the streams attached to the connection, whose id is greater than <last>
 * or unassigned.
 */
static void h3_wake_some_streams(struct h3c *h3c, int last)
{
	struct eb32_node *node;
	struct h3s *h3s;

	TRACE_ENTER(H3_EV_H3S_WAKE, h3c->conn);

	/* Wake all streams with ID > last */
	node = eb32_lookup_ge(&h3c->streams_by_id, last + 1);
	while (node) {
		h3s = container_of(node, struct h3s, by_id);
		node = eb32_next(node);
		h3s_wake_one_stream(h3s);
	}

	/* Wake all streams with unassigned ID (ID == 0) */
	node = eb32_lookup(&h3c->streams_by_id, 0);
	while (node) {
		h3s = container_of(node, struct h3s, by_id);
		if (h3s->id > 0)
			break;
		node = eb32_next(node);
		h3s_wake_one_stream(h3s);
	}

	TRACE_LEAVE(H3_EV_H3S_WAKE, h3c->conn);
}

/* Wake up all blocked streams whose window size has become positive after the
 * mux's initial window was adjusted. This should be done after having processed
 * SETTINGS frames which have updated the mux's initial window size.
 */
static void h3c_unblock_sfctl(struct h3c *h3c)
{
	struct h3s *h3s;
	struct eb32_node *node;

	TRACE_ENTER(H3_EV_H3C_WAKE, h3c->conn);

	node = eb32_first(&h3c->streams_by_id);
	while (node) {
		h3s = container_of(node, struct h3s, by_id);
		if (h3s->flags & H3_SF_BLK_SFCTL && h3s_mws(h3s) > 0) {
			h3s->flags &= ~H3_SF_BLK_SFCTL;
			LIST_DEL_INIT(&h3s->list);
			if ((h3s->subs && h3s->subs->events & SUB_RETRY_SEND) ||
			    h3s->flags & (H3_SF_WANT_SHUTR|H3_SF_WANT_SHUTW))
				LIST_ADDQ(&h3c->send_list, &h3s->list);
		}
		node = eb32_next(node);
	}

	TRACE_LEAVE(H3_EV_H3C_WAKE, h3c->conn);
}

/* processes a SETTINGS frame whose payload is <payload> for <plen> bytes, and
 * ACKs it if needed. Returns > 0 on success or zero on missing data. It may
 * return an error in h3c. The caller must have already verified frame length
 * and stream ID validity. Described in RFC7540#6.5.
 */
static int h3c_handle_settings(struct h3c *h3c)
{
	unsigned int offset;
	int error;

	TRACE_ENTER(H3_EV_RX_FRAME|H3_EV_RX_SETTINGS, h3c->conn);

	if (h3c->dff & H3_F_SETTINGS_ACK) {
		if (h3c->dfl) {
			error = H3_ERR_FRAME_SIZE_ERROR;
			goto fail;
		}
		goto done;
	}

	/* process full frame only */
	if (b_data(&h3c->dbuf) < h3c->dfl)
		goto out0;

	/* parse the frame */
	for (offset = 0; offset < h3c->dfl; offset += 6) {
		uint16_t type = h3_get_n16(&h3c->dbuf, offset);
		int32_t  arg  = h3_get_n32(&h3c->dbuf, offset + 2);

		switch (type) {
		case H3_SETTINGS_INITIAL_WINDOW_SIZE:
			/* we need to update all existing streams with the
			 * difference from the previous iws.
			 */
			if (arg < 0) { // RFC7540#6.5.2
				error = H3_ERR_FLOW_CONTROL_ERROR;
				goto fail;
			}
			h3c->miw = arg;
			break;
		case H3_SETTINGS_MAX_FRAME_SIZE:
			if (arg < 16384 || arg > 16777215) { // RFC7540#6.5.2
				error = H3_ERR_PROTOCOL_ERROR;
				goto fail;
			}
			h3c->mfs = arg;
			break;
		case H3_SETTINGS_ENABLE_PUSH:
			if (arg < 0 || arg > 1) { // RFC7540#6.5.2
				error = H3_ERR_PROTOCOL_ERROR;
				goto fail;
			}
			break;
		case H3_SETTINGS_MAX_CONCURRENT_STREAMS:
			if (h3c->flags & H3_CF_IS_BACK) {
				/* the limit is only for the backend; for the frontend it is our limit */
				if ((unsigned int)arg > h3_settings_max_concurrent_streams)
					arg = h3_settings_max_concurrent_streams;
				h3c->streams_limit = arg;
			}
			break;
		}
	}

	/* need to ACK this frame now */
	h3c->st0 = H3_CS_FRAME_A;
 done:
	TRACE_LEAVE(H3_EV_RX_FRAME|H3_EV_RX_SETTINGS, h3c->conn);
	return 1;
 fail:
	if (!(h3c->flags & H3_CF_IS_BACK))
		sess_log(h3c->conn->owner);
	h3c_error(h3c, error);
 out0:
	TRACE_DEVEL("leaving with missing data or error", H3_EV_RX_FRAME|H3_EV_RX_SETTINGS, h3c->conn);
	return 0;
}

/* try to send an ACK for a settings frame on the connection. Returns > 0 on
 * success or one of the h3_status values.
 */
static int h3c_ack_settings(struct h3c *h3c)
{
	struct buffer *res;
	char str[9];
	int ret = 0;

	TRACE_ENTER(H3_EV_TX_FRAME|H3_EV_TX_SETTINGS, h3c->conn);

	if (h3c_mux_busy(h3c, NULL)) {
		h3c->flags |= H3_CF_DEM_MBUSY;
		goto out;
	}

	memcpy(str,
	       "\x00\x00\x00"     /* length : 0 (no data)  */
	       "\x04" "\x01"      /* type   : 4, flags : ACK */
	       "\x00\x00\x00\x00" /* stream ID */, 9);

	res = br_tail(h3c->mbuf);
 retry:
	if (!h3_get_buf(h3c, res)) {
		h3c->flags |= H3_CF_MUX_MALLOC;
		h3c->flags |= H3_CF_DEM_MROOM;
		goto out;
	}

	ret = b_istput(res, ist2(str, 9));
	if (unlikely(ret <= 0)) {
		if (!ret) {
			if ((res = br_tail_add(h3c->mbuf)) != NULL)
				goto retry;
			h3c->flags |= H3_CF_MUX_MFULL;
			h3c->flags |= H3_CF_DEM_MROOM;
		}
		else {
			h3c_error(h3c, H3_ERR_INTERNAL_ERROR);
			ret = 0;
		}
	}
 out:
	TRACE_LEAVE(H3_EV_TX_FRAME|H3_EV_TX_SETTINGS, h3c->conn);
	return ret;
}

/* processes a PING frame and schedules an ACK if needed. The caller must pass
 * the pointer to the payload in <payload>. Returns > 0 on success or zero on
 * missing data. The caller must have already verified frame length
 * and stream ID validity.
 */
static int h3c_handle_ping(struct h3c *h3c)
{
	/* schedule a response */
	if (!(h3c->dff & H3_F_PING_ACK))
		h3c->st0 = H3_CS_FRAME_A;
	return 1;
}

/* Try to send a window update for stream id <sid> and value <increment>.
 * Returns > 0 on success or zero on missing room or failure. It may return an
 * error in h3c.
 */
static int h3c_send_window_update(struct h3c *h3c, int sid, uint32_t increment)
{
	struct buffer *res;
	char str[13];
	int ret = 0;

	TRACE_ENTER(H3_EV_TX_FRAME|H3_EV_TX_WU, h3c->conn);

	if (h3c_mux_busy(h3c, NULL)) {
		h3c->flags |= H3_CF_DEM_MBUSY;
		goto out;
	}

	/* length: 4, type: 8, flags: none */
	memcpy(str, "\x00\x00\x04\x08\x00", 5);
	write_n32(str + 5, sid);
	write_n32(str + 9, increment);

	res = br_tail(h3c->mbuf);
 retry:
	if (!h3_get_buf(h3c, res)) {
		h3c->flags |= H3_CF_MUX_MALLOC;
		h3c->flags |= H3_CF_DEM_MROOM;
		goto out;
	}

	ret = b_istput(res, ist2(str, 13));
	if (unlikely(ret <= 0)) {
		if (!ret) {
			if ((res = br_tail_add(h3c->mbuf)) != NULL)
				goto retry;
			h3c->flags |= H3_CF_MUX_MFULL;
			h3c->flags |= H3_CF_DEM_MROOM;
		}
		else {
			h3c_error(h3c, H3_ERR_INTERNAL_ERROR);
			ret = 0;
		}
	}
 out:
	TRACE_LEAVE(H3_EV_TX_FRAME|H3_EV_TX_WU, h3c->conn);
	return ret;
}

/* try to send pending window update for the connection. It's safe to call it
 * with no pending updates. Returns > 0 on success or zero on missing room or
 * failure. It may return an error in h3c.
 */
static int h3c_send_conn_wu(struct h3c *h3c)
{
	int ret = 1;

	TRACE_ENTER(H3_EV_TX_FRAME|H3_EV_TX_WU, h3c->conn);

	if (h3c->rcvd_c <= 0)
		goto out;

	if (!(h3c->flags & H3_CF_WINDOW_OPENED)) {
		/* increase the advertised connection window to 2G on
		 * first update.
		 */
		h3c->flags |= H3_CF_WINDOW_OPENED;
		h3c->rcvd_c += H3_INITIAL_WINDOW_INCREMENT;
	}

	/* send WU for the connection */
	ret = h3c_send_window_update(h3c, 0, h3c->rcvd_c);
	if (ret > 0)
		h3c->rcvd_c = 0;

 out:
	TRACE_LEAVE(H3_EV_TX_FRAME|H3_EV_TX_WU, h3c->conn);
	return ret;
}

/* try to send pending window update for the current dmux stream. It's safe to
 * call it with no pending updates. Returns > 0 on success or zero on missing
 * room or failure. It may return an error in h3c.
 */
static int h3c_send_strm_wu(struct h3c *h3c)
{
	int ret = 1;

	TRACE_ENTER(H3_EV_TX_FRAME|H3_EV_TX_WU, h3c->conn);

	if (h3c->rcvd_s <= 0)
		goto out;

	/* send WU for the stream */
	ret = h3c_send_window_update(h3c, h3c->dsi, h3c->rcvd_s);
	if (ret > 0)
		h3c->rcvd_s = 0;
 out:
	TRACE_LEAVE(H3_EV_TX_FRAME|H3_EV_TX_WU, h3c->conn);
	return ret;
}

/* try to send an ACK for a ping frame on the connection. Returns > 0 on
 * success, 0 on missing data or one of the h3_status values.
 */
static int h3c_ack_ping(struct h3c *h3c)
{
	struct buffer *res;
	char str[17];
	int ret = 0;

	TRACE_ENTER(H3_EV_TX_FRAME|H3_EV_TX_PING, h3c->conn);

	if (b_data(&h3c->dbuf) < 8)
		goto out;

	if (h3c_mux_busy(h3c, NULL)) {
		h3c->flags |= H3_CF_DEM_MBUSY;
		goto out;
	}

	memcpy(str,
	       "\x00\x00\x08"     /* length : 8 (same payload) */
	       "\x06" "\x01"      /* type   : 6, flags : ACK   */
	       "\x00\x00\x00\x00" /* stream ID */, 9);

	/* copy the original payload */
	h3_get_buf_bytes(str + 9, 8, &h3c->dbuf, 0);

	res = br_tail(h3c->mbuf);
 retry:
	if (!h3_get_buf(h3c, res)) {
		h3c->flags |= H3_CF_MUX_MALLOC;
		h3c->flags |= H3_CF_DEM_MROOM;
		goto out;
	}

	ret = b_istput(res, ist2(str, 17));
	if (unlikely(ret <= 0)) {
		if (!ret) {
			if ((res = br_tail_add(h3c->mbuf)) != NULL)
				goto retry;
			h3c->flags |= H3_CF_MUX_MFULL;
			h3c->flags |= H3_CF_DEM_MROOM;
		}
		else {
			h3c_error(h3c, H3_ERR_INTERNAL_ERROR);
			ret = 0;
		}
	}
 out:
	TRACE_LEAVE(H3_EV_TX_FRAME|H3_EV_TX_PING, h3c->conn);
	return ret;
}

/* processes a WINDOW_UPDATE frame whose payload is <payload> for <plen> bytes.
 * Returns > 0 on success or zero on missing data. It may return an error in
 * h3c or h3s. The caller must have already verified frame length and stream ID
 * validity. Described in RFC7540#6.9.
 */
static int h3c_handle_window_update(struct h3c *h3c, struct h3s *h3s)
{
	int32_t inc;
	int error;

	TRACE_ENTER(H3_EV_RX_FRAME|H3_EV_RX_WU, h3c->conn);

	/* process full frame only */
	if (b_data(&h3c->dbuf) < h3c->dfl)
		goto out0;

	inc = h3_get_n32(&h3c->dbuf, 0);

	if (h3c->dsi != 0) {
		/* stream window update */

		/* it's not an error to receive WU on a closed stream */
		if (h3s->st == H3_SS_CLOSED)
			goto done;

		if (!inc) {
			error = H3_ERR_PROTOCOL_ERROR;
			goto strm_err;
		}

		if (h3s_mws(h3s) >= 0 && h3s_mws(h3s) + inc < 0) {
			error = H3_ERR_FLOW_CONTROL_ERROR;
			goto strm_err;
		}

		h3s->sws += inc;
		if (h3s_mws(h3s) > 0 && (h3s->flags & H3_SF_BLK_SFCTL)) {
			h3s->flags &= ~H3_SF_BLK_SFCTL;
			LIST_DEL_INIT(&h3s->list);
			if ((h3s->subs && h3s->subs->events & SUB_RETRY_SEND) ||
			    h3s->flags & (H3_SF_WANT_SHUTR|H3_SF_WANT_SHUTW))
				LIST_ADDQ(&h3c->send_list, &h3s->list);
		}
	}
	else {
		/* connection window update */
		if (!inc) {
			error = H3_ERR_PROTOCOL_ERROR;
			goto conn_err;
		}

		if (h3c->mws >= 0 && h3c->mws + inc < 0) {
			error = H3_ERR_FLOW_CONTROL_ERROR;
			goto conn_err;
		}

		h3c->mws += inc;
	}

 done:
	TRACE_LEAVE(H3_EV_RX_FRAME|H3_EV_RX_WU, h3c->conn);
	return 1;

 conn_err:
	h3c_error(h3c, error);
 out0:
	TRACE_DEVEL("leaving on missing data or error", H3_EV_RX_FRAME|H3_EV_RX_WU, h3c->conn);
	return 0;

 strm_err:
	h3s_error(h3s, error);
	h3c->st0 = H3_CS_FRAME_E;
	TRACE_DEVEL("leaving on stream error", H3_EV_RX_FRAME|H3_EV_RX_WU, h3c->conn);
	return 0;
}

/* processes a GOAWAY frame, and signals all streams whose ID is greater than
 * the last ID. Returns > 0 on success or zero on missing data. The caller must
 * have already verified frame length and stream ID validity. Described in
 * RFC7540#6.8.
 */
static int h3c_handle_goaway(struct h3c *h3c)
{
	int last;

	TRACE_ENTER(H3_EV_RX_FRAME|H3_EV_RX_GOAWAY, h3c->conn);
	/* process full frame only */
	if (b_data(&h3c->dbuf) < h3c->dfl) {
		TRACE_DEVEL("leaving on missing data", H3_EV_RX_FRAME|H3_EV_RX_GOAWAY, h3c->conn);
		return 0;
	}

	last = h3_get_n32(&h3c->dbuf, 0);
	h3c->errcode = h3_get_n32(&h3c->dbuf, 4);
	if (h3c->last_sid < 0)
		h3c->last_sid = last;
	h3_wake_some_streams(h3c, last);
	TRACE_LEAVE(H3_EV_RX_FRAME|H3_EV_RX_GOAWAY, h3c->conn);
	return 1;
}

/* processes a PRIORITY frame, and either skips it or rejects if it is
 * invalid. Returns > 0 on success or zero on missing data. It may return an
 * error in h3c. The caller must have already verified frame length and stream
 * ID validity. Described in RFC7540#6.3.
 */
static int h3c_handle_priority(struct h3c *h3c)
{
	TRACE_ENTER(H3_EV_RX_FRAME|H3_EV_RX_PRIO, h3c->conn);

	/* process full frame only */
	if (b_data(&h3c->dbuf) < h3c->dfl) {
		TRACE_DEVEL("leaving on missing data", H3_EV_RX_FRAME|H3_EV_RX_PRIO, h3c->conn);
		return 0;
	}

	if (h3_get_n32(&h3c->dbuf, 0) == h3c->dsi) {
		/* 7540#5.3 : can't depend on itself */
		h3c_error(h3c, H3_ERR_PROTOCOL_ERROR);
		TRACE_DEVEL("leaving on error", H3_EV_RX_FRAME|H3_EV_RX_PRIO, h3c->conn);
		return 0;
	}
	TRACE_LEAVE(H3_EV_RX_FRAME|H3_EV_RX_PRIO, h3c->conn);
	return 1;
}

/* processes an RST_STREAM frame, and sets the 32-bit error code on the stream.
 * Returns > 0 on success or zero on missing data. The caller must have already
 * verified frame length and stream ID validity. Described in RFC7540#6.4.
 */
static int h3c_handle_rst_stream(struct h3c *h3c, struct h3s *h3s)
{
	TRACE_ENTER(H3_EV_RX_FRAME|H3_EV_RX_RST|H3_EV_RX_EOI, h3c->conn, h3s);

	/* process full frame only */
	if (b_data(&h3c->dbuf) < h3c->dfl) {
		TRACE_DEVEL("leaving on missing data", H3_EV_RX_FRAME|H3_EV_RX_RST|H3_EV_RX_EOI, h3c->conn, h3s);
		return 0;
	}

	/* late RST, already handled */
	if (h3s->st == H3_SS_CLOSED) {
		TRACE_DEVEL("leaving on stream closed", H3_EV_RX_FRAME|H3_EV_RX_RST|H3_EV_RX_EOI, h3c->conn, h3s);
		return 1;
	}

	h3s->errcode = h3_get_n32(&h3c->dbuf, 0);
	h3s_close(h3s);

	if (h3s->cs) {
		cs_set_error(h3s->cs);
		h3s_alert(h3s);
	}

	h3s->flags |= H3_SF_RST_RCVD;
	TRACE_LEAVE(H3_EV_RX_FRAME|H3_EV_RX_RST|H3_EV_RX_EOI, h3c->conn, h3s);
	return 1;
}

/* processes a HEADERS frame. Returns h3s on success or NULL on missing data.
 * It may return an error in h3c or h3s. The caller must consider that the
 * return value is the new h3s in case one was allocated (most common case).
 * Described in RFC7540#6.2. Most of the
 * errors here are reported as connection errors since it's impossible to
 * recover from such errors after the compression context has been altered.
 */
static struct h3s *h3c_frt_handle_headers(struct h3c *h3c, struct h3s *h3s)
{
	struct buffer rxbuf = BUF_NULL;
	unsigned long long body_len = 0;
	uint32_t flags = 0;
	int error;

	TRACE_ENTER(H3_EV_RX_FRAME|H3_EV_RX_HDR, h3c->conn, h3s);

	if (!b_size(&h3c->dbuf))
		goto out; // empty buffer

	if (b_data(&h3c->dbuf) < h3c->dfl && !b_full(&h3c->dbuf))
		goto out; // incomplete frame

	/* now either the frame is complete or the buffer is complete */
	if (h3s->st != H3_SS_IDLE) {
		/* The stream exists/existed, this must be a trailers frame */
		if (h3s->st != H3_SS_CLOSED) {
			error = h3c_decode_headers(h3c, &h3s->rxbuf, &h3s->flags, &body_len);
			/* unrecoverable error ? */
			if (h3c->st0 >= H3_CS_ERROR)
				goto out;

			if (error == 0)
				goto out; // missing data

			if (error < 0) {
				/* Failed to decode this frame (e.g. too large request)
				 * but the HPACK decompressor is still synchronized.
				 */
				h3s_error(h3s, H3_ERR_INTERNAL_ERROR);
				h3c->st0 = H3_CS_FRAME_E;
				goto out;
			}
			goto done;
		}
		/* the connection was already killed by an RST, let's consume
		 * the data and send another RST.
		 */
		error = h3c_decode_headers(h3c, &rxbuf, &flags, &body_len);
		h3s = (struct h3s*)h3_error_stream;
		goto send_rst;
	}
	else if (h3c->dsi <= h3c->max_id || !(h3c->dsi & 1)) {
		/* RFC7540#5.1.1 stream id > prev ones, and must be odd here */
		error = H3_ERR_PROTOCOL_ERROR;
		sess_log(h3c->conn->owner);
		goto conn_err;
	}
	else if (h3c->flags & H3_CF_DEM_TOOMANY)
		goto out; // IDLE but too many cs still present

	error = h3c_decode_headers(h3c, &rxbuf, &flags, &body_len);

	/* unrecoverable error ? */
	if (h3c->st0 >= H3_CS_ERROR)
		goto out;

	if (error <= 0) {
		if (error == 0)
			goto out; // missing data

		/* Failed to decode this stream (e.g. too large request)
		 * but the HPACK decompressor is still synchronized.
		 */
		h3s = (struct h3s*)h3_error_stream;
		goto send_rst;
	}

	/* Note: we don't emit any other logs below because ff we return
	 * positively from h3c_frt_stream_new(), the stream will report the error,
	 * and if we return in error, h3c_frt_stream_new() will emit the error.
	 */
	h3s = h3c_frt_stream_new(h3c, h3c->dsi);
	if (!h3s) {
		h3s = (struct h3s*)h3_refused_stream;
		goto send_rst;
	}

	h3s->st = H3_SS_OPEN;
	h3s->rxbuf = rxbuf;
	h3s->flags |= flags;
	h3s->body_len = body_len;

 done:
	if (h3c->dff & H3_F_HEADERS_END_STREAM)
		h3s->flags |= H3_SF_ES_RCVD;

	if (h3s->flags & H3_SF_ES_RCVD) {
		if (h3s->st == H3_SS_OPEN)
			h3s->st = H3_SS_HREM;
		else
			h3s_close(h3s);
	}

	/* update the max stream ID if the request is being processed */
	if (h3s->id > h3c->max_id)
		h3c->max_id = h3s->id;

	TRACE_USER("rcvd H3 request ", H3_EV_RX_FRAME|H3_EV_RX_HDR|H3_EV_STRM_NEW, h3c->conn,, &rxbuf);
	return h3s;

 conn_err:
	h3c_error(h3c, error);
	goto out;

 out:
	h3_release_buf(h3c, &rxbuf);
	TRACE_DEVEL("leaving on missing data or error", H3_EV_RX_FRAME|H3_EV_RX_HDR, h3c->conn, h3s);
	return NULL;

 send_rst:
	/* make the demux send an RST for the current stream. We may only
	 * do this if we're certain that the HEADERS frame was properly
	 * decompressed so that the HPACK decoder is still kept up to date.
	 */
	h3_release_buf(h3c, &rxbuf);
	h3c->st0 = H3_CS_FRAME_E;

	TRACE_USER("rejected H3 request", H3_EV_RX_FRAME|H3_EV_RX_HDR|H3_EV_STRM_NEW|H3_EV_STRM_END, h3c->conn,, &rxbuf);
	TRACE_DEVEL("leaving on error", H3_EV_RX_FRAME|H3_EV_RX_HDR, h3c->conn, h3s);
	return h3s;
}

/* processes a HEADERS frame. Returns h3s on success or NULL on missing data.
 * It may return an error in h3c or h3s. Described in RFC7540#6.2. Most of the
 * errors here are reported as connection errors since it's impossible to
 * recover from such errors after the compression context has been altered.
 */
static struct h3s *h3c_bck_handle_headers(struct h3c *h3c, struct h3s *h3s)
{
	struct buffer rxbuf = BUF_NULL;
	unsigned long long body_len = 0;
	uint32_t flags = 0;
	int error;

	TRACE_ENTER(H3_EV_RX_FRAME|H3_EV_RX_HDR, h3c->conn, h3s);

	if (!b_size(&h3c->dbuf))
		goto fail; // empty buffer

	if (b_data(&h3c->dbuf) < h3c->dfl && !b_full(&h3c->dbuf))
		goto fail; // incomplete frame

	if (h3s->st != H3_SS_CLOSED) {
		error = h3c_decode_headers(h3c, &h3s->rxbuf, &h3s->flags, &h3s->body_len);
	}
	else {
		/* the connection was already killed by an RST, let's consume
		 * the data and send another RST.
		 */
		error = h3c_decode_headers(h3c, &rxbuf, &flags, &body_len);
		h3s = (struct h3s*)h3_error_stream;
		h3c->st0 = H3_CS_FRAME_E;
		goto send_rst;
	}

	/* unrecoverable error ? */
	if (h3c->st0 >= H3_CS_ERROR)
		goto fail;

	if (h3s->st != H3_SS_OPEN && h3s->st != H3_SS_HLOC) {
		/* RFC7540#5.1 */
		h3s_error(h3s, H3_ERR_STREAM_CLOSED);
		h3c->st0 = H3_CS_FRAME_E;
		goto fail;
	}

	if (error <= 0) {
		if (error == 0)
			goto fail; // missing data

		/* stream error : send RST_STREAM */
		h3s_error(h3s, H3_ERR_PROTOCOL_ERROR);
		h3c->st0 = H3_CS_FRAME_E;
		goto fail;
	}

	if (h3c->dff & H3_F_HEADERS_END_STREAM)
		h3s->flags |= H3_SF_ES_RCVD;

	if (h3s->cs && h3s->cs->flags & CS_FL_ERROR && h3s->st < H3_SS_ERROR)
		h3s->st = H3_SS_ERROR;
	else if (h3s->flags & H3_SF_ES_RCVD) {
		if (h3s->st == H3_SS_OPEN)
			h3s->st = H3_SS_HREM;
		else if (h3s->st == H3_SS_HLOC)
			h3s_close(h3s);
	}

	TRACE_USER("rcvd H3 response", H3_EV_RX_FRAME|H3_EV_RX_HDR, h3c->conn,, &h3s->rxbuf);
	TRACE_LEAVE(H3_EV_RX_FRAME|H3_EV_RX_HDR, h3c->conn, h3s);
	return h3s;
 fail:
	TRACE_DEVEL("leaving on missing data or error", H3_EV_RX_FRAME|H3_EV_RX_HDR, h3c->conn, h3s);
	return NULL;

 send_rst:
	/* make the demux send an RST for the current stream. We may only
	 * do this if we're certain that the HEADERS frame was properly
	 * decompressed so that the HPACK decoder is still kept up to date.
	 */
	h3_release_buf(h3c, &rxbuf);
	h3c->st0 = H3_CS_FRAME_E;

	TRACE_USER("rejected H3 response", H3_EV_RX_FRAME|H3_EV_RX_HDR|H3_EV_STRM_NEW|H3_EV_STRM_END, h3c->conn,, &rxbuf);
	TRACE_DEVEL("leaving on error", H3_EV_RX_FRAME|H3_EV_RX_HDR, h3c->conn, h3s);
	return h3s;
}

/* processes a DATA frame. Returns > 0 on success or zero on missing data.
 * It may return an error in h3c or h3s. Described in RFC7540#6.1.
 */
static int h3c_frt_handle_data(struct h3c *h3c, struct h3s *h3s)
{
	int error;

	TRACE_ENTER(H3_EV_RX_FRAME|H3_EV_RX_DATA, h3c->conn, h3s);

	/* note that empty DATA frames are perfectly valid and sometimes used
	 * to signal an end of stream (with the ES flag).
	 */

	if (!b_size(&h3c->dbuf) && h3c->dfl)
		goto fail; // empty buffer

	if (b_data(&h3c->dbuf) < h3c->dfl && !b_full(&h3c->dbuf))
		goto fail; // incomplete frame

	/* now either the frame is complete or the buffer is complete */

	if (h3s->st != H3_SS_OPEN && h3s->st != H3_SS_HLOC) {
		/* RFC7540#6.1 */
		error = H3_ERR_STREAM_CLOSED;
		goto strm_err;
	}

	if ((h3s->flags & H3_SF_DATA_CLEN) && (h3c->dfl - h3c->dpl) > h3s->body_len) {
		/* RFC7540#8.1.2 */
		error = H3_ERR_PROTOCOL_ERROR;
		goto strm_err;
	}

	if (!h3_frt_transfer_data(h3s))
		goto fail;

	/* call the upper layers to process the frame, then let the upper layer
	 * notify the stream about any change.
	 */
	if (!h3s->cs) {
		/* The upper layer has already closed, this may happen on
		 * 4xx/redirects during POST, or when receiving a response
		 * from an H3 server after the client has aborted.
		 */
		error = H3_ERR_CANCEL;
		goto strm_err;
	}

	if (h3c->st0 >= H3_CS_ERROR)
		goto fail;

	if (h3s->st >= H3_SS_ERROR) {
		/* stream error : send RST_STREAM */
		h3c->st0 = H3_CS_FRAME_E;
	}

	/* check for completion : the callee will change this to FRAME_A or
	 * FRAME_H once done.
	 */
	if (h3c->st0 == H3_CS_FRAME_P)
		goto fail;

	/* last frame */
	if (h3c->dff & H3_F_DATA_END_STREAM) {
		h3s->flags |= H3_SF_ES_RCVD;
		if (h3s->st == H3_SS_OPEN)
			h3s->st = H3_SS_HREM;
		else
			h3s_close(h3s);

		if (h3s->flags & H3_SF_DATA_CLEN && h3s->body_len) {
			/* RFC7540#8.1.2 */
			error = H3_ERR_PROTOCOL_ERROR;
			goto strm_err;
		}
	}

	TRACE_LEAVE(H3_EV_RX_FRAME|H3_EV_RX_DATA, h3c->conn, h3s);
	return 1;

 strm_err:
	h3s_error(h3s, error);
	h3c->st0 = H3_CS_FRAME_E;
 fail:
	TRACE_DEVEL("leaving on missing data or error", H3_EV_RX_FRAME|H3_EV_RX_DATA, h3c->conn, h3s);
	return 0;
}

/* check that the current frame described in h3c->{dsi,dft,dfl,dff,...} is
 * valid for the current stream state. This is needed only after parsing the
 * frame header but in practice it can be performed at any time during
 * H3_CS_FRAME_P since no state transition happens there. Returns >0 on success
 * or 0 in case of error, in which case either h3s or h3c will carry an error.
 */
static int h3_frame_check_vs_state(struct h3c *h3c, struct h3s *h3s)
{
	TRACE_ENTER(H3_EV_RX_FRAME|H3_EV_RX_FHDR, h3c->conn, h3s);

	if (h3s->st == H3_SS_IDLE &&
	    h3c->dft != H3_FT_HEADERS && h3c->dft != H3_FT_PRIORITY) {
		/* RFC7540#5.1: any frame other than HEADERS or PRIORITY in
		 * this state MUST be treated as a connection error
		 */
		h3c_error(h3c, H3_ERR_PROTOCOL_ERROR);
		if (!h3c->nb_streams && !(h3c->flags & H3_CF_IS_BACK)) {
			/* only log if no other stream can report the error */
			sess_log(h3c->conn->owner);
		}
		TRACE_DEVEL("leaving in error (idle&!hdrs&!prio)", H3_EV_RX_FRAME|H3_EV_RX_FHDR|H3_EV_PROTO_ERR, h3c->conn, h3s);
		return 0;
	}

	if (h3s->st == H3_SS_IDLE && (h3c->flags & H3_CF_IS_BACK)) {
		/* only PUSH_PROMISE would be permitted here */
		h3c_error(h3c, H3_ERR_PROTOCOL_ERROR);
		TRACE_DEVEL("leaving in error (idle&back)", H3_EV_RX_FRAME|H3_EV_RX_FHDR|H3_EV_PROTO_ERR, h3c->conn, h3s);
		return 0;
	}

	if (h3s->st == H3_SS_HREM && h3c->dft != H3_FT_WINDOW_UPDATE &&
	    h3c->dft != H3_FT_RST_STREAM && h3c->dft != H3_FT_PRIORITY) {
		/* RFC7540#5.1: any frame other than WU/PRIO/RST in
		 * this state MUST be treated as a stream error.
		 * 6.2, 6.6 and 6.10 further mandate that HEADERS/
		 * PUSH_PROMISE/CONTINUATION cause connection errors.
		 */
		if (h3_ft_bit(h3c->dft) & H3_FT_HDR_MASK)
			h3c_error(h3c, H3_ERR_PROTOCOL_ERROR);
		else
			h3s_error(h3s, H3_ERR_STREAM_CLOSED);
		TRACE_DEVEL("leaving in error (hrem&!wu&!rst&!prio)", H3_EV_RX_FRAME|H3_EV_RX_FHDR|H3_EV_PROTO_ERR, h3c->conn, h3s);
		return 0;
	}

	/* Below the management of frames received in closed state is a
	 * bit hackish because the spec makes strong differences between
	 * streams closed by receiving RST, sending RST, and seeing ES
	 * in both directions. In addition to this, the creation of a
	 * new stream reusing the identifier of a closed one will be
	 * detected here. Given that we cannot keep track of all closed
	 * streams forever, we consider that unknown closed streams were
	 * closed on RST received, which allows us to respond with an
	 * RST without breaking the connection (eg: to abort a transfer).
	 * Some frames have to be silently ignored as well.
	 */
	if (h3s->st == H3_SS_CLOSED && h3c->dsi) {
		if (!(h3c->flags & H3_CF_IS_BACK) && h3_ft_bit(h3c->dft) & H3_FT_HDR_MASK) {
			/* #5.1.1: The identifier of a newly
			 * established stream MUST be numerically
			 * greater than all streams that the initiating
			 * endpoint has opened or reserved. This
			 * governs streams that are opened using a
			 * HEADERS frame and streams that are reserved
			 * using PUSH_PROMISE. An endpoint that
			 * receives an unexpected stream identifier
			 * MUST respond with a connection error.
			 */
			h3c_error(h3c, H3_ERR_STREAM_CLOSED);
			TRACE_DEVEL("leaving in error (closed&hdrmask)", H3_EV_RX_FRAME|H3_EV_RX_FHDR|H3_EV_PROTO_ERR, h3c->conn, h3s);
			return 0;
		}

		if (h3s->flags & H3_SF_RST_RCVD &&
		    !(h3_ft_bit(h3c->dft) & (H3_FT_HDR_MASK | H3_FT_RST_STREAM_BIT | H3_FT_PRIORITY_BIT | H3_FT_WINDOW_UPDATE_BIT))) {
			/* RFC7540#5.1:closed: an endpoint that
			 * receives any frame other than PRIORITY after
			 * receiving a RST_STREAM MUST treat that as a
			 * stream error of type STREAM_CLOSED.
			 *
			 * Note that old streams fall into this category
			 * and will lead to an RST being sent.
			 *
			 * However, we cannot generalize this to all frame types. Those
			 * carrying compression state must still be processed before
			 * being dropped or we'll desynchronize the decoder. This can
			 * happen with request trailers received after sending an
			 * RST_STREAM, or with header/trailers responses received after
			 * sending RST_STREAM (aborted stream).
			 *
			 * In addition, since our CLOSED streams always carry the
			 * RST_RCVD bit, we don't want to accidently catch valid
			 * frames for a closed stream, i.e. RST/PRIO/WU.
			 */
			h3s_error(h3s, H3_ERR_STREAM_CLOSED);
			h3c->st0 = H3_CS_FRAME_E;
			TRACE_DEVEL("leaving in error (rst_rcvd&!hdrmask)", H3_EV_RX_FRAME|H3_EV_RX_FHDR|H3_EV_PROTO_ERR, h3c->conn, h3s);
			return 0;
		}

		/* RFC7540#5.1:closed: if this state is reached as a
		 * result of sending a RST_STREAM frame, the peer that
		 * receives the RST_STREAM might have already sent
		 * frames on the stream that cannot be withdrawn. An
		 * endpoint MUST ignore frames that it receives on
		 * closed streams after it has sent a RST_STREAM
		 * frame. An endpoint MAY choose to limit the period
		 * over which it ignores frames and treat frames that
		 * arrive after this time as being in error.
		 */
		if (h3s->id && !(h3s->flags & H3_SF_RST_SENT)) {
			/* RFC7540#5.1:closed: any frame other than
			 * PRIO/WU/RST in this state MUST be treated as
			 * a connection error
			 */
			if (h3c->dft != H3_FT_RST_STREAM &&
			    h3c->dft != H3_FT_PRIORITY &&
			    h3c->dft != H3_FT_WINDOW_UPDATE) {
				h3c_error(h3c, H3_ERR_STREAM_CLOSED);
				TRACE_DEVEL("leaving in error (rst_sent&!rst&!prio&!wu)", H3_EV_RX_FRAME|H3_EV_RX_FHDR|H3_EV_PROTO_ERR, h3c->conn, h3s);
				return 0;
			}
		}
	}
	TRACE_LEAVE(H3_EV_RX_FRAME|H3_EV_RX_FHDR, h3c->conn, h3s);
	return 1;
}

/* process Rx frames to be demultiplexed */
static void h3_process_demux(struct h3c *h3c)
{
	struct h3s *h3s = NULL, *tmp_h3s;
	struct h3_fh hdr;
	unsigned int padlen = 0;
	int32_t old_iw = h3c->miw;

	TRACE_ENTER(H3_EV_H3C_WAKE, h3c->conn);

	if (h3c->st0 >= H3_CS_ERROR)
		goto out;

	if (unlikely(h3c->st0 < H3_CS_FRAME_H)) {
		if (h3c->st0 == H3_CS_PREFACE) {
			TRACE_STATE("expecting preface", H3_EV_RX_PREFACE, h3c->conn);
			if (h3c->flags & H3_CF_IS_BACK)
				goto out;

			if (unlikely(h3c_frt_recv_preface(h3c) <= 0)) {
				/* RFC7540#3.5: a GOAWAY frame MAY be omitted */
				if (h3c->st0 == H3_CS_ERROR) {
					TRACE_PROTO("failed to receive preface", H3_EV_RX_PREFACE|H3_EV_PROTO_ERR, h3c->conn);
					h3c->st0 = H3_CS_ERROR2;
					sess_log(h3c->conn->owner);
				}
				goto fail;
			}
			TRACE_PROTO("received preface", H3_EV_RX_PREFACE, h3c->conn);

			h3c->max_id = 0;
			h3c->st0 = H3_CS_SETTINGS1;
			TRACE_STATE("switching to SETTINGS1", H3_EV_RX_PREFACE, h3c->conn);
		}

		if (h3c->st0 == H3_CS_SETTINGS1) {
			/* ensure that what is pending is a valid SETTINGS frame
			 * without an ACK.
			 */
			TRACE_STATE("expecting settings", H3_EV_RX_FRAME|H3_EV_RX_FHDR|H3_EV_RX_SETTINGS, h3c->conn);
			if (!h3_get_frame_hdr(&h3c->dbuf, &hdr)) {
				/* RFC7540#3.5: a GOAWAY frame MAY be omitted */
				if (h3c->st0 == H3_CS_ERROR) {
					TRACE_PROTO("failed to receive settings", H3_EV_RX_FRAME|H3_EV_RX_FHDR|H3_EV_RX_SETTINGS|H3_EV_PROTO_ERR, h3c->conn);
					h3c->st0 = H3_CS_ERROR2;
					if (!(h3c->flags & H3_CF_IS_BACK))
						sess_log(h3c->conn->owner);
				}
				goto fail;
			}

			if (hdr.sid || hdr.ft != H3_FT_SETTINGS || hdr.ff & H3_F_SETTINGS_ACK) {
				/* RFC7540#3.5: a GOAWAY frame MAY be omitted */
				TRACE_PROTO("unexpected frame type or flags", H3_EV_RX_FRAME|H3_EV_RX_FHDR|H3_EV_RX_SETTINGS|H3_EV_PROTO_ERR, h3c->conn);
				h3c_error(h3c, H3_ERR_PROTOCOL_ERROR);
				h3c->st0 = H3_CS_ERROR2;
				if (!(h3c->flags & H3_CF_IS_BACK))
					sess_log(h3c->conn->owner);
				goto fail;
			}

			if ((int)hdr.len < 0 || (int)hdr.len > global.tune.bufsize) {
				/* RFC7540#3.5: a GOAWAY frame MAY be omitted */
				TRACE_PROTO("invalid settings frame length", H3_EV_RX_FRAME|H3_EV_RX_FHDR|H3_EV_RX_SETTINGS|H3_EV_PROTO_ERR, h3c->conn);
				h3c_error(h3c, H3_ERR_FRAME_SIZE_ERROR);
				h3c->st0 = H3_CS_ERROR2;
				if (!(h3c->flags & H3_CF_IS_BACK))
					sess_log(h3c->conn->owner);
				goto fail;
			}

			/* that's OK, switch to FRAME_P to process it. This is
			 * a SETTINGS frame whose header has already been
			 * deleted above.
			 */
			padlen = 0;
			goto new_frame;
		}
	}

	/* process as many incoming frames as possible below */
	while (1) {
		int ret = 0;

		if (!b_data(&h3c->dbuf)) {
			TRACE_DEVEL("no more Rx data", H3_EV_RX_FRAME, h3c->conn);
			break;
		}

		if (h3c->st0 >= H3_CS_ERROR) {
			TRACE_STATE("end of connection reported", H3_EV_RX_FRAME|H3_EV_RX_EOI, h3c->conn);
			break;
		}

		if (h3c->st0 == H3_CS_FRAME_H) {
			h3c->rcvd_s = 0;

			TRACE_STATE("expecting H3 frame header", H3_EV_RX_FRAME|H3_EV_RX_FHDR, h3c->conn);
			if (!h3_peek_frame_hdr(&h3c->dbuf, 0, &hdr))
				break;

			if ((int)hdr.len < 0 || (int)hdr.len > global.tune.bufsize) {
				TRACE_PROTO("invalid H3 frame length", H3_EV_RX_FRAME|H3_EV_RX_FHDR|H3_EV_PROTO_ERR, h3c->conn);
				h3c_error(h3c, H3_ERR_FRAME_SIZE_ERROR);
				if (!h3c->nb_streams && !(h3c->flags & H3_CF_IS_BACK)) {
					/* only log if no other stream can report the error */
					sess_log(h3c->conn->owner);
				}
				break;
			}

			padlen = 0;
			if (h3_ft_bit(hdr.ft) & H3_FT_PADDED_MASK && hdr.ff & H3_F_PADDED) {
				/* If the frame is padded (HEADERS, PUSH_PROMISE or DATA),
				 * we read the pad length and drop it from the remaining
				 * payload (one byte + the 9 remaining ones = 10 total
				 * removed), so we have a frame payload starting after the
				 * pad len. Flow controlled frames (DATA) also count the
				 * padlen in the flow control, so it must be adjusted.
				 */
				if (hdr.len < 1) {
					TRACE_PROTO("invalid H3 padded frame length", H3_EV_RX_FRAME|H3_EV_RX_FHDR|H3_EV_PROTO_ERR, h3c->conn);
					h3c_error(h3c, H3_ERR_FRAME_SIZE_ERROR);
					if (!(h3c->flags & H3_CF_IS_BACK))
						sess_log(h3c->conn->owner);
					goto fail;
				}
				hdr.len--;

				if (b_data(&h3c->dbuf) < 10)
					break; // missing padlen

				padlen = *(uint8_t *)b_peek(&h3c->dbuf, 9);

				if (padlen > hdr.len) {
					TRACE_PROTO("invalid H3 padding length", H3_EV_RX_FRAME|H3_EV_RX_FHDR|H3_EV_PROTO_ERR, h3c->conn);
					/* RFC7540#6.1 : pad length = length of
					 * frame payload or greater => error.
					 */
					h3c_error(h3c, H3_ERR_PROTOCOL_ERROR);
					if (!(h3c->flags & H3_CF_IS_BACK))
						sess_log(h3c->conn->owner);
					goto fail;
				}

				if (h3_ft_bit(hdr.ft) & H3_FT_FC_MASK) {
					h3c->rcvd_c++;
					h3c->rcvd_s++;
				}
				b_del(&h3c->dbuf, 1);
			}
			h3_skip_frame_hdr(&h3c->dbuf);

		new_frame:
			h3c->dfl = hdr.len;
			h3c->dsi = hdr.sid;
			h3c->dft = hdr.ft;
			h3c->dff = hdr.ff;
			h3c->dpl = padlen;
			TRACE_STATE("rcvd H3 frame header, switching to FRAME_P state", H3_EV_RX_FRAME|H3_EV_RX_FHDR, h3c->conn);
			h3c->st0 = H3_CS_FRAME_P;

			/* check for minimum basic frame format validity */
			ret = h3_frame_check(h3c->dft, 1, h3c->dsi, h3c->dfl, global.tune.bufsize);
			if (ret != H3_ERR_NO_ERROR) {
				TRACE_PROTO("received invalid H3 frame header", H3_EV_RX_FRAME|H3_EV_RX_FHDR|H3_EV_PROTO_ERR, h3c->conn);
				h3c_error(h3c, ret);
				if (!(h3c->flags & H3_CF_IS_BACK))
					sess_log(h3c->conn->owner);
				goto fail;
			}
		}

		/* Only H3_CS_FRAME_P, H3_CS_FRAME_A and H3_CS_FRAME_E here.
		 * H3_CS_FRAME_P indicates an incomplete previous operation
		 * (most often the first attempt) and requires some validity
		 * checks for the frame and the current state. The two other
		 * ones are set after completion (or abortion) and must skip
		 * validity checks.
		 */
		tmp_h3s = h3c_st_by_id(h3c, h3c->dsi);

		if (tmp_h3s != h3s && h3s && h3s->cs &&
		    (b_data(&h3s->rxbuf) ||
		     conn_xprt_read0_pending(h3c->conn) ||
		     h3s->st == H3_SS_CLOSED ||
		     (h3s->flags & H3_SF_ES_RCVD) ||
		     (h3s->cs->flags & (CS_FL_ERROR|CS_FL_ERR_PENDING|CS_FL_EOS)))) {
			/* we may have to signal the upper layers */
			TRACE_DEVEL("notifying stream before switching SID", H3_EV_RX_FRAME|H3_EV_STRM_WAKE, h3c->conn, h3s);
			h3s->cs->flags |= CS_FL_RCV_MORE;
			h3s_notify_recv(h3s);
		}
		h3s = tmp_h3s;

		if (h3c->st0 == H3_CS_FRAME_E ||
		    (h3c->st0 == H3_CS_FRAME_P && !h3_frame_check_vs_state(h3c, h3s))) {
			TRACE_PROTO("stream error reported", H3_EV_RX_FRAME|H3_EV_PROTO_ERR, h3c->conn, h3s);
			goto strm_err;
		}

		switch (h3c->dft) {
		case H3_FT_SETTINGS:
			if (h3c->st0 == H3_CS_FRAME_P) {
				TRACE_PROTO("receiving H3 SETTINGS frame", H3_EV_RX_FRAME|H3_EV_RX_SETTINGS, h3c->conn, h3s);
				ret = h3c_handle_settings(h3c);
			}

			if (h3c->st0 == H3_CS_FRAME_A) {
				TRACE_PROTO("sending H3 SETTINGS ACK frame", H3_EV_TX_FRAME|H3_EV_RX_SETTINGS, h3c->conn, h3s);
				ret = h3c_ack_settings(h3c);
			}
			break;

		case H3_FT_PING:
			if (h3c->st0 == H3_CS_FRAME_P) {
				TRACE_PROTO("receiving H3 PING frame", H3_EV_RX_FRAME|H3_EV_RX_PING, h3c->conn, h3s);
				ret = h3c_handle_ping(h3c);
			}

			if (h3c->st0 == H3_CS_FRAME_A) {
				TRACE_PROTO("sending H3 PING ACK frame", H3_EV_TX_FRAME|H3_EV_TX_SETTINGS, h3c->conn, h3s);
				ret = h3c_ack_ping(h3c);
			}
			break;

		case H3_FT_WINDOW_UPDATE:
			if (h3c->st0 == H3_CS_FRAME_P) {
				TRACE_PROTO("receiving H3 WINDOW_UPDATE frame", H3_EV_RX_FRAME|H3_EV_RX_WU, h3c->conn, h3s);
				ret = h3c_handle_window_update(h3c, h3s);
			}
			break;

		case H3_FT_CONTINUATION:
			/* RFC7540#6.10: CONTINUATION may only be preceeded by
			 * a HEADERS/PUSH_PROMISE/CONTINUATION frame. These
			 * frames' parsers consume all following CONTINUATION
			 * frames so this one is out of sequence.
			 */
			TRACE_PROTO("received unexpected H3 CONTINUATION frame", H3_EV_RX_FRAME|H3_EV_RX_CONT|H3_EV_H3C_ERR, h3c->conn, h3s);
			h3c_error(h3c, H3_ERR_PROTOCOL_ERROR);
			if (!(h3c->flags & H3_CF_IS_BACK))
				sess_log(h3c->conn->owner);
			goto fail;

		case H3_FT_HEADERS:
			if (h3c->st0 == H3_CS_FRAME_P) {
				TRACE_PROTO("receiving H3 HEADERS frame", H3_EV_RX_FRAME|H3_EV_RX_HDR, h3c->conn, h3s);
				if (h3c->flags & H3_CF_IS_BACK)
					tmp_h3s = h3c_bck_handle_headers(h3c, h3s);
				else
					tmp_h3s = h3c_frt_handle_headers(h3c, h3s);
				if (tmp_h3s) {
					h3s = tmp_h3s;
					ret = 1;
				}
			}
			break;

		case H3_FT_DATA:
			if (h3c->st0 == H3_CS_FRAME_P) {
				TRACE_PROTO("receiving H3 DATA frame", H3_EV_RX_FRAME|H3_EV_RX_DATA, h3c->conn, h3s);
				ret = h3c_frt_handle_data(h3c, h3s);
			}

			if (h3c->st0 == H3_CS_FRAME_A) {
				TRACE_PROTO("sending stream WINDOW_UPDATE frame", H3_EV_TX_FRAME|H3_EV_TX_WU, h3c->conn, h3s);
				ret = h3c_send_strm_wu(h3c);
			}
			break;

		case H3_FT_PRIORITY:
			if (h3c->st0 == H3_CS_FRAME_P) {
				TRACE_PROTO("receiving H3 PRIORITY frame", H3_EV_RX_FRAME|H3_EV_RX_PRIO, h3c->conn, h3s);
				ret = h3c_handle_priority(h3c);
			}
			break;

		case H3_FT_RST_STREAM:
			if (h3c->st0 == H3_CS_FRAME_P) {
				TRACE_PROTO("receiving H3 RST_STREAM frame", H3_EV_RX_FRAME|H3_EV_RX_RST|H3_EV_RX_EOI, h3c->conn, h3s);
				ret = h3c_handle_rst_stream(h3c, h3s);
			}
			break;

		case H3_FT_GOAWAY:
			if (h3c->st0 == H3_CS_FRAME_P) {
				TRACE_PROTO("receiving H3 GOAWAY frame", H3_EV_RX_FRAME|H3_EV_RX_GOAWAY, h3c->conn, h3s);
				ret = h3c_handle_goaway(h3c);
			}
			break;

			/* implement all extra frame types here */
		default:
			TRACE_PROTO("receiving H3 ignored frame", H3_EV_RX_FRAME, h3c->conn, h3s);
			/* drop frames that we ignore. They may be larger than
			 * the buffer so we drain all of their contents until
			 * we reach the end.
			 */
			ret = MIN(b_data(&h3c->dbuf), h3c->dfl);
			b_del(&h3c->dbuf, ret);
			h3c->dfl -= ret;
			ret = h3c->dfl == 0;
		}

	strm_err:
		/* We may have to send an RST if not done yet */
		if (h3s->st == H3_SS_ERROR) {
			TRACE_STATE("stream error, switching to FRAME_E", H3_EV_RX_FRAME|H3_EV_H3S_ERR, h3c->conn, h3s);
			h3c->st0 = H3_CS_FRAME_E;
		}

		if (h3c->st0 == H3_CS_FRAME_E) {
			TRACE_PROTO("sending H3 RST_STREAM frame", H3_EV_TX_FRAME|H3_EV_TX_RST|H3_EV_TX_EOI, h3c->conn, h3s);
			ret = h3c_send_rst_stream(h3c, h3s);
		}

		/* error or missing data condition met above ? */
		if (ret <= 0) {
			TRACE_DEVEL("insufficient data to proceed", H3_EV_RX_FRAME, h3c->conn, h3s);
			break;
		}

		if (h3c->st0 != H3_CS_FRAME_H) {
			TRACE_DEVEL("stream error, skip frame payload", H3_EV_RX_FRAME, h3c->conn, h3s);
			ret = MIN(b_data(&h3c->dbuf), h3c->dfl);
			b_del(&h3c->dbuf, ret);
			h3c->dfl -= ret;
			if (!h3c->dfl) {
				TRACE_STATE("switching to FRAME_H", H3_EV_RX_FRAME|H3_EV_RX_FHDR, h3c->conn);
				h3c->st0 = H3_CS_FRAME_H;
				h3c->dsi = -1;
			}
		}
	}

	if (h3c->rcvd_c > 0 &&
	    !(h3c->flags & (H3_CF_MUX_MFULL | H3_CF_DEM_MBUSY | H3_CF_DEM_MROOM))) {
		TRACE_PROTO("sending H3 WINDOW_UPDATE frame", H3_EV_TX_FRAME|H3_EV_TX_WU, h3c->conn);
		h3c_send_conn_wu(h3c);
	}

 fail:
	/* we can go here on missing data, blocked response or error */
	if (h3s && h3s->cs &&
	    (b_data(&h3s->rxbuf) ||
	     conn_xprt_read0_pending(h3c->conn) ||
	     h3s->st == H3_SS_CLOSED ||
	     (h3s->flags & H3_SF_ES_RCVD) ||
	     (h3s->cs->flags & (CS_FL_ERROR|CS_FL_ERR_PENDING|CS_FL_EOS)))) {
		/* we may have to signal the upper layers */
		TRACE_DEVEL("notifying stream before switching SID", H3_EV_RX_FRAME|H3_EV_H3S_WAKE, h3c->conn, h3s);
		h3s->cs->flags |= CS_FL_RCV_MORE;
		h3s_notify_recv(h3s);
	}

	if (old_iw != h3c->miw) {
		TRACE_STATE("notifying streams about SFCTL increase", H3_EV_RX_FRAME|H3_EV_H3S_WAKE, h3c->conn);
		h3c_unblock_sfctl(h3c);
	}

	h3c_restart_reading(h3c, 0);
 out:
	TRACE_LEAVE(H3_EV_H3C_WAKE, h3c->conn);
}

/* resume each h3s eligible for sending in list head <head> */
static void h3_resume_each_sending_h3s(struct h3c *h3c, struct list *head)
{
	struct h3s *h3s, *h3s_back;

	TRACE_ENTER(H3_EV_H3C_SEND|H3_EV_H3S_WAKE, h3c->conn);

	list_for_each_entry_safe(h3s, h3s_back, head, list) {
		if (h3c->mws <= 0 ||
		    h3c->flags & H3_CF_MUX_BLOCK_ANY ||
		    h3c->st0 >= H3_CS_ERROR)
			break;

		h3s->flags &= ~H3_SF_BLK_ANY;

		if (h3s->flags & H3_SF_NOTIFIED)
			continue;

		/* If the sender changed his mind and unsubscribed, let's just
		 * remove the stream from the send_list.
		 */
		if (!(h3s->flags & (H3_SF_WANT_SHUTR|H3_SF_WANT_SHUTW)) &&
		    (!h3s->subs || !(h3s->subs->events & SUB_RETRY_SEND))) {
			LIST_DEL_INIT(&h3s->list);
			continue;
		}

		if (h3s->subs && h3s->subs->events & SUB_RETRY_SEND) {
			h3s->flags |= H3_SF_NOTIFIED;
			//tasklet_wakeup(h3s->subs->tasklet);
			h3s->subs->events &= ~SUB_RETRY_SEND;
			if (!h3s->subs->events)
				h3s->subs = NULL;
		}
		else if (h3s->flags & (H3_SF_WANT_SHUTR|H3_SF_WANT_SHUTW)) {
			//tasklet_wakeup(h3s->shut_tl);
		}
	}

	TRACE_LEAVE(H3_EV_H3C_SEND|H3_EV_H3S_WAKE, h3c->conn);
}

/* process Tx frames from streams to be multiplexed. Returns > 0 if it reached
 * the end.
 */
static int h3_process_mux(struct h3c *h3c)
{
	TRACE_ENTER(H3_EV_H3C_WAKE, h3c->conn);

	if (unlikely(h3c->st0 < H3_CS_FRAME_H)) {
		if (unlikely(h3c->st0 == H3_CS_PREFACE && (h3c->flags & H3_CF_IS_BACK))) {
			if (unlikely(h3c_bck_send_preface(h3c) <= 0)) {
				/* RFC7540#3.5: a GOAWAY frame MAY be omitted */
				if (h3c->st0 == H3_CS_ERROR)
					h3c->st0 = H3_CS_ERROR2;
				goto fail;
			}
			h3c->st0 = H3_CS_SETTINGS1;
		}
		/* need to wait for the other side */
		if (h3c->st0 < H3_CS_FRAME_H)
			goto done;
	}

	/* start by sending possibly pending window updates */
	if (h3c->rcvd_s > 0 &&
	    !(h3c->flags & (H3_CF_MUX_MFULL | H3_CF_MUX_MALLOC)) &&
	    h3c_send_strm_wu(h3c) < 0)
		goto fail;

	if (h3c->rcvd_c > 0 &&
	    !(h3c->flags & (H3_CF_MUX_MFULL | H3_CF_MUX_MALLOC)) &&
	    h3c_send_conn_wu(h3c) < 0)
		goto fail;

	/* First we always process the flow control list because the streams
	 * waiting there were already elected for immediate emission but were
	 * blocked just on this.
	 */
	h3_resume_each_sending_h3s(h3c, &h3c->fctl_list);
	h3_resume_each_sending_h3s(h3c, &h3c->send_list);

 fail:
	if (unlikely(h3c->st0 >= H3_CS_ERROR)) {
		if (h3c->st0 == H3_CS_ERROR) {
			if (h3c->max_id >= 0) {
				h3c_send_goaway_error(h3c, NULL);
				if (h3c->flags & H3_CF_MUX_BLOCK_ANY)
					goto out0;
			}

			h3c->st0 = H3_CS_ERROR2; // sent (or failed hard) !
		}
	}
 done:
	TRACE_LEAVE(H3_EV_H3C_WAKE, h3c->conn);
	return 1;
 out0:
	TRACE_DEVEL("leaving in blocked situation", H3_EV_H3C_WAKE, h3c->conn);
	return 0;
}


/* Attempt to read data, and subscribe if none available.
 * The function returns 1 if data has been received, otherwise zero.
 */
static int h3_recv(struct h3c *h3c)
{
	struct connection *conn = h3c->conn;
	struct buffer *buf;
	int max;
	size_t ret;

	TRACE_ENTER(H3_EV_H3C_RECV, h3c->conn);

	if (h3c->wait_event.events & SUB_RETRY_RECV) {
		TRACE_DEVEL("leaving on sub_recv", H3_EV_H3C_RECV, h3c->conn);
		return (b_data(&h3c->dbuf));
	}

	if (!h3_recv_allowed(h3c)) {
		TRACE_DEVEL("leaving on !recv_allowed", H3_EV_H3C_RECV, h3c->conn);
		return 1;
	}

	buf = h3_get_buf(h3c, &h3c->dbuf);
	if (!buf) {
		h3c->flags |= H3_CF_DEM_DALLOC;
		TRACE_DEVEL("leaving on !alloc", H3_EV_H3C_RECV, h3c->conn);
		return 0;
	}

	b_realign_if_empty(buf);
	if (!b_data(buf)) {
		/* try to pre-align the buffer like the
		 * rxbufs will be to optimize memory copies. We'll make
		 * sure that the frame header lands at the end of the
		 * HTX block to alias it upon recv. We cannot use the
		 * head because rcv_buf() will realign the buffer if
		 * it's empty. Thus we cheat and pretend we already
		 * have a few bytes there.
		 */
		max = buf_room_for_htx_data(buf) + 9;
		buf->head = sizeof(struct htx) - 9;
	}
	else
		max = b_room(buf);

	ret = max ? conn->xprt->rcv_buf(conn, conn->xprt_ctx, buf, max, 0) : 0;

	if (max && !ret && h3_recv_allowed(h3c)) {
		TRACE_DATA("failed to receive data, subscribing", H3_EV_H3C_RECV, h3c->conn);
		conn->xprt->subscribe(conn, conn->xprt_ctx, SUB_RETRY_RECV, &h3c->wait_event);
	} else if (ret)
		TRACE_DATA("received data", H3_EV_H3C_RECV, h3c->conn,,, (void*)(long)ret);

	if (!b_data(buf)) {
		h3_release_buf(h3c, &h3c->dbuf);
		TRACE_LEAVE(H3_EV_H3C_RECV, h3c->conn);
		return (conn->flags & CO_FL_ERROR || conn_xprt_read0_pending(conn));
	}

	if (b_data(buf) == buf->size) {
		h3c->flags |= H3_CF_DEM_DFULL;
		TRACE_STATE("demux buffer full", H3_EV_H3C_RECV|H3_EV_H3C_BLK, h3c->conn);
	}

	TRACE_LEAVE(H3_EV_H3C_RECV, h3c->conn);
	return !!ret || (conn->flags & CO_FL_ERROR) || conn_xprt_read0_pending(conn);
}

/* Try to send data if possible.
 * The function returns 1 if data have been sent, otherwise zero.
 */
static int h3_send(struct h3c *h3c)
{
	struct connection *conn = h3c->conn;
	int done;
	int sent = 0;

	TRACE_ENTER(H3_EV_H3C_SEND, h3c->conn);

	if (conn->flags & CO_FL_ERROR) {
		TRACE_DEVEL("leaving on error", H3_EV_H3C_SEND, h3c->conn);
		return 1;
	}

	if (conn->flags & CO_FL_WAIT_XPRT) {
		/* a handshake was requested */
		goto schedule;
	}

	/* This loop is quite simple : it tries to fill as much as it can from
	 * pending streams into the existing buffer until it's reportedly full
	 * or the end of send requests is reached. Then it tries to send this
	 * buffer's contents out, marks it not full if at least one byte could
	 * be sent, and tries again.
	 *
	 * The snd_buf() function normally takes a "flags" argument which may
	 * be made of a combination of CO_SFL_MSG_MORE to indicate that more
	 * data immediately comes and CO_SFL_STREAMER to indicate that the
	 * connection is streaming lots of data (used to increase TLS record
	 * size at the expense of latency). The former can be sent any time
	 * there's a buffer full flag, as it indicates at least one stream
	 * attempted to send and failed so there are pending data. An
	 * alternative would be to set it as long as there's an active stream
	 * but that would be problematic for ACKs until we have an absolute
	 * guarantee that all waiters have at least one byte to send. The
	 * latter should possibly not be set for now.
	 */

	done = 0;
	while (!done) {
		unsigned int flags = 0;
		unsigned int released = 0;
		struct buffer *buf;

		/* fill as much as we can into the current buffer */
		while (((h3c->flags & (H3_CF_MUX_MFULL|H3_CF_MUX_MALLOC)) == 0) && !done)
			done = h3_process_mux(h3c);

		if (h3c->flags & H3_CF_MUX_MALLOC)
			done = 1; // we won't go further without extra buffers

		if (conn->flags & CO_FL_ERROR)
			break;

		if (h3c->flags & (H3_CF_MUX_MFULL | H3_CF_DEM_MBUSY | H3_CF_DEM_MROOM))
			flags |= CO_SFL_MSG_MORE;

		for (buf = br_head(h3c->mbuf); b_size(buf); buf = br_del_head(h3c->mbuf)) {
			if (b_data(buf)) {
				int ret = conn->xprt->snd_buf(conn, conn->xprt_ctx, buf, b_data(buf), flags);
				if (!ret) {
					done = 1;
					break;
				}
				sent = 1;
				TRACE_DATA("sent data", H3_EV_H3C_SEND, h3c->conn,, buf, (void*)(long)ret);
				b_del(buf, ret);
				if (b_data(buf)) {
					done = 1;
					break;
				}
			}
			b_free(buf);
			released++;
		}

		if (released)
			offer_buffers(NULL, tasks_run_queue);

		/* wrote at least one byte, the buffer is not full anymore */
		if (sent)
			h3c->flags &= ~(H3_CF_MUX_MFULL | H3_CF_DEM_MROOM);
	}

	if (conn->flags & CO_FL_SOCK_WR_SH) {
		/* output closed, nothing to send, clear the buffer to release it */
		b_reset(br_tail(h3c->mbuf));
	}
	/* We're not full anymore, so we can wake any task that are waiting
	 * for us.
	 */
	if (!(h3c->flags & (H3_CF_MUX_MFULL | H3_CF_DEM_MROOM)) && h3c->st0 >= H3_CS_FRAME_H)
		h3_resume_each_sending_h3s(h3c, &h3c->send_list);

	/* We're done, no more to send */
	if (!br_data(h3c->mbuf)) {
		TRACE_DEVEL("leaving with everything sent", H3_EV_H3C_SEND, h3c->conn);
		return sent;
	}
schedule:
	if (!(conn->flags & CO_FL_ERROR) && !(h3c->wait_event.events & SUB_RETRY_SEND)) {
		TRACE_STATE("more data to send, subscribing", H3_EV_H3C_SEND, h3c->conn);
		conn->xprt->subscribe(conn, conn->xprt_ctx, SUB_RETRY_SEND, &h3c->wait_event);
	}

	TRACE_DEVEL("leaving with some data left to send", H3_EV_H3C_SEND, h3c->conn);
	return sent;
}

/* this is the tasklet referenced in h3c->wait_event.tasklet */
static struct task *h3_io_cb(struct task *t, void *ctx, unsigned short status)
{
	struct connection *conn;
	struct tasklet *tl = (struct tasklet *)t;
	int conn_in_list;
	struct h3c *h3c;
	int ret = 0;


	HA_SPIN_LOCK(OTHER_LOCK, &toremove_lock[tid]);
	if (t->context == NULL) {
		/* The connection has been taken over by another thread,
		 * we're no longer responsible for it, so just free the
		 * tasklet, and do nothing.
		 */
		HA_SPIN_UNLOCK(OTHER_LOCK, &toremove_lock[tid]);
		tasklet_free(tl);
		return NULL;
	}
	h3c = ctx;
	conn = h3c->conn;

	TRACE_ENTER(H3_EV_H3C_WAKE, conn);

	conn_in_list = conn->flags & CO_FL_LIST_MASK;

	/* Remove the connection from the list, to be sure nobody attempts
	 * to use it while we handle the I/O events
	 */
	if (conn_in_list)
		MT_LIST_DEL(&conn->list);

	HA_SPIN_UNLOCK(OTHER_LOCK, &toremove_lock[tid]);

	if (!(h3c->wait_event.events & SUB_RETRY_SEND))
		ret = h3_send(h3c);
	if (!(h3c->wait_event.events & SUB_RETRY_RECV))
		ret |= h3_recv(h3c);
	if (ret || b_data(&h3c->dbuf))
		ret = h3_process(h3c);

	/* If we were in an idle list, we want to add it back into it,
	 * unless h3_process() returned -1, which mean it has destroyed
	 * the connection (testing !ret is enough, if h3_process() wasn't
	 * called then ret will be 0 anyway.
	 */
	if (!ret && conn_in_list) {
		struct server *srv = objt_server(conn->target);

		if (conn_in_list == CO_FL_SAFE_LIST)
			MT_LIST_ADDQ(&srv->safe_conns[tid], &conn->list);
		else
			MT_LIST_ADDQ(&srv->idle_conns[tid], &conn->list);
	}

	TRACE_LEAVE(H3_EV_H3C_WAKE);
	return NULL;
}

/* callback called on any event by the connection handler.
 * It applies changes and returns zero, or < 0 if it wants immediate
 * destruction of the connection (which normally doesn not happen in h3).
 */
static int h3_process(struct h3c *h3c)
{
	struct connection *conn = h3c->conn;

	TRACE_ENTER(H3_EV_H3C_WAKE, conn);

	if (b_data(&h3c->dbuf) && !(h3c->flags & H3_CF_DEM_BLOCK_ANY)) {
		h3_process_demux(h3c);

		if (h3c->st0 >= H3_CS_ERROR || conn->flags & CO_FL_ERROR)
			b_reset(&h3c->dbuf);

		if (!b_full(&h3c->dbuf))
			h3c->flags &= ~H3_CF_DEM_DFULL;
	}
	h3_send(h3c);

	if (unlikely(h3c->proxy->state == PR_STSTOPPED)) {
		/* frontend is stopping, reload likely in progress, let's try
		 * to announce a graceful shutdown if not yet done. We don't
		 * care if it fails, it will be tried again later.
		 */
		TRACE_STATE("proxy stopped, sending GOAWAY", H3_EV_H3C_WAKE|H3_EV_TX_FRAME, conn);
		if (!(h3c->flags & (H3_CF_GOAWAY_SENT|H3_CF_GOAWAY_FAILED))) {
			if (h3c->last_sid < 0)
				h3c->last_sid = (1U << 31) - 1;
			h3c_send_goaway_error(h3c, NULL);
		}
	}

	/*
	 * If we received early data, and the handshake is done, wake
	 * any stream that was waiting for it.
	 */
	if (!(h3c->flags & H3_CF_WAIT_FOR_HS) &&
	    (conn->flags & (CO_FL_EARLY_SSL_HS | CO_FL_WAIT_XPRT | CO_FL_EARLY_DATA)) == CO_FL_EARLY_DATA) {
		struct eb32_node *node;
		struct h3s *h3s;

		h3c->flags |= H3_CF_WAIT_FOR_HS;
		node = eb32_lookup_ge(&h3c->streams_by_id, 1);

		while (node) {
			h3s = container_of(node, struct h3s, by_id);
			if (h3s->cs && h3s->cs->flags & CS_FL_WAIT_FOR_HS)
				h3s_notify_recv(h3s);
			node = eb32_next(node);
		}
	}

	if (conn->flags & CO_FL_ERROR || conn_xprt_read0_pending(conn) ||
	    h3c->st0 == H3_CS_ERROR2 || h3c->flags & H3_CF_GOAWAY_FAILED ||
	    (eb_is_empty(&h3c->streams_by_id) && h3c->last_sid >= 0 &&
	     h3c->max_id >= h3c->last_sid)) {
		h3_wake_some_streams(h3c, 0);

		if (eb_is_empty(&h3c->streams_by_id)) {
			/* no more stream, kill the connection now */
			h3_release(h3c);
			TRACE_DEVEL("leaving after releasing the connection", H3_EV_H3C_WAKE);
			return -1;
		}

		/* connections in error must be removed from the idle lists */
		HA_SPIN_LOCK(OTHER_LOCK, &toremove_lock[tid]);
		MT_LIST_DEL((struct mt_list *)&conn->list);
		HA_SPIN_UNLOCK(OTHER_LOCK, &toremove_lock[tid]);
	}
	else if (h3c->st0 == H3_CS_ERROR) {
		/* connections in error must be removed from the idle lists */
		HA_SPIN_LOCK(OTHER_LOCK, &toremove_lock[tid]);
		MT_LIST_DEL((struct mt_list *)&conn->list);
		HA_SPIN_UNLOCK(OTHER_LOCK, &toremove_lock[tid]);
	}

	if (!b_data(&h3c->dbuf))
		h3_release_buf(h3c, &h3c->dbuf);

	if ((conn->flags & CO_FL_SOCK_WR_SH) ||
	    h3c->st0 == H3_CS_ERROR2 || (h3c->flags & H3_CF_GOAWAY_FAILED) ||
	    (h3c->st0 != H3_CS_ERROR &&
	     !br_data(h3c->mbuf) &&
	     (h3c->mws <= 0 || LIST_ISEMPTY(&h3c->fctl_list)) &&
	     ((h3c->flags & H3_CF_MUX_BLOCK_ANY) || LIST_ISEMPTY(&h3c->send_list))))
		h3_release_mbuf(h3c);

	if (h3c->task) {
		if (h3c_may_expire(h3c))
			h3c->task->expire = tick_add(now_ms, h3c->last_sid < 0 ? h3c->timeout : h3c->shut_timeout);
		else
			h3c->task->expire = TICK_ETERNITY;
		task_queue(h3c->task);
	}

	h3_send(h3c);
	TRACE_LEAVE(H3_EV_H3C_WAKE, conn);
	return 0;
}

/* wake-up function called by the connection layer (mux_ops.wake) */
static int h3_wake(struct connection *conn)
{
	struct h3c *h3c = conn->ctx;
	int ret;

	TRACE_ENTER(H3_EV_H3C_WAKE, conn);
	ret = h3_process(h3c);
	if (ret >= 0)
		h3_wake_some_streams(h3c, 0);
	TRACE_LEAVE(H3_EV_H3C_WAKE);
	return ret;
}

/* Connection timeout management. The principle is that if there's no receipt
 * nor sending for a certain amount of time, the connection is closed. If the
 * MUX buffer still has lying data or is not allocatable, the connection is
 * immediately killed. If it's allocatable and empty, we attempt to send a
 * GOAWAY frame.
 */
static struct task *h3_timeout_task(struct task *t, void *context, unsigned short state)
{
	struct h3c *h3c = context;
	int expired = tick_is_expired(t->expire, now_ms);

	TRACE_ENTER(H3_EV_H3C_WAKE, h3c ? h3c->conn : NULL);

	if (!expired && h3c) {
		TRACE_DEVEL("leaving (not expired)", H3_EV_H3C_WAKE, h3c->conn);
		return t;
	}

	if (h3c && !h3c_may_expire(h3c)) {
		/* we do still have streams but all of them are idle, waiting
		 * for the data layer, so we must not enforce the timeout here.
		 */
		t->expire = TICK_ETERNITY;
		return t;
	}

	/* We're about to destroy the connection, so make sure nobody attempts
	 * to steal it from us.
	 */
	HA_SPIN_LOCK(OTHER_LOCK, &toremove_lock[tid]);

	if (h3c && h3c->conn->flags & CO_FL_LIST_MASK)
		MT_LIST_DEL(&h3c->conn->list);

	/* Somebody already stole the connection from us, so we should not
	 * free it, we just have to free the task.
	 */
	if (!t->context)
		h3c = NULL;

	HA_SPIN_UNLOCK(OTHER_LOCK, &toremove_lock[tid]);

	task_destroy(t);

	if (!h3c) {
		/* resources were already deleted */
		TRACE_DEVEL("leaving (not more h3c)", H3_EV_H3C_WAKE);
		return NULL;
	}

	h3c->task = NULL;
	h3c_error(h3c, H3_ERR_NO_ERROR);
	h3_wake_some_streams(h3c, 0);

	if (br_data(h3c->mbuf)) {
		/* don't even try to send a GOAWAY, the buffer is stuck */
		h3c->flags |= H3_CF_GOAWAY_FAILED;
	}

	/* try to send but no need to insist */
	h3c->last_sid = h3c->max_id;
	if (h3c_send_goaway_error(h3c, NULL) <= 0)
		h3c->flags |= H3_CF_GOAWAY_FAILED;

	if (br_data(h3c->mbuf) && !(h3c->flags & H3_CF_GOAWAY_FAILED) && conn_xprt_ready(h3c->conn)) {
		unsigned int released = 0;
		struct buffer *buf;

		for (buf = br_head(h3c->mbuf); b_size(buf); buf = br_del_head(h3c->mbuf)) {
			if (b_data(buf)) {
				int ret = h3c->conn->xprt->snd_buf(h3c->conn, h3c->conn->xprt_ctx, buf, b_data(buf), 0);
				if (!ret)
					break;
				b_del(buf, ret);
				if (b_data(buf))
					break;
				b_free(buf);
				released++;
			}
		}

		if (released)
			offer_buffers(NULL, tasks_run_queue);
	}

	/* in any case this connection must not be considered idle anymore */
	HA_SPIN_LOCK(OTHER_LOCK, &toremove_lock[tid]);
	MT_LIST_DEL((struct mt_list *)&h3c->conn->list);
	HA_SPIN_UNLOCK(OTHER_LOCK, &toremove_lock[tid]);

	/* either we can release everything now or it will be done later once
	 * the last stream closes.
	 */
	if (eb_is_empty(&h3c->streams_by_id))
		h3_release(h3c);

	TRACE_LEAVE(H3_EV_H3C_WAKE);
	return NULL;
}


/*******************************************/
/* functions below are used by the streams */
/*******************************************/

/*
 * Attach a new stream to a connection
 * (Used for outgoing connections)
 */
static struct conn_stream *h3_attach(struct connection *conn, struct session *sess)
{
	struct conn_stream *cs;
	struct h3s *h3s;
	struct h3c *h3c = conn->ctx;

	TRACE_ENTER(H3_EV_H3S_NEW, conn);
	cs = cs_new(conn);
	if (!cs) {
		TRACE_DEVEL("leaving on CS allocation failure", H3_EV_H3S_NEW|H3_EV_H3S_ERR, conn);
		return NULL;
	}
	h3s = h3c_bck_stream_new(h3c, cs, sess);
	if (!h3s) {
		TRACE_DEVEL("leaving on stream creation failure", H3_EV_H3S_NEW|H3_EV_H3S_ERR, conn);
		cs_free(cs);
		return NULL;
	}
	TRACE_LEAVE(H3_EV_H3S_NEW, conn, h3s);
	return cs;
}

/* Retrieves the first valid conn_stream from this connection, or returns NULL.
 * We have to scan because we may have some orphan streams. It might be
 * beneficial to scan backwards from the end to reduce the likeliness to find
 * orphans.
 */
static const struct conn_stream *h3_get_first_cs(const struct connection *conn)
{
	struct h3c *h3c = conn->ctx;
	struct h3s *h3s;
	struct eb32_node *node;

	node = eb32_first(&h3c->streams_by_id);
	while (node) {
		h3s = container_of(node, struct h3s, by_id);
		if (h3s->cs)
			return h3s->cs;
		node = eb32_next(node);
	}
	return NULL;
}

static int h3_ctl(struct connection *conn, enum mux_ctl_type mux_ctl, void *output)
{
	int ret = 0;
	struct h3c *h3c = conn->ctx;

	switch (mux_ctl) {
	case MUX_STATUS:
		/* Only consider the mux to be ready if we're done with
		 * the preface and settings, and we had no error.
		 */
		if (h3c->st0 >= H3_CS_FRAME_H && h3c->st0 < H3_CS_ERROR)
			ret |= MUX_STATUS_READY;
		return ret;
	default:
		return -1;
	}
}

/*
 * Destroy the mux and the associated connection, if it is no longer used
 */
static void h3_destroy(void *ctx)
{
	struct h3c *h3c = ctx;

	TRACE_ENTER(H3_EV_H3C_END, h3c->conn);
	if (eb_is_empty(&h3c->streams_by_id) || !h3c->conn || h3c->conn->ctx != h3c)
		h3_release(h3c);
	TRACE_LEAVE(H3_EV_H3C_END);
}

/*
 * Detach the stream from the connection and possibly release the connection.
 */
static void h3_detach(struct conn_stream *cs)
{
	struct h3s *h3s = cs->ctx;
	struct h3c *h3c;
	struct session *sess;

	TRACE_ENTER(H3_EV_STRM_END, h3s ? h3s->h3c->conn : NULL, h3s);

	cs->ctx = NULL;
	if (!h3s) {
		TRACE_LEAVE(H3_EV_STRM_END);
		return;
	}

	/* there's no txbuf so we're certain not to be able to send anything */
	h3s->flags &= ~H3_SF_NOTIFIED;

	sess = h3s->sess;
	h3c = h3s->h3c;
	h3s->cs = NULL;
	h3c->nb_cs--;
	if ((h3c->flags & (H3_CF_IS_BACK|H3_CF_DEM_TOOMANY)) == H3_CF_DEM_TOOMANY &&
	    !h3_frt_has_too_many_cs(h3c)) {
		/* frontend connection was blocking new streams creation */
		h3c->flags &= ~H3_CF_DEM_TOOMANY;
		h3c_restart_reading(h3c, 1);
	}

	/* this stream may be blocked waiting for some data to leave (possibly
	 * an ES or RST frame), so orphan it in this case.
	 */
	if (!(cs->conn->flags & CO_FL_ERROR) &&
	    (h3c->st0 < H3_CS_ERROR) &&
	    (h3s->flags & (H3_SF_BLK_MBUSY | H3_SF_BLK_MROOM | H3_SF_BLK_MFCTL)) &&
	    ((h3s->flags & (H3_SF_WANT_SHUTR | H3_SF_WANT_SHUTW)) || h3s->subs)) {
		TRACE_DEVEL("leaving on stream blocked", H3_EV_STRM_END|H3_EV_H3S_BLK, h3c->conn, h3s);
		return;
	}

	if ((h3c->flags & H3_CF_DEM_BLOCK_ANY && h3s->id == h3c->dsi) ||
	    (h3c->flags & H3_CF_MUX_BLOCK_ANY && h3s->id == h3c->msi)) {
		/* unblock the connection if it was blocked on this
		 * stream.
		 */
		h3c->flags &= ~H3_CF_DEM_BLOCK_ANY;
		h3c->flags &= ~H3_CF_MUX_BLOCK_ANY;
		h3c_restart_reading(h3c, 1);
	}

	h3s_destroy(h3s);

	if (h3c->flags & H3_CF_IS_BACK) {
		if (!(h3c->conn->flags &
		    (CO_FL_ERROR | CO_FL_SOCK_RD_SH | CO_FL_SOCK_WR_SH))) {
			/* Never ever allow to reuse a connection from a non-reuse backend */
			if ((h3c->proxy->options & PR_O_REUSE_MASK) == PR_O_REUSE_NEVR)
				h3c->conn->flags |= CO_FL_PRIVATE;
			if (!h3c->conn->owner && (h3c->conn->flags & CO_FL_PRIVATE)) {
				h3c->conn->owner = sess;
				if (!session_add_conn(sess, h3c->conn, h3c->conn->target)) {
					h3c->conn->owner = NULL;
					if (eb_is_empty(&h3c->streams_by_id)) {
						h3c->conn->mux->destroy(h3c);
						TRACE_DEVEL("leaving on error after killing outgoing connection", H3_EV_STRM_END|H3_EV_H3C_ERR);
						return;
					}
				}
			}
			if (eb_is_empty(&h3c->streams_by_id)) {
				if (sess && h3c->conn->owner == sess &&
				    session_check_idle_conn(h3c->conn->owner, h3c->conn) != 0) {
					/* At this point either the connection is destroyed, or it's been added to the server idle list, just stop */
					TRACE_DEVEL("leaving without reusable idle connection", H3_EV_STRM_END);
					return;
				}
				if (!(h3c->conn->flags & CO_FL_PRIVATE)) {
					if (!srv_add_to_idle_list(objt_server(h3c->conn->target), h3c->conn, 1)) {
						/* The server doesn't want it, let's kill the connection right away */
						h3c->conn->mux->destroy(h3c);
						TRACE_DEVEL("leaving on error after killing outgoing connection", H3_EV_STRM_END|H3_EV_H3C_ERR);
						return;
					}
					/* At this point, the connection has been added to the
					 * server idle list, so another thread may already have
					 * hijacked it, so we can't do anything with it.
					 */
					TRACE_DEVEL("reusable idle connection", H3_EV_STRM_END);
					return;

				}
			} else if (MT_LIST_ISEMPTY(&h3c->conn->list) &&
			           h3_avail_streams(h3c->conn) > 0 && objt_server(h3c->conn->target)) {
				LIST_ADD(&__objt_server(h3c->conn->target)->available_conns[tid], mt_list_to_list(&h3c->conn->list));
			}
		}
	}

	/* We don't want to close right now unless we're removing the
	 * last stream, and either the connection is in error, or it
	 * reached the ID already specified in a GOAWAY frame received
	 * or sent (as seen by last_sid >= 0).
	 */
	if (h3c_is_dead(h3c)) {
		/* no more stream will come, kill it now */
		TRACE_DEVEL("leaving and killing dead connection", H3_EV_STRM_END, h3c->conn);
		h3_release(h3c);
	}
	else if (h3c->task) {
		if (h3c_may_expire(h3c))
			h3c->task->expire = tick_add(now_ms, h3c->last_sid < 0 ? h3c->timeout : h3c->shut_timeout);
		else
			h3c->task->expire = TICK_ETERNITY;
		task_queue(h3c->task);
		TRACE_DEVEL("leaving, refreshing connection's timeout", H3_EV_STRM_END, h3c->conn);
	}
	else
		TRACE_DEVEL("leaving", H3_EV_STRM_END, h3c->conn);
}

/* Performs a synchronous or asynchronous shutr(). */
static void h3_do_shutr(struct h3s *h3s)
{
	struct h3c *h3c = h3s->h3c;

	if (h3s->st == H3_SS_CLOSED)
		goto done;

	TRACE_ENTER(H3_EV_STRM_SHUT, h3c->conn, h3s);

	/* a connstream may require us to immediately kill the whole connection
	 * for example because of a "tcp-request content reject" rule that is
	 * normally used to limit abuse. In this case we schedule a goaway to
	 * close the connection.
	 */
	if ((h3s->flags & H3_SF_KILL_CONN) &&
	    !(h3c->flags & (H3_CF_GOAWAY_SENT|H3_CF_GOAWAY_FAILED))) {
		TRACE_STATE("stream wants to kill the connection", H3_EV_STRM_SHUT, h3c->conn, h3s);
		h3c_error(h3c, H3_ERR_ENHANCE_YOUR_CALM);
		h3s_error(h3s, H3_ERR_ENHANCE_YOUR_CALM);
	}
	else if (!(h3s->flags & H3_SF_HEADERS_SENT)) {
		/* Nothing was never sent for this stream, so reset with
		 * REFUSED_STREAM error to let the client retry the
		 * request.
		 */
		TRACE_STATE("no headers sent yet, trying a retryable abort", H3_EV_STRM_SHUT, h3c->conn, h3s);
		h3s_error(h3s, H3_ERR_REFUSED_STREAM);
	}
	else {
		/* a final response was already provided, we don't want this
		 * stream anymore. This may happen when the server responds
		 * before the end of an upload and closes quickly (redirect,
		 * deny, ...)
		 */
		h3s_error(h3s, H3_ERR_CANCEL);
	}

	if (!(h3s->flags & H3_SF_RST_SENT) &&
	    h3s_send_rst_stream(h3c, h3s) <= 0)
		goto add_to_list;

	if (!(h3c->wait_event.events & SUB_RETRY_SEND)) { /* XXX XXX XXX XXX XXX */ }
		//tasklet_wakeup(h3c->wait_event.tasklet);
	h3s_close(h3s);
 done:
	h3s->flags &= ~H3_SF_WANT_SHUTR;
	TRACE_LEAVE(H3_EV_STRM_SHUT, h3c->conn, h3s);
	return;
add_to_list:
	/* Let the handler know we want to shutr, and add ourselves to the
	 * most relevant list if not yet done. h3_deferred_shut() will be
	 * automatically called via the shut_tl tasklet when there's room
	 * again.
	 */
	h3s->flags |= H3_SF_WANT_SHUTR;
	if (!LIST_ADDED(&h3s->list)) {
		if (h3s->flags & H3_SF_BLK_MFCTL)
			LIST_ADDQ(&h3c->fctl_list, &h3s->list);
		else if (h3s->flags & (H3_SF_BLK_MBUSY|H3_SF_BLK_MROOM))
			LIST_ADDQ(&h3c->send_list, &h3s->list);
	}
	TRACE_LEAVE(H3_EV_STRM_SHUT, h3c->conn, h3s);
	return;
}

/* Performs a synchronous or asynchronous shutw(). */
static void h3_do_shutw(struct h3s *h3s)
{
	struct h3c *h3c = h3s->h3c;

	if (h3s->st == H3_SS_HLOC || h3s->st == H3_SS_CLOSED)
		goto done;

	TRACE_ENTER(H3_EV_STRM_SHUT, h3c->conn, h3s);

	if (h3s->st != H3_SS_ERROR && (h3s->flags & H3_SF_HEADERS_SENT)) {
		/* we can cleanly close using an empty data frame only after headers */

		if (!(h3s->flags & (H3_SF_ES_SENT|H3_SF_RST_SENT)) &&
		    h3_send_empty_data_es(h3s) <= 0)
			goto add_to_list;

		if (h3s->st == H3_SS_HREM)
			h3s_close(h3s);
		else
			h3s->st = H3_SS_HLOC;
	} else {
		/* a connstream may require us to immediately kill the whole connection
		 * for example because of a "tcp-request content reject" rule that is
		 * normally used to limit abuse. In this case we schedule a goaway to
		 * close the connection.
		 */
		if ((h3s->flags & H3_SF_KILL_CONN) &&
		    !(h3c->flags & (H3_CF_GOAWAY_SENT|H3_CF_GOAWAY_FAILED))) {
			TRACE_STATE("stream wants to kill the connection", H3_EV_STRM_SHUT, h3c->conn, h3s);
			h3c_error(h3c, H3_ERR_ENHANCE_YOUR_CALM);
			h3s_error(h3s, H3_ERR_ENHANCE_YOUR_CALM);
		}
		else {
			/* Nothing was never sent for this stream, so reset with
			 * REFUSED_STREAM error to let the client retry the
			 * request.
			 */
			TRACE_STATE("no headers sent yet, trying a retryable abort", H3_EV_STRM_SHUT, h3c->conn, h3s);
			h3s_error(h3s, H3_ERR_REFUSED_STREAM);
		}

		if (!(h3s->flags & H3_SF_RST_SENT) &&
		    h3s_send_rst_stream(h3c, h3s) <= 0)
			goto add_to_list;

		h3s_close(h3s);
	}

	if (!(h3c->wait_event.events & SUB_RETRY_SEND)) { /* XXX XXX XXX XXX XXX */ }
		//tasklet_wakeup(h3c->wait_event.tasklet);

	TRACE_LEAVE(H3_EV_STRM_SHUT, h3c->conn, h3s);

 done:
	h3s->flags &= ~H3_SF_WANT_SHUTW;
	return;

 add_to_list:
	/* Let the handler know we want to shutw, and add ourselves to the
	 * most relevant list if not yet done. h3_deferred_shut() will be
	 * automatically called via the shut_tl tasklet when there's room
	 * again.
	 */
	h3s->flags |= H3_SF_WANT_SHUTW;
	if (!LIST_ADDED(&h3s->list)) {
		if (h3s->flags & H3_SF_BLK_MFCTL)
			LIST_ADDQ(&h3c->fctl_list, &h3s->list);
		else if (h3s->flags & (H3_SF_BLK_MBUSY|H3_SF_BLK_MROOM))
			LIST_ADDQ(&h3c->send_list, &h3s->list);
	}
	TRACE_LEAVE(H3_EV_STRM_SHUT, h3c->conn, h3s);
	return;
}

/* This is the tasklet referenced in h3s->shut_tl, it is used for
 * deferred shutdowns when the h3_detach() was done but the mux buffer was full
 * and prevented the last frame from being emitted.
 */
static struct task *h3_deferred_shut(struct task *t, void *ctx, unsigned short state)
{
	struct h3s *h3s = ctx;
	struct h3c *h3c = h3s->h3c;

	TRACE_ENTER(H3_EV_STRM_SHUT, h3c->conn, h3s);

	if (h3s->flags & H3_SF_NOTIFIED) {
		/* some data processing remains to be done first */
		goto end;
	}

	if (h3s->flags & H3_SF_WANT_SHUTW)
		h3_do_shutw(h3s);

	if (h3s->flags & H3_SF_WANT_SHUTR)
		h3_do_shutr(h3s);

	if (!(h3s->flags & (H3_SF_WANT_SHUTR|H3_SF_WANT_SHUTW))) {
		/* We're done trying to send, remove ourself from the send_list */
		LIST_DEL_INIT(&h3s->list);

		if (!h3s->cs) {
			h3s_destroy(h3s);
			if (h3c_is_dead(h3c))
				h3_release(h3c);
		}
	}
 end:
	TRACE_LEAVE(H3_EV_STRM_SHUT);
	return NULL;
}

/* shutr() called by the conn_stream (mux_ops.shutr) */
static void h3_shutr(struct conn_stream *cs, enum cs_shr_mode mode)
{
	struct h3s *h3s = cs->ctx;

	TRACE_ENTER(H3_EV_STRM_SHUT, h3s->h3c->conn, h3s);
	if (cs->flags & CS_FL_KILL_CONN)
		h3s->flags |= H3_SF_KILL_CONN;

	if (mode)
		h3_do_shutr(h3s);

	TRACE_LEAVE(H3_EV_STRM_SHUT, h3s->h3c->conn, h3s);
}

/* shutw() called by the conn_stream (mux_ops.shutw) */
static void h3_shutw(struct conn_stream *cs, enum cs_shw_mode mode)
{
	struct h3s *h3s = cs->ctx;

	TRACE_ENTER(H3_EV_STRM_SHUT, h3s->h3c->conn, h3s);
	if (cs->flags & CS_FL_KILL_CONN)
		h3s->flags |= H3_SF_KILL_CONN;

	h3_do_shutw(h3s);
	TRACE_LEAVE(H3_EV_STRM_SHUT, h3s->h3c->conn, h3s);
}

/* Decode the payload of a HEADERS frame and produce the HTX request or response
 * depending on the connection's side. Returns a positive value on success, a
 * negative value on failure, or 0 if it couldn't proceed. May report connection
 * errors in h3c->errcode if the frame is non-decodable and the connection
 * unrecoverable. In absence of connection error when a failure is reported, the
 * caller must assume a stream error.
 *
 * The function may fold CONTINUATION frames into the initial HEADERS frame
 * by removing padding and next frame header, then moving the CONTINUATION
 * frame's payload and adjusting h3c->dfl to match the new aggregated frame,
 * leaving a hole between the main frame and the beginning of the next one.
 * The possibly remaining incomplete or next frame at the end may be moved
 * if the aggregated frame is not deleted, in order to fill the hole. Wrapped
 * HEADERS frames are unwrapped into a temporary buffer before decoding.
 *
 * A buffer at the beginning of processing may look like this :
 *
 *  ,---.---------.-----.--------------.--------------.------.---.
 *  |///| HEADERS | PAD | CONTINUATION | CONTINUATION | DATA |///|
 *  `---^---------^-----^--------------^--------------^------^---'
 *  |   |         <----->                                    |   |
 * area |           dpl                                      |  wrap
 *      |<-------------->                                    |
 *      |       dfl                                          |
 *      |<-------------------------------------------------->|
 *    head                    data
 *
 * Padding is automatically overwritten when folding, participating to the
 * hole size after dfl :
 *
 *  ,---.------------------------.-----.--------------.------.---.
 *  |///| HEADERS : CONTINUATION |/////| CONTINUATION | DATA |///|
 *  `---^------------------------^-----^--------------^------^---'
 *  |   |                        <----->                     |   |
 * area |                          hole                      |  wrap
 *      |<----------------------->                           |
 *      |           dfl                                      |
 *      |<-------------------------------------------------->|
 *    head                    data
 *
 * Please note that the HEADERS frame is always deprived from its PADLEN byte
 * however it may start with the 5 stream-dep+weight bytes in case of PRIORITY
 * bit.
 *
 * The <flags> field must point to either the stream's flags or to a copy of it
 * so that the function can update the following flags :
 *   - H3_SF_DATA_CLEN when content-length is seen
 *   - H3_SF_HEADERS_RCVD once the frame is successfully decoded
 *
 * The H3_SF_HEADERS_RCVD flag is also looked at in the <flags> field prior to
 * decoding, in order to detect if we're dealing with a headers or a trailers
 * block (the trailers block appears after H3_SF_HEADERS_RCVD was seen).
 */
static int h3c_decode_headers(struct h3c *h3c, struct buffer *rxbuf, uint32_t *flags, unsigned long long *body_len)
{
	const uint8_t *hdrs = (uint8_t *)b_head(&h3c->dbuf);
	struct buffer *tmp = get_trash_chunk();
	struct http_hdr list[global.tune.max_http_hdr * 2];
	struct buffer *copy = NULL;
	unsigned int msgf;
	struct htx *htx = NULL;
	int flen; // header frame len
	int hole = 0;
	int ret = 0;
	int outlen;
	int wrap;

	TRACE_ENTER(H3_EV_RX_FRAME|H3_EV_RX_HDR, h3c->conn);

next_frame:
	if (b_data(&h3c->dbuf) - hole < h3c->dfl)
		goto leave; // incomplete input frame

	/* No END_HEADERS means there's one or more CONTINUATION frames. In
	 * this case, we'll try to paste it immediately after the initial
	 * HEADERS frame payload and kill any possible padding. The initial
	 * frame's length will be increased to represent the concatenation
	 * of the two frames. The next frame is read from position <tlen>
	 * and written at position <flen> (minus padding if some is present).
	 */
	if (unlikely(!(h3c->dff & H3_F_HEADERS_END_HEADERS))) {
		struct h3_fh hdr;
		int clen; // CONTINUATION frame's payload length

		TRACE_STATE("EH missing, expecting continuation frame", H3_EV_RX_FRAME|H3_EV_RX_FHDR|H3_EV_RX_HDR, h3c->conn);
		if (!h3_peek_frame_hdr(&h3c->dbuf, h3c->dfl + hole, &hdr)) {
			/* no more data, the buffer may be full, either due to
			 * too large a frame or because of too large a hole that
			 * we're going to compact at the end.
			 */
			goto leave;
		}

		if (hdr.ft != H3_FT_CONTINUATION) {
			/* RFC7540#6.10: frame of unexpected type */
			TRACE_STATE("not continuation!", H3_EV_RX_FRAME|H3_EV_RX_FHDR|H3_EV_RX_HDR|H3_EV_RX_CONT|H3_EV_H3C_ERR|H3_EV_PROTO_ERR, h3c->conn);
			h3c_error(h3c, H3_ERR_PROTOCOL_ERROR);
			goto fail;
		}

		if (hdr.sid != h3c->dsi) {
			/* RFC7540#6.10: frame of different stream */
			TRACE_STATE("different stream ID!", H3_EV_RX_FRAME|H3_EV_RX_FHDR|H3_EV_RX_HDR|H3_EV_RX_CONT|H3_EV_H3C_ERR|H3_EV_PROTO_ERR, h3c->conn);
			h3c_error(h3c, H3_ERR_PROTOCOL_ERROR);
			goto fail;
		}

		if ((unsigned)hdr.len > (unsigned)global.tune.bufsize) {
			/* RFC7540#4.2: invalid frame length */
			TRACE_STATE("too large frame!", H3_EV_RX_FRAME|H3_EV_RX_FHDR|H3_EV_RX_HDR|H3_EV_RX_CONT|H3_EV_H3C_ERR|H3_EV_PROTO_ERR, h3c->conn);
			h3c_error(h3c, H3_ERR_FRAME_SIZE_ERROR);
			goto fail;
		}

		/* detect when we must stop aggragating frames */
		h3c->dff |= hdr.ff & H3_F_HEADERS_END_HEADERS;

		/* Take as much as we can of the CONTINUATION frame's payload */
		clen = b_data(&h3c->dbuf) - (h3c->dfl + hole + 9);
		if (clen > hdr.len)
			clen = hdr.len;

		/* Move the frame's payload over the padding, hole and frame
		 * header. At least one of hole or dpl is null (see diagrams
		 * above). The hole moves after the new aggragated frame.
		 */
		b_move(&h3c->dbuf, b_peek_ofs(&h3c->dbuf, h3c->dfl + hole + 9), clen, -(h3c->dpl + hole + 9));
		h3c->dfl += clen - h3c->dpl;
		hole     += h3c->dpl + 9;
		h3c->dpl  = 0;
		TRACE_STATE("waiting for next continuation frame", H3_EV_RX_FRAME|H3_EV_RX_FHDR|H3_EV_RX_CONT|H3_EV_RX_HDR, h3c->conn);
		goto next_frame;
	}

	flen = h3c->dfl - h3c->dpl;

	/* if the input buffer wraps, take a temporary copy of it (rare) */
	wrap = b_wrap(&h3c->dbuf) - b_head(&h3c->dbuf);
	if (wrap < h3c->dfl) {
		copy = alloc_trash_chunk();
		if (!copy) {
			TRACE_DEVEL("failed to allocate temporary buffer", H3_EV_RX_FRAME|H3_EV_RX_HDR|H3_EV_H3C_ERR, h3c->conn);
			h3c_error(h3c, H3_ERR_INTERNAL_ERROR);
			goto fail;
		}
		memcpy(copy->area, b_head(&h3c->dbuf), wrap);
		memcpy(copy->area + wrap, b_orig(&h3c->dbuf), h3c->dfl - wrap);
		hdrs = (uint8_t *) copy->area;
	}

	/* Skip StreamDep and weight for now (we don't support PRIORITY) */
	if (h3c->dff & H3_F_HEADERS_PRIORITY) {
		if (read_n32(hdrs) == h3c->dsi) {
			/* RFC7540#5.3.1 : stream dep may not depend on itself */
			TRACE_STATE("invalid stream dependency!", H3_EV_RX_FRAME|H3_EV_RX_HDR|H3_EV_H3C_ERR|H3_EV_PROTO_ERR, h3c->conn);
			h3c_error(h3c, H3_ERR_PROTOCOL_ERROR);
			goto fail;
		}

		if (flen < 5) {
			TRACE_STATE("frame too short for priority!", H3_EV_RX_FRAME|H3_EV_RX_HDR|H3_EV_H3C_ERR|H3_EV_PROTO_ERR, h3c->conn);
			h3c_error(h3c, H3_ERR_FRAME_SIZE_ERROR);
			goto fail;
		}

		hdrs += 5; // stream dep = 4, weight = 1
		flen -= 5;
	}

	if (!h3_get_buf(h3c, rxbuf)) {
		TRACE_STATE("waiting for h3c rxbuf allocation", H3_EV_RX_FRAME|H3_EV_RX_HDR|H3_EV_H3C_BLK, h3c->conn);
		h3c->flags |= H3_CF_DEM_SALLOC;
		goto leave;
	}

	/* we can't retry a failed decompression operation so we must be very
	 * careful not to take any risks. In practice the output buffer is
	 * always empty except maybe for trailers, in which case we simply have
	 * to wait for the upper layer to finish consuming what is available.
	 */
	htx = htx_from_buf(rxbuf);
	if (!htx_is_empty(htx)) {
		TRACE_STATE("waiting for room in h3c rxbuf", H3_EV_RX_FRAME|H3_EV_RX_HDR|H3_EV_H3C_BLK, h3c->conn);
		h3c->flags |= H3_CF_DEM_SFULL;
		goto leave;
	}

	/* past this point we cannot roll back in case of error */
	outlen = hpack_decode_frame(h3c->ddht, hdrs, flen, list,
	                            sizeof(list)/sizeof(list[0]), tmp);
	if (outlen < 0) {
		TRACE_STATE("failed to decompress HPACK", H3_EV_RX_FRAME|H3_EV_RX_HDR|H3_EV_H3C_ERR|H3_EV_PROTO_ERR, h3c->conn);
		h3c_error(h3c, H3_ERR_COMPRESSION_ERROR);
		goto fail;
	}

	/* The PACK decompressor was updated, let's update the input buffer and
	 * the parser's state to commit these changes and allow us to later
	 * fail solely on the stream if needed.
	 */
	b_del(&h3c->dbuf, h3c->dfl + hole);
	h3c->dfl = hole = 0;
	h3c->st0 = H3_CS_FRAME_H;

	/* OK now we have our header list in <list> */
	msgf = (h3c->dff & H3_F_HEADERS_END_STREAM) ? 0 : H3_MSGF_BODY;

	if (*flags & H3_SF_HEADERS_RCVD)
		goto trailers;

	/* This is the first HEADERS frame so it's a headers block */
	if (h3c->flags & H3_CF_IS_BACK)
		outlen = h3_make_htx_response(list, htx, &msgf, body_len);
	else
		outlen = h3_make_htx_request(list, htx, &msgf, body_len);

	if (outlen < 0) {
		/* too large headers? this is a stream error only */
		TRACE_STATE("request headers too large", H3_EV_RX_FRAME|H3_EV_RX_HDR|H3_EV_H3S_ERR|H3_EV_PROTO_ERR, h3c->conn);
		goto fail;
	}

	if (msgf & H3_MSGF_BODY) {
		/* a payload is present */
		if (msgf & H3_MSGF_BODY_CL) {
			*flags |= H3_SF_DATA_CLEN;
			htx->extra = *body_len;
		}
	}

 done:
	/* indicate that a HEADERS frame was received for this stream, except
	 * for 1xx responses. For 1xx responses, another HEADERS frame is
	 * expected.
	 */
	if (!(msgf & H3_MSGF_RSP_1XX))
		*flags |= H3_SF_HEADERS_RCVD;

	if ((h3c->dff & H3_F_HEADERS_END_STREAM)) {
		/* Mark the end of message using EOM */
		if (!htx_add_endof(htx, HTX_BLK_EOM)) {
			TRACE_STATE("failed to append HTX EOM block into rxbuf", H3_EV_RX_FRAME|H3_EV_RX_HDR|H3_EV_H3S_ERR, h3c->conn);
			goto fail;
		}
	}

	/* success */
	ret = 1;

 leave:
	/* If there is a hole left and it's not at the end, we are forced to
	 * move the remaining data over it.
	 */
	if (hole) {
		if (b_data(&h3c->dbuf) > h3c->dfl + hole)
			b_move(&h3c->dbuf, b_peek_ofs(&h3c->dbuf, h3c->dfl + hole),
			       b_data(&h3c->dbuf) - (h3c->dfl + hole), -hole);
		b_sub(&h3c->dbuf, hole);
	}

	if (b_full(&h3c->dbuf) && h3c->dfl >= b_data(&h3c->dbuf)) {
		/* too large frames */
		h3c_error(h3c, H3_ERR_INTERNAL_ERROR);
		ret = -1;
	}

	if (htx)
		htx_to_buf(htx, rxbuf);
	free_trash_chunk(copy);
	TRACE_LEAVE(H3_EV_RX_FRAME|H3_EV_RX_HDR, h3c->conn);
	return ret;

 fail:
	ret = -1;
	goto leave;

 trailers:
	/* This is the last HEADERS frame hence a trailer */
	if (!(h3c->dff & H3_F_HEADERS_END_STREAM)) {
		/* It's a trailer but it's missing ES flag */
		TRACE_STATE("missing EH on trailers frame", H3_EV_RX_FRAME|H3_EV_RX_HDR|H3_EV_H3C_ERR|H3_EV_PROTO_ERR, h3c->conn);
		h3c_error(h3c, H3_ERR_PROTOCOL_ERROR);
		goto fail;
	}

	/* Trailers terminate a DATA sequence */
	if (h3_make_htx_trailers(list, htx) <= 0) {
		TRACE_STATE("failed to append HTX trailers into rxbuf", H3_EV_RX_FRAME|H3_EV_RX_HDR|H3_EV_H3S_ERR, h3c->conn);
		goto fail;
	}
	goto done;
}

/* Transfer the payload of a DATA frame to the HTTP/1 side. The HTTP/2 frame
 * parser state is automatically updated. Returns > 0 if it could completely
 * send the current frame, 0 if it couldn't complete, in which case
 * CS_FL_RCV_MORE must be checked to know if some data remain pending (an empty
 * DATA frame can return 0 as a valid result). Stream errors are reported in
 * h3s->errcode and connection errors in h3c->errcode. The caller must already
 * have checked the frame header and ensured that the frame was complete or the
 * buffer full. It changes the frame state to FRAME_A once done.
 */
static int h3_frt_transfer_data(struct h3s *h3s)
{
	struct h3c *h3c = h3s->h3c;
	int block;
	unsigned int flen = 0;
	struct htx *htx = NULL;
	struct buffer *csbuf;
	unsigned int sent;

	TRACE_ENTER(H3_EV_RX_FRAME|H3_EV_RX_DATA, h3c->conn, h3s);

	h3c->flags &= ~H3_CF_DEM_SFULL;

	csbuf = h3_get_buf(h3c, &h3s->rxbuf);
	if (!csbuf) {
		h3c->flags |= H3_CF_DEM_SALLOC;
		TRACE_STATE("waiting for an h3s rxbuf", H3_EV_RX_FRAME|H3_EV_RX_DATA|H3_EV_H3S_BLK, h3c->conn, h3s);
		goto fail;
	}
	htx = htx_from_buf(csbuf);

try_again:
	flen = h3c->dfl - h3c->dpl;
	if (!flen)
		goto end_transfer;

	if (flen > b_data(&h3c->dbuf)) {
		flen = b_data(&h3c->dbuf);
		if (!flen)
			goto fail;
	}

	block = htx_free_data_space(htx);
	if (!block) {
		h3c->flags |= H3_CF_DEM_SFULL;
		TRACE_STATE("h3s rxbuf is full", H3_EV_RX_FRAME|H3_EV_RX_DATA|H3_EV_H3S_BLK, h3c->conn, h3s);
		goto fail;
	}
	if (flen > block)
		flen = block;

	/* here, flen is the max we can copy into the output buffer */
	block = b_contig_data(&h3c->dbuf, 0);
	if (flen > block)
		flen = block;

	sent = htx_add_data(htx, ist2(b_head(&h3c->dbuf), flen));
	TRACE_DATA("move some data to h3s rxbuf", H3_EV_RX_FRAME|H3_EV_RX_DATA, h3c->conn, h3s,, (void *)(long)sent);

	b_del(&h3c->dbuf, sent);
	h3c->dfl    -= sent;
	h3c->rcvd_c += sent;
	h3c->rcvd_s += sent;  // warning, this can also affect the closed streams!

	if (h3s->flags & H3_SF_DATA_CLEN) {
		h3s->body_len -= sent;
		htx->extra = h3s->body_len;
	}

	if (sent < flen) {
		h3c->flags |= H3_CF_DEM_SFULL;
		TRACE_STATE("h3s rxbuf is full", H3_EV_RX_FRAME|H3_EV_RX_DATA|H3_EV_H3S_BLK, h3c->conn, h3s);
		goto fail;
	}

	goto try_again;

 end_transfer:
	/* here we're done with the frame, all the payload (except padding) was
	 * transferred.
	 */

	if (h3c->dff & H3_F_DATA_END_STREAM) {
		if (!htx_add_endof(htx, HTX_BLK_EOM)) {
			TRACE_STATE("h3s rxbuf is full, failed to add EOM", H3_EV_RX_FRAME|H3_EV_RX_DATA|H3_EV_H3S_BLK, h3c->conn, h3s);
			h3c->flags |= H3_CF_DEM_SFULL;
			goto fail;
		}
	}

	h3c->rcvd_c += h3c->dpl;
	h3c->rcvd_s += h3c->dpl;
	h3c->dpl = 0;
	h3c->st0 = H3_CS_FRAME_A; // send the corresponding window update
	htx_to_buf(htx, csbuf);
	TRACE_LEAVE(H3_EV_RX_FRAME|H3_EV_RX_DATA, h3c->conn, h3s);
	return 1;
 fail:
	if (htx)
		htx_to_buf(htx, csbuf);
	TRACE_LEAVE(H3_EV_RX_FRAME|H3_EV_RX_DATA, h3c->conn, h3s);
	return 0;
}

/* Try to send a HEADERS frame matching HTX response present in HTX message
 * <htx> for the H3 stream <h3s>. Returns the number of bytes sent. The caller
 * must check the stream's status to detect any error which might have happened
 * subsequently to a successful send. The htx blocks are automatically removed
 * from the message. The htx message is assumed to be valid since produced from
 * the internal code, hence it contains a start line, an optional series of
 * header blocks and an end of header, otherwise an invalid frame could be
 * emitted and the resulting htx message could be left in an inconsistent state.
 */
static size_t h3s_frt_make_resp_headers(struct h3s *h3s, struct htx *htx)
{
	struct http_hdr list[global.tune.max_http_hdr];
	struct h3c *h3c = h3s->h3c;
	struct htx_blk *blk;
	struct htx_blk *blk_end;
	struct buffer outbuf;
	struct buffer *mbuf;
	struct htx_sl *sl;
	enum htx_blk_type type;
	int es_now = 0;
	int ret = 0;
	int hdr;
	int idx;

	TRACE_ENTER(H3_EV_TX_FRAME|H3_EV_TX_HDR, h3c->conn, h3s);

	if (h3c_mux_busy(h3c, h3s)) {
		TRACE_STATE("mux output busy", H3_EV_TX_FRAME|H3_EV_TX_HDR, h3c->conn, h3s);
		h3s->flags |= H3_SF_BLK_MBUSY;
		TRACE_LEAVE(H3_EV_TX_FRAME|H3_EV_TX_HDR, h3c->conn, h3s);
		return 0;
	}

	/* determine the first block which must not be deleted, blk_end may
	 * be NULL if all blocks have to be deleted.
	 */
	idx = htx_get_head(htx);
	blk_end = NULL;
	while (idx != -1) {
		type = htx_get_blk_type(htx_get_blk(htx, idx));
		idx = htx_get_next(htx, idx);
		if (type == HTX_BLK_EOH) {
			if (idx != -1)
				blk_end = htx_get_blk(htx, idx);
			break;
		}
	}

	/* get the start line, we do have one */
	blk = htx_get_head_blk(htx);
	BUG_ON(!blk || htx_get_blk_type(blk) != HTX_BLK_RES_SL);
	ALREADY_CHECKED(blk);
	sl = htx_get_blk_ptr(htx, blk);
	h3s->status = sl->info.res.status;
	if (h3s->status < 100 || h3s->status > 999) {
		TRACE_PROTO("will not encode an invalid status code", H3_EV_TX_FRAME|H3_EV_TX_HDR|H3_EV_H3S_ERR, h3c->conn, h3s);
		goto fail;
	}

	/* and the rest of the headers, that we dump starting at header 0 */
	hdr = 0;

	idx = htx_get_head(htx); // returns the SL that we skip
	while ((idx = htx_get_next(htx, idx)) != -1) {
		blk = htx_get_blk(htx, idx);
		type = htx_get_blk_type(blk);

		if (type == HTX_BLK_UNUSED)
			continue;

		if (type != HTX_BLK_HDR)
			break;

		if (unlikely(hdr >= sizeof(list)/sizeof(list[0]) - 1)) {
			TRACE_PROTO("too many headers", H3_EV_TX_FRAME|H3_EV_TX_HDR|H3_EV_H3S_ERR, h3c->conn, h3s);
			goto fail;
		}

		list[hdr].n = htx_get_blk_name(htx, blk);
		list[hdr].v = htx_get_blk_value(htx, blk);
		hdr++;
	}

	/* marker for end of headers */
	list[hdr].n = ist("");

	if (h3s->status == 204 || h3s->status == 304) {
		/* no contents, claim c-len is present and set to zero */
		es_now = 1;
	}

	mbuf = br_tail(h3c->mbuf);
 retry:
	if (!h3_get_buf(h3c, mbuf)) {
		h3c->flags |= H3_CF_MUX_MALLOC;
		h3s->flags |= H3_SF_BLK_MROOM;
		TRACE_STATE("waiting for room in output buffer", H3_EV_TX_FRAME|H3_EV_TX_HDR|H3_EV_H3S_BLK, h3c->conn, h3s);
		return 0;
	}

	chunk_reset(&outbuf);

	while (1) {
		outbuf = b_make(b_tail(mbuf), b_contig_space(mbuf), 0, 0);
		if (outbuf.size >= 9 || !b_space_wraps(mbuf))
			break;
	realign_again:
		b_slow_realign(mbuf, trash.area, b_data(mbuf));
	}

	if (outbuf.size < 9)
		goto full;

	/* len: 0x000000 (fill later), type: 1(HEADERS), flags: ENDH=4 */
	memcpy(outbuf.area, "\x00\x00\x00\x01\x04", 5);
	write_n32(outbuf.area + 5, h3s->id); // 4 bytes
	outbuf.data = 9;

	/* encode status, which necessarily is the first one */
	if (!hpack_encode_int_status(&outbuf, h3s->status)) {
		if (b_space_wraps(mbuf))
			goto realign_again;
		goto full;
	}

	/* encode all headers, stop at empty name */
	for (hdr = 0; hdr < sizeof(list)/sizeof(list[0]); hdr++) {
		/* these ones do not exist in H3 and must be dropped. */
		if (isteq(list[hdr].n, ist("connection")) ||
		    isteq(list[hdr].n, ist("proxy-connection")) ||
		    isteq(list[hdr].n, ist("keep-alive")) ||
		    isteq(list[hdr].n, ist("upgrade")) ||
		    isteq(list[hdr].n, ist("transfer-encoding")))
			continue;

		/* Skip all pseudo-headers */
		if (*(list[hdr].n.ptr) == ':')
			continue;

		if (isteq(list[hdr].n, ist("")))
			break; // end

		if (!hpack_encode_header(&outbuf, list[hdr].n, list[hdr].v)) {
			/* output full */
			if (b_space_wraps(mbuf))
				goto realign_again;
			goto full;
		}
	}

	/* update the frame's size */
	h3_set_frame_size(outbuf.area, outbuf.data - 9);

	if (outbuf.data > h3c->mfs + 9) {
		if (!h3_fragment_headers(&outbuf, h3c->mfs)) {
			/* output full */
			if (b_space_wraps(mbuf))
				goto realign_again;
			goto full;
		}
	}

	/* we may need to add END_STREAM except for 1xx responses.
	 * FIXME: we should also set it when we know for sure that the
	 * content-length is zero as well as on 204/304
	 */
	if (blk_end && htx_get_blk_type(blk_end) == HTX_BLK_EOM &&
	    (h3s->status >= 200 || h3s->status == 101))
		es_now = 1;

	if (!h3s->cs || h3s->cs->flags & CS_FL_SHW)
		es_now = 1;

	if (es_now)
		outbuf.area[4] |= H3_F_HEADERS_END_STREAM;

	/* commit the H3 response */
	TRACE_USER("sent H3 response", H3_EV_TX_FRAME|H3_EV_TX_HDR, h3c->conn, h3s, htx);
	b_add(mbuf, outbuf.data);

	/* indicates the HEADERS frame was sent, except for 1xx responses. For
	 * 1xx responses, another HEADERS frame is expected.
	 */
	if (h3s->status >= 200 || h3s->status == 101)
		h3s->flags |= H3_SF_HEADERS_SENT;

	if (es_now) {
		h3s->flags |= H3_SF_ES_SENT;
		TRACE_PROTO("setting ES on HEADERS frame", H3_EV_TX_FRAME|H3_EV_TX_HDR, h3c->conn, h3s, htx);
		if (h3s->st == H3_SS_OPEN)
			h3s->st = H3_SS_HLOC;
		else
			h3s_close(h3s);
	}

	/* OK we could properly deliver the response */

	/* remove all header blocks including the EOH and compute the
	 * corresponding size.
	 *
	 * FIXME: We should remove everything when es_now is set.
	 */
	ret = 0;
	idx = htx_get_head(htx);
	blk = htx_get_blk(htx, idx);
	while (blk != blk_end) {
		ret += htx_get_blksz(blk);
		blk = htx_remove_blk(htx, blk);
	}

	if (blk_end && htx_get_blk_type(blk_end) == HTX_BLK_EOM) {
		ret += htx_get_blksz(blk_end);
		htx_remove_blk(htx, blk_end);
	}
 end:
	TRACE_LEAVE(H3_EV_TX_FRAME|H3_EV_TX_HDR, h3c->conn, h3s);
	return ret;
 full:
	if ((mbuf = br_tail_add(h3c->mbuf)) != NULL)
		goto retry;
	h3c->flags |= H3_CF_MUX_MFULL;
	h3s->flags |= H3_SF_BLK_MROOM;
	ret = 0;
	TRACE_STATE("mux buffer full", H3_EV_TX_FRAME|H3_EV_TX_HDR|H3_EV_H3S_BLK, h3c->conn, h3s);
	goto end;
 fail:
	/* unparsable HTX messages, too large ones to be produced in the local
	 * list etc go here (unrecoverable errors).
	 */
	h3s_error(h3s, H3_ERR_INTERNAL_ERROR);
	ret = 0;
	goto end;
}

/* Try to send a HEADERS frame matching HTX request present in HTX message
 * <htx> for the H3 stream <h3s>. Returns the number of bytes sent. The caller
 * must check the stream's status to detect any error which might have happened
 * subsequently to a successful send. The htx blocks are automatically removed
 * from the message. The htx message is assumed to be valid since produced from
 * the internal code, hence it contains a start line, an optional series of
 * header blocks and an end of header, otherwise an invalid frame could be
 * emitted and the resulting htx message could be left in an inconsistent state.
 */
static size_t h3s_bck_make_req_headers(struct h3s *h3s, struct htx *htx)
{
	struct http_hdr list[global.tune.max_http_hdr];
	struct h3c *h3c = h3s->h3c;
	struct htx_blk *blk;
	struct htx_blk *blk_end;
	struct buffer outbuf;
	struct buffer *mbuf;
	struct htx_sl *sl;
	struct ist meth, uri, auth;
	enum htx_blk_type type;
	int es_now = 0;
	int ret = 0;
	int hdr;
	int idx;

	TRACE_ENTER(H3_EV_TX_FRAME|H3_EV_TX_HDR, h3c->conn, h3s);

	if (h3c_mux_busy(h3c, h3s)) {
		TRACE_STATE("mux output busy", H3_EV_TX_FRAME|H3_EV_TX_HDR, h3c->conn, h3s);
		h3s->flags |= H3_SF_BLK_MBUSY;
		TRACE_LEAVE(H3_EV_TX_FRAME|H3_EV_TX_HDR, h3c->conn, h3s);
		return 0;
	}

	/* determine the first block which must not be deleted, blk_end may
	 * be NULL if all blocks have to be deleted.
	 */
	idx = htx_get_head(htx);
	blk_end = NULL;
	while (idx != -1) {
		type = htx_get_blk_type(htx_get_blk(htx, idx));
		idx = htx_get_next(htx, idx);
		if (type == HTX_BLK_EOH) {
			if (idx != -1)
				blk_end = htx_get_blk(htx, idx);
			break;
		}
	}

	/* get the start line, we do have one */
	blk = htx_get_head_blk(htx);
	BUG_ON(!blk || htx_get_blk_type(blk) != HTX_BLK_REQ_SL);
	ALREADY_CHECKED(blk);
	sl = htx_get_blk_ptr(htx, blk);
	meth = htx_sl_req_meth(sl);
	uri  = htx_sl_req_uri(sl);
	if (unlikely(uri.len == 0)) {
		TRACE_PROTO("no URI in HTX request", H3_EV_TX_FRAME|H3_EV_TX_HDR|H3_EV_H3S_ERR, h3c->conn, h3s);
		goto fail;
	}

	/* and the rest of the headers, that we dump starting at header 0 */
	hdr = 0;

	idx = htx_get_head(htx); // returns the SL that we skip
	while ((idx = htx_get_next(htx, idx)) != -1) {
		blk = htx_get_blk(htx, idx);
		type = htx_get_blk_type(blk);

		if (type == HTX_BLK_UNUSED)
			continue;

		if (type != HTX_BLK_HDR)
			break;

		if (unlikely(hdr >= sizeof(list)/sizeof(list[0]) - 1)) {
			TRACE_PROTO("too many headers", H3_EV_TX_FRAME|H3_EV_TX_HDR|H3_EV_H3S_ERR, h3c->conn, h3s);
			goto fail;
		}

		list[hdr].n = htx_get_blk_name(htx, blk);
		list[hdr].v = htx_get_blk_value(htx, blk);

		/* Skip header if same name is used to add the server name */
		if ((h3c->flags & H3_CF_IS_BACK) && h3c->proxy->server_id_hdr_name &&
		    isteq(list[hdr].n, ist2(h3c->proxy->server_id_hdr_name, h3c->proxy->server_id_hdr_len)))
			continue;

		hdr++;
	}

	/* Now add the server name to a header (if requested) */
	if ((h3c->flags & H3_CF_IS_BACK) && h3c->proxy->server_id_hdr_name) {
		struct server *srv = objt_server(h3c->conn->target);

		if (srv) {
			list[hdr].n = ist2(h3c->proxy->server_id_hdr_name, h3c->proxy->server_id_hdr_len);
			list[hdr].v = ist(srv->id);
			hdr++;
		}
	}

	/* marker for end of headers */
	list[hdr].n = ist("");

	mbuf = br_tail(h3c->mbuf);
 retry:
	if (!h3_get_buf(h3c, mbuf)) {
		h3c->flags |= H3_CF_MUX_MALLOC;
		h3s->flags |= H3_SF_BLK_MROOM;
		TRACE_STATE("waiting for room in output buffer", H3_EV_TX_FRAME|H3_EV_TX_HDR|H3_EV_H3S_BLK, h3c->conn, h3s);
		return 0;
	}

	chunk_reset(&outbuf);

	while (1) {
		outbuf = b_make(b_tail(mbuf), b_contig_space(mbuf), 0, 0);
		if (outbuf.size >= 9 || !b_space_wraps(mbuf))
			break;
	realign_again:
		b_slow_realign(mbuf, trash.area, b_data(mbuf));
	}

	if (outbuf.size < 9)
		goto full;

	/* len: 0x000000 (fill later), type: 1(HEADERS), flags: ENDH=4 */
	memcpy(outbuf.area, "\x00\x00\x00\x01\x04", 5);
	write_n32(outbuf.area + 5, h3s->id); // 4 bytes
	outbuf.data = 9;

	/* encode the method, which necessarily is the first one */
	if (!hpack_encode_method(&outbuf, sl->info.req.meth, meth)) {
		if (b_space_wraps(mbuf))
			goto realign_again;
		goto full;
	}

	auth = ist(NULL);

	/* RFC7540 #8.3: the CONNECT method must have :
	 *   - :authority set to the URI part (host:port)
	 *   - :method set to CONNECT
	 *   - :scheme and :path omitted
	 */
	if (unlikely(sl->info.req.meth == HTTP_METH_CONNECT)) {
		auth = uri;

		if (!hpack_encode_header(&outbuf, ist(":authority"), auth)) {
			/* output full */
			if (b_space_wraps(mbuf))
				goto realign_again;
			goto full;
		}
	} else {
		/* other methods need a :scheme. If an authority is known from
		 * the request line, it must be sent, otherwise only host is
		 * sent. Host is never sent as the authority.
		 */
		struct ist scheme = { };

		if (uri.ptr[0] != '/' && uri.ptr[0] != '*') {
			/* the URI seems to start with a scheme */
			int len = 1;

			while (len < uri.len && uri.ptr[len] != ':')
				len++;

			if (len + 2 < uri.len && uri.ptr[len + 1] == '/' && uri.ptr[len + 2] == '/') {
				/* make the uri start at the authority now */
				scheme.ptr = uri.ptr;
				scheme.len = len,
				uri.ptr += len + 3;
				uri.len -= len + 3;

				/* find the auth part of the URI */
				auth.ptr = uri.ptr;
				auth.len = 0;
				while (auth.len < uri.len && auth.ptr[auth.len] != '/')
					auth.len++;

				uri.ptr += auth.len;
				uri.len -= auth.len;
			}
		}

		if (!scheme.len) {
			/* no explicit scheme, we're using an origin-form URI,
			 * probably from an H1 request transcoded to H3 via an
			 * external layer, then received as H3 without authority.
			 * So we have to look up the scheme from the HTX flags.
			 * In such a case only http and https are possible, and
			 * https is the default (sent by browsers).
			 */
			if ((sl->flags & (HTX_SL_F_HAS_SCHM|HTX_SL_F_SCHM_HTTP)) == (HTX_SL_F_HAS_SCHM|HTX_SL_F_SCHM_HTTP))
				scheme = ist("http");
			else
				scheme = ist("https");
		}

		if (!hpack_encode_scheme(&outbuf, scheme)) {
			/* output full */
			if (b_space_wraps(mbuf))
				goto realign_again;
			goto full;
		}

		if (auth.len && !hpack_encode_header(&outbuf, ist(":authority"), auth)) {
			/* output full */
			if (b_space_wraps(mbuf))
				goto realign_again;
			goto full;
		}

		/* encode the path. RFC7540#8.1.2.3: if path is empty it must
		 * be sent as '/' or '*'.
		 */
		if (unlikely(!uri.len)) {
			if (sl->info.req.meth == HTTP_METH_OPTIONS)
				uri = ist("*");
			else
				uri = ist("/");
		}

		if (!hpack_encode_path(&outbuf, uri)) {
			/* output full */
			if (b_space_wraps(mbuf))
				goto realign_again;
			goto full;
		}
	}

	/* encode all headers, stop at empty name. Host is only sent if we
	 * do not provide an authority.
	 */
	for (hdr = 0; hdr < sizeof(list)/sizeof(list[0]); hdr++) {
		struct ist n = list[hdr].n;
		struct ist v = list[hdr].v;

		/* these ones do not exist in H3 and must be dropped. */
		if (isteq(n, ist("connection")) ||
		    (auth.len && isteq(n, ist("host"))) ||
		    isteq(n, ist("proxy-connection")) ||
		    isteq(n, ist("keep-alive")) ||
		    isteq(n, ist("upgrade")) ||
		    isteq(n, ist("transfer-encoding")))
			continue;

		if (isteq(n, ist("te"))) {
			/* "te" may only be sent with "trailers" if this value
			 * is present, otherwise it must be deleted.
			 */
			v = istist(v, ist("trailers"));
			if (!v.ptr || (v.len > 8 && v.ptr[8] != ','))
				continue;
			v = ist("trailers");
		}

		/* Skip all pseudo-headers */
		if (*(n.ptr) == ':')
			continue;

		if (isteq(n, ist("")))
			break; // end

		if (!hpack_encode_header(&outbuf, n, v)) {
			/* output full */
			if (b_space_wraps(mbuf))
				goto realign_again;
			goto full;
		}
	}

	/* update the frame's size */
	h3_set_frame_size(outbuf.area, outbuf.data - 9);

	if (outbuf.data > h3c->mfs + 9) {
		if (!h3_fragment_headers(&outbuf, h3c->mfs)) {
			/* output full */
			if (b_space_wraps(mbuf))
				goto realign_again;
			goto full;
		}
	}

	/* we may need to add END_STREAM if we have no body :
	 *  - request already closed, or :
	 *  - no transfer-encoding, and :
	 *  - no content-length or content-length:0
	 * Fixme: this doesn't take into account CONNECT requests.
	 */
	if (blk_end && htx_get_blk_type(blk_end) == HTX_BLK_EOM)
		es_now = 1;

	if (sl->flags & HTX_SL_F_BODYLESS)
		es_now = 1;

	if (!h3s->cs || h3s->cs->flags & CS_FL_SHW)
		es_now = 1;

	if (es_now)
		outbuf.area[4] |= H3_F_HEADERS_END_STREAM;

	/* commit the H3 response */
	TRACE_USER("sent H3 request", H3_EV_TX_FRAME|H3_EV_TX_HDR, h3c->conn, h3s, htx);
	b_add(mbuf, outbuf.data);
	h3s->flags |= H3_SF_HEADERS_SENT;
	h3s->st = H3_SS_OPEN;

	if (es_now) {
		TRACE_PROTO("setting ES on HEADERS frame", H3_EV_TX_FRAME|H3_EV_TX_HDR, h3c->conn, h3s, htx);
		// trim any possibly pending data (eg: inconsistent content-length)
		h3s->flags |= H3_SF_ES_SENT;
		h3s->st = H3_SS_HLOC;
	}

	/* remove all header blocks including the EOH and compute the
	 * corresponding size.
	 *
	 * FIXME: We should remove everything when es_now is set.
	 */
	ret = 0;
	idx = htx_get_head(htx);
	blk = htx_get_blk(htx, idx);
	while (blk != blk_end) {
		ret += htx_get_blksz(blk);
		blk = htx_remove_blk(htx, blk);
	}

	if (blk_end && htx_get_blk_type(blk_end) == HTX_BLK_EOM) {
		ret += htx_get_blksz(blk_end);
		htx_remove_blk(htx, blk_end);
	}

 end:
	return ret;
 full:
	if ((mbuf = br_tail_add(h3c->mbuf)) != NULL)
		goto retry;
	h3c->flags |= H3_CF_MUX_MFULL;
	h3s->flags |= H3_SF_BLK_MROOM;
	ret = 0;
	TRACE_STATE("mux buffer full", H3_EV_TX_FRAME|H3_EV_TX_HDR|H3_EV_H3S_BLK, h3c->conn, h3s);
	goto end;
 fail:
	/* unparsable HTX messages, too large ones to be produced in the local
	 * list etc go here (unrecoverable errors).
	 */
	h3s_error(h3s, H3_ERR_INTERNAL_ERROR);
	ret = 0;
	goto end;
}

/* Try to send a DATA frame matching HTTP response present in HTX structure
 * present in <buf>, for stream <h3s>. Returns the number of bytes sent. The
 * caller must check the stream's status to detect any error which might have
 * happened subsequently to a successful send. Returns the number of data bytes
 * consumed, or zero if nothing done. Note that EOM count for 1 byte.
 */
static size_t h3s_frt_make_resp_data(struct h3s *h3s, struct buffer *buf, size_t count)
{
	struct h3c *h3c = h3s->h3c;
	struct htx *htx;
	struct buffer outbuf;
	struct buffer *mbuf;
	size_t total = 0;
	int es_now = 0;
	int bsize; /* htx block size */
	int fsize; /* h3 frame size  */
	struct htx_blk *blk;
	enum htx_blk_type type;
	int idx;
	int trunc_out; /* non-zero if truncated on out buf */

	TRACE_ENTER(H3_EV_TX_FRAME|H3_EV_TX_DATA, h3c->conn, h3s);

	if (h3c_mux_busy(h3c, h3s)) {
		TRACE_STATE("mux output busy", H3_EV_TX_FRAME|H3_EV_TX_DATA, h3c->conn, h3s);
		h3s->flags |= H3_SF_BLK_MBUSY;
		TRACE_LEAVE(H3_EV_TX_FRAME|H3_EV_TX_DATA, h3c->conn, h3s);
		goto end;
	}

	htx = htx_from_buf(buf);

	/* We only come here with HTX_BLK_DATA blocks. However, while looping,
	 * we can meet an HTX_BLK_EOM block that we'll leave to the caller to
	 * handle.
	 */

 new_frame:
	if (!count || htx_is_empty(htx))
		goto end;

	idx   = htx_get_head(htx);
	blk   = htx_get_blk(htx, idx);
	type  = htx_get_blk_type(blk); // DATA or EOM
	bsize = htx_get_blksz(blk);
	fsize = bsize;
	trunc_out = 0;

	if (type == HTX_BLK_EOM) {
		if (h3s->flags & H3_SF_ES_SENT) {
			/* ES already sent */
			htx_remove_blk(htx, blk);
			total++; // EOM counts as one byte
			count--;
			goto end;
		}
	}
	else if (type != HTX_BLK_DATA)
		goto end;

	mbuf = br_tail(h3c->mbuf);
 retry:
	if (!h3_get_buf(h3c, mbuf)) {
		h3c->flags |= H3_CF_MUX_MALLOC;
		h3s->flags |= H3_SF_BLK_MROOM;
		TRACE_STATE("waiting for room in output buffer", H3_EV_TX_FRAME|H3_EV_TX_DATA|H3_EV_H3S_BLK, h3c->conn, h3s);
		goto end;
	}

	/* Perform some optimizations to reduce the number of buffer copies.
	 * First, if the mux's buffer is empty and the htx area contains
	 * exactly one data block of the same size as the requested count, and
	 * this count fits within the frame size, the stream's window size, and
	 * the connection's window size, then it's possible to simply swap the
	 * caller's buffer with the mux's output buffer and adjust offsets and
	 * length to match the entire DATA HTX block in the middle. In this
	 * case we perform a true zero-copy operation from end-to-end. This is
	 * the situation that happens all the time with large files. Second, if
	 * this is not possible, but the mux's output buffer is empty, we still
	 * have an opportunity to avoid the copy to the intermediary buffer, by
	 * making the intermediary buffer's area point to the output buffer's
	 * area. In this case we want to skip the HTX header to make sure that
	 * copies remain aligned and that this operation remains possible all
	 * the time. This goes for headers, data blocks and any data extracted
	 * from the HTX blocks.
	 */
	if (unlikely(fsize == count &&
	             htx_nbblks(htx) == 1 && type == HTX_BLK_DATA &&
	             fsize <= h3s_mws(h3s) && fsize <= h3c->mws && fsize <= h3c->mfs)) {
		void *old_area = mbuf->area;

		if (b_data(mbuf)) {
			/* Too bad there are data left there. We're willing to memcpy/memmove
			 * up to 1/4 of the buffer, which means that it's OK to copy a large
			 * frame into a buffer containing few data if it needs to be realigned,
			 * and that it's also OK to copy few data without realigning. Otherwise
			 * we'll pretend the mbuf is full and wait for it to become empty.
			 */
			if (fsize + 9 <= b_room(mbuf) &&
			    (b_data(mbuf) <= b_size(mbuf) / 4 ||
			     (fsize <= b_size(mbuf) / 4 && fsize + 9 <= b_contig_space(mbuf)))) {
				TRACE_STATE("small data present in output buffer, appending", H3_EV_TX_FRAME|H3_EV_TX_DATA, h3c->conn, h3s);
				goto copy;
			}

			if ((mbuf = br_tail_add(h3c->mbuf)) != NULL)
				goto retry;

			h3c->flags |= H3_CF_MUX_MFULL;
			h3s->flags |= H3_SF_BLK_MROOM;
			TRACE_STATE("too large data present in output buffer, waiting for emptiness", H3_EV_TX_FRAME|H3_EV_TX_DATA, h3c->conn, h3s);
			goto end;
		}

		/* map an H3 frame to the HTX block so that we can put the
		 * frame header there.
		 */
		*mbuf = b_make(buf->area, buf->size, sizeof(struct htx) + blk->addr - 9, fsize + 9);
		outbuf.area    = b_head(mbuf);

		/* prepend an H3 DATA frame header just before the DATA block */
		memcpy(outbuf.area, "\x00\x00\x00\x00\x00", 5);
		write_n32(outbuf.area + 5, h3s->id); // 4 bytes
		h3_set_frame_size(outbuf.area, fsize);

		/* update windows */
		h3s->sws -= fsize;
		h3c->mws -= fsize;

		/* and exchange with our old area */
		buf->area = old_area;
		buf->data = buf->head = 0;
		total += fsize;

		TRACE_PROTO("sent H3 DATA frame (zero-copy)", H3_EV_TX_FRAME|H3_EV_TX_DATA, h3c->conn, h3s);
		goto end;
	}

 copy:
	/* for DATA and EOM we'll have to emit a frame, even if empty */

	while (1) {
		outbuf = b_make(b_tail(mbuf), b_contig_space(mbuf), 0, 0);
		if (outbuf.size >= 9 || !b_space_wraps(mbuf))
			break;
	realign_again:
		b_slow_realign(mbuf, trash.area, b_data(mbuf));
	}

	if (outbuf.size < 9) {
		if ((mbuf = br_tail_add(h3c->mbuf)) != NULL)
			goto retry;
		h3c->flags |= H3_CF_MUX_MFULL;
		h3s->flags |= H3_SF_BLK_MROOM;
		TRACE_STATE("output buffer full", H3_EV_TX_FRAME|H3_EV_TX_DATA, h3c->conn, h3s);
		goto end;
	}

	/* len: 0x000000 (fill later), type: 0(DATA), flags: none=0 */
	memcpy(outbuf.area, "\x00\x00\x00\x00\x00", 5);
	write_n32(outbuf.area + 5, h3s->id); // 4 bytes
	outbuf.data = 9;

	/* we have in <fsize> the exact number of bytes we need to copy from
	 * the HTX buffer. We need to check this against the connection's and
	 * the stream's send windows, and to ensure that this fits in the max
	 * frame size and in the buffer's available space minus 9 bytes (for
	 * the frame header). The connection's flow control is applied last so
	 * that we can use a separate list of streams which are immediately
	 * unblocked on window opening. Note: we don't implement padding.
	 */

	/* EOM is presented with bsize==1 but would lead to the emission of an
	 * empty frame, thus we force it to zero here.
	 */
	if (type == HTX_BLK_EOM)
		bsize = fsize = 0;

	if (!fsize)
		goto send_empty;

	if (h3s_mws(h3s) <= 0) {
		h3s->flags |= H3_SF_BLK_SFCTL;
		if (LIST_ADDED(&h3s->list))
			LIST_DEL_INIT(&h3s->list);
		LIST_ADDQ(&h3c->blocked_list, &h3s->list);
		TRACE_STATE("stream window <=0, flow-controlled", H3_EV_TX_FRAME|H3_EV_TX_DATA|H3_EV_H3S_FCTL, h3c->conn, h3s);
		goto end;
	}

	if (fsize > count)
		fsize = count;

	if (fsize > h3s_mws(h3s))
		fsize = h3s_mws(h3s); // >0

	if (h3c->mfs && fsize > h3c->mfs)
		fsize = h3c->mfs; // >0

	if (fsize + 9 > outbuf.size) {
		/* It doesn't fit at once. If it at least fits once split and
		 * the amount of data to move is low, let's defragment the
		 * buffer now.
		 */
		if (b_space_wraps(mbuf) &&
		    (fsize + 9 <= b_room(mbuf)) &&
		    b_data(mbuf) <= MAX_DATA_REALIGN)
			goto realign_again;
		fsize = outbuf.size - 9;
		trunc_out = 1;

		if (fsize <= 0) {
			/* no need to send an empty frame here */
			if ((mbuf = br_tail_add(h3c->mbuf)) != NULL)
				goto retry;
			h3c->flags |= H3_CF_MUX_MFULL;
			h3s->flags |= H3_SF_BLK_MROOM;
			TRACE_STATE("output buffer full", H3_EV_TX_FRAME|H3_EV_TX_DATA, h3c->conn, h3s);
			goto end;
		}
	}

	if (h3c->mws <= 0) {
		h3s->flags |= H3_SF_BLK_MFCTL;
		TRACE_STATE("connection window <=0, stream flow-controlled", H3_EV_TX_FRAME|H3_EV_TX_DATA|H3_EV_H3C_FCTL, h3c->conn, h3s);
		goto end;
	}

	if (fsize > h3c->mws)
		fsize = h3c->mws;

	/* now let's copy this this into the output buffer */
	memcpy(outbuf.area + 9, htx_get_blk_ptr(htx, blk), fsize);
	h3s->sws -= fsize;
	h3c->mws -= fsize;
	count    -= fsize;

 send_empty:
	/* update the frame's size */
	h3_set_frame_size(outbuf.area, fsize);

	/* FIXME: for now we only set the ES flag on empty DATA frames, once
	 * meeting EOM. We should optimize this later.
	 */
	if (type == HTX_BLK_EOM) {
		total++; // EOM counts as one byte
		count--;
		es_now = 1;
	}

	if (es_now)
		outbuf.area[4] |= H3_F_DATA_END_STREAM;

	/* commit the H3 response */
	b_add(mbuf, fsize + 9);

	/* consume incoming HTX block, including EOM */
	total += fsize;
	if (fsize == bsize) {
		htx_remove_blk(htx, blk);
		if (fsize) {
			TRACE_DEVEL("more data available, trying to send another frame", H3_EV_TX_FRAME|H3_EV_TX_DATA, h3c->conn, h3s);
			goto new_frame;
		}
	} else {
		/* we've truncated this block */
		htx_cut_data_blk(htx, blk, fsize);
		if (trunc_out)
			goto new_frame;
	}

	if (es_now) {
		if (h3s->st == H3_SS_OPEN)
			h3s->st = H3_SS_HLOC;
		else
			h3s_close(h3s);

		h3s->flags |= H3_SF_ES_SENT;
		TRACE_PROTO("ES flag set on outgoing frame", H3_EV_TX_FRAME|H3_EV_TX_DATA|H3_EV_TX_EOI, h3c->conn, h3s);
	}

 end:
	TRACE_LEAVE(H3_EV_TX_FRAME|H3_EV_TX_DATA, h3c->conn, h3s);
	return total;
}

/* Try to send a HEADERS frame matching HTX_BLK_TLR series of blocks present in
 * HTX message <htx> for the H3 stream <h3s>. Returns the number of bytes
 * processed. The caller must check the stream's status to detect any error
 * which might have happened subsequently to a successful send. The htx blocks
 * are automatically removed from the message. The htx message is assumed to be
 * valid since produced from the internal code. Processing stops when meeting
 * the EOM, which is *not* removed. All trailers are processed at once and sent
 * as a single frame. The ES flag is always set.
 */
static size_t h3s_make_trailers(struct h3s *h3s, struct htx *htx)
{
	struct http_hdr list[global.tune.max_http_hdr];
	struct h3c *h3c = h3s->h3c;
	struct htx_blk *blk;
	struct htx_blk *blk_end;
	struct buffer outbuf;
	struct buffer *mbuf;
	enum htx_blk_type type;
	int ret = 0;
	int hdr;
	int idx;

	TRACE_ENTER(H3_EV_TX_FRAME|H3_EV_TX_HDR, h3c->conn, h3s);

	if (h3c_mux_busy(h3c, h3s)) {
		TRACE_STATE("mux output busy", H3_EV_TX_FRAME|H3_EV_TX_HDR, h3c->conn, h3s);
		h3s->flags |= H3_SF_BLK_MBUSY;
		TRACE_LEAVE(H3_EV_TX_FRAME|H3_EV_TX_HDR, h3c->conn, h3s);
		goto end;
	}

	/* determine the first block which must not be deleted, blk_end may
	 * be NULL if all blocks have to be deleted. also get trailers.
         */
	idx = htx_get_head(htx);
	blk_end = NULL;

	hdr = 0;
	while (idx != -1) {
		blk = htx_get_blk(htx, idx);
		type = htx_get_blk_type(blk);
		idx = htx_get_next(htx, idx);
		if (type == HTX_BLK_UNUSED)
			continue;

		if (type == HTX_BLK_EOT) {
			if (idx != -1)
				blk_end = blk;
			break;
		}
		if (type != HTX_BLK_TLR)
			break;

		if (unlikely(hdr >= sizeof(list)/sizeof(list[0]) - 1)) {
			TRACE_PROTO("too many headers", H3_EV_TX_FRAME|H3_EV_TX_HDR|H3_EV_H3S_ERR, h3c->conn, h3s);
			goto fail;
		}

		list[hdr].n = htx_get_blk_name(htx, blk);
		list[hdr].v = htx_get_blk_value(htx, blk);
		hdr++;
	}

	/* marker for end of trailers */
	list[hdr].n = ist("");

	mbuf = br_tail(h3c->mbuf);
 retry:
	if (!h3_get_buf(h3c, mbuf)) {
		h3c->flags |= H3_CF_MUX_MALLOC;
		h3s->flags |= H3_SF_BLK_MROOM;
		TRACE_STATE("waiting for room in output buffer", H3_EV_TX_FRAME|H3_EV_TX_HDR|H3_EV_H3S_BLK, h3c->conn, h3s);
		goto end;
	}

	chunk_reset(&outbuf);

	while (1) {
		outbuf = b_make(b_tail(mbuf), b_contig_space(mbuf), 0, 0);
		if (outbuf.size >= 9 || !b_space_wraps(mbuf))
			break;
	realign_again:
		b_slow_realign(mbuf, trash.area, b_data(mbuf));
	}

	if (outbuf.size < 9)
		goto full;

	/* len: 0x000000 (fill later), type: 1(HEADERS), flags: ENDH=4,ES=1 */
	memcpy(outbuf.area, "\x00\x00\x00\x01\x05", 5);
	write_n32(outbuf.area + 5, h3s->id); // 4 bytes
	outbuf.data = 9;

	/* encode all headers */
	for (idx = 0; idx < hdr; idx++) {
		/* these ones do not exist in H3 or must not appear in
		 * trailers and must be dropped.
		 */
		if (isteq(list[idx].n, ist("host")) ||
		    isteq(list[idx].n, ist("content-length")) ||
		    isteq(list[idx].n, ist("connection")) ||
		    isteq(list[idx].n, ist("proxy-connection")) ||
		    isteq(list[idx].n, ist("keep-alive")) ||
		    isteq(list[idx].n, ist("upgrade")) ||
		    isteq(list[idx].n, ist("te")) ||
		    isteq(list[idx].n, ist("transfer-encoding")))
			continue;

		/* Skip all pseudo-headers */
		if (*(list[idx].n.ptr) == ':')
			continue;

		if (!hpack_encode_header(&outbuf, list[idx].n, list[idx].v)) {
			/* output full */
			if (b_space_wraps(mbuf))
				goto realign_again;
			goto full;
		}
	}

	if (outbuf.data == 9) {
		/* here we have a problem, we have nothing to emit (either we
		 * received an empty trailers block followed or we removed its
		 * contents above). Because of this we can't send a HEADERS
		 * frame, so we have to cheat and instead send an empty DATA
		 * frame conveying the ES flag.
		 */
		outbuf.area[3] = H3_FT_DATA;
		outbuf.area[4] = H3_F_DATA_END_STREAM;
	}

	/* update the frame's size */
	h3_set_frame_size(outbuf.area, outbuf.data - 9);

	if (outbuf.data > h3c->mfs + 9) {
		if (!h3_fragment_headers(&outbuf, h3c->mfs)) {
			/* output full */
			if (b_space_wraps(mbuf))
				goto realign_again;
			goto full;
		}
	}

	/* commit the H3 response */
	TRACE_PROTO("sent H3 trailers HEADERS frame", H3_EV_TX_FRAME|H3_EV_TX_HDR|H3_EV_TX_EOI, h3c->conn, h3s);
	b_add(mbuf, outbuf.data);
	h3s->flags |= H3_SF_ES_SENT;

	if (h3s->st == H3_SS_OPEN)
		h3s->st = H3_SS_HLOC;
	else
		h3s_close(h3s);

	/* OK we could properly deliver the response */
 done:
	/* remove all header blocks till the end and compute the corresponding size. */
	ret = 0;
	idx = htx_get_head(htx);
	blk = htx_get_blk(htx, idx);
	while (blk != blk_end) {
		ret += htx_get_blksz(blk);
		blk = htx_remove_blk(htx, blk);
	}

	if (blk_end && htx_get_blk_type(blk_end) == HTX_BLK_EOM) {
		ret += htx_get_blksz(blk_end);
		htx_remove_blk(htx, blk_end);
	}

 end:
	TRACE_LEAVE(H3_EV_TX_FRAME|H3_EV_TX_HDR, h3c->conn, h3s);
	return ret;
 full:
	if ((mbuf = br_tail_add(h3c->mbuf)) != NULL)
		goto retry;
	h3c->flags |= H3_CF_MUX_MFULL;
	h3s->flags |= H3_SF_BLK_MROOM;
	ret = 0;
	TRACE_STATE("mux buffer full", H3_EV_TX_FRAME|H3_EV_TX_HDR|H3_EV_H3S_BLK, h3c->conn, h3s);
	goto end;
 fail:
	/* unparsable HTX messages, too large ones to be produced in the local
	 * list etc go here (unrecoverable errors).
	 */
	h3s_error(h3s, H3_ERR_INTERNAL_ERROR);
	ret = 0;
	goto end;
}

/* Called from the upper layer, to subscribe <es> to events <event_type>. The
 * event subscriber <es> is not allowed to change from a previous call as long
 * as at least one event is still subscribed. The <event_type> must only be a
 * combination of SUB_RETRY_RECV and SUB_RETRY_SEND. It always returns 0.
 */
static int h3_subscribe(struct conn_stream *cs, int event_type, struct wait_event *es)
{
	struct h3s *h3s = cs->ctx;
	struct h3c *h3c = h3s->h3c;

	TRACE_ENTER(H3_EV_STRM_SEND|H3_EV_STRM_RECV, h3c->conn, h3s);

	BUG_ON(event_type & ~(SUB_RETRY_SEND|SUB_RETRY_RECV));
	BUG_ON(h3s->subs && h3s->subs != es);

	es->events |= event_type;
	h3s->subs = es;

	if (event_type & SUB_RETRY_RECV)
		TRACE_DEVEL("subscribe(recv)", H3_EV_STRM_RECV, h3c->conn, h3s);

	if (event_type & SUB_RETRY_SEND) {
		TRACE_DEVEL("subscribe(send)", H3_EV_STRM_SEND, h3c->conn, h3s);
		if (!(h3s->flags & H3_SF_BLK_SFCTL) &&
		    !LIST_ADDED(&h3s->list)) {
			if (h3s->flags & H3_SF_BLK_MFCTL)
				LIST_ADDQ(&h3c->fctl_list, &h3s->list);
			else
				LIST_ADDQ(&h3c->send_list, &h3s->list);
		}
	}
	TRACE_LEAVE(H3_EV_STRM_SEND|H3_EV_STRM_RECV, h3c->conn, h3s);
	return 0;
}

/* Called from the upper layer, to unsubscribe <es> from events <event_type>.
 * The <es> pointer is not allowed to differ from the one passed to the
 * subscribe() call. It always returns zero.
 */
static int h3_unsubscribe(struct conn_stream *cs, int event_type, struct wait_event *es)
{
	struct h3s *h3s = cs->ctx;

	TRACE_ENTER(H3_EV_STRM_SEND|H3_EV_STRM_RECV, h3s->h3c->conn, h3s);

	BUG_ON(event_type & ~(SUB_RETRY_SEND|SUB_RETRY_RECV));
	BUG_ON(h3s->subs && h3s->subs != es);

	es->events &= ~event_type;
	if (!es->events)
		h3s->subs = NULL;

	if (event_type & SUB_RETRY_RECV)
		TRACE_DEVEL("unsubscribe(recv)", H3_EV_STRM_RECV, h3s->h3c->conn, h3s);

	if (event_type & SUB_RETRY_SEND) {
		TRACE_DEVEL("subscribe(send)", H3_EV_STRM_SEND, h3s->h3c->conn, h3s);
		h3s->flags &= ~H3_SF_NOTIFIED;
		if (!(h3s->flags & (H3_SF_WANT_SHUTR | H3_SF_WANT_SHUTW)))
			LIST_DEL_INIT(&h3s->list);
	}

	TRACE_LEAVE(H3_EV_STRM_SEND|H3_EV_STRM_RECV, h3s->h3c->conn, h3s);
	return 0;
}


/* Called from the upper layer, to receive data */
static size_t h3_rcv_buf(struct conn_stream *cs, struct buffer *buf, size_t count, int flags)
{
	struct h3s *h3s = cs->ctx;
	struct h3c *h3c = h3s->h3c;
	struct htx *h3s_htx = NULL;
	struct htx *buf_htx = NULL;
	size_t ret = 0;

	TRACE_ENTER(H3_EV_STRM_RECV, h3c->conn, h3s);

	/* transfer possibly pending data to the upper layer */
	h3s_htx = htx_from_buf(&h3s->rxbuf);
	if (htx_is_empty(h3s_htx)) {
		/* Here htx_to_buf() will set buffer data to 0 because
		 * the HTX is empty.
		 */
		htx_to_buf(h3s_htx, &h3s->rxbuf);
		goto end;
	}

	ret = h3s_htx->data;
	buf_htx = htx_from_buf(buf);

	/* <buf> is empty and the message is small enough, swap the
	 * buffers. */
	if (htx_is_empty(buf_htx) && htx_used_space(h3s_htx) <= count) {
		htx_to_buf(buf_htx, buf);
		htx_to_buf(h3s_htx, &h3s->rxbuf);
		b_xfer(buf, &h3s->rxbuf, b_data(&h3s->rxbuf));
		goto end;
	}

	htx_xfer_blks(buf_htx, h3s_htx, count, HTX_BLK_EOM);

	if (h3s_htx->flags & HTX_FL_PARSING_ERROR) {
		buf_htx->flags |= HTX_FL_PARSING_ERROR;
		if (htx_is_empty(buf_htx))
			cs->flags |= CS_FL_EOI;
	}

	buf_htx->extra = (h3s_htx->extra ? (h3s_htx->data + h3s_htx->extra) : 0);
	htx_to_buf(buf_htx, buf);
	htx_to_buf(h3s_htx, &h3s->rxbuf);
	ret -= h3s_htx->data;

  end:
	if (b_data(&h3s->rxbuf))
		cs->flags |= (CS_FL_RCV_MORE | CS_FL_WANT_ROOM);
	else {
		cs->flags &= ~(CS_FL_RCV_MORE | CS_FL_WANT_ROOM);
		if (h3s->flags & H3_SF_ES_RCVD)
			cs->flags |= CS_FL_EOI;
		if (conn_xprt_read0_pending(h3c->conn) || h3s->st == H3_SS_CLOSED)
			cs->flags |= CS_FL_EOS;
		if (cs->flags & CS_FL_ERR_PENDING)
			cs->flags |= CS_FL_ERROR;
		if (b_size(&h3s->rxbuf)) {
			b_free(&h3s->rxbuf);
			offer_buffers(NULL, tasks_run_queue);
		}
	}

	if (ret && h3c->dsi == h3s->id) {
		/* demux is blocking on this stream's buffer */
		h3c->flags &= ~H3_CF_DEM_SFULL;
		h3c_restart_reading(h3c, 1);
	}

	TRACE_LEAVE(H3_EV_STRM_RECV, h3c->conn, h3s);
	return ret;
}


/* Called from the upper layer, to send data from buffer <buf> for no more than
 * <count> bytes. Returns the number of bytes effectively sent. Some status
 * flags may be updated on the conn_stream.
 */
static size_t h3_snd_buf(struct conn_stream *cs, struct buffer *buf, size_t count, int flags)
{
	struct h3s *h3s = cs->ctx;
	size_t total = 0;
	size_t ret;
	struct htx *htx;
	struct htx_blk *blk;
	enum htx_blk_type btype;
	uint32_t bsize;
	int32_t idx;

	TRACE_ENTER(H3_EV_H3S_SEND|H3_EV_STRM_SEND, h3s->h3c->conn, h3s);

	/* If we were not just woken because we wanted to send but couldn't,
	 * and there's somebody else that is waiting to send, do nothing,
	 * we will subscribe later and be put at the end of the list
	 */
	if (!(h3s->flags & H3_SF_NOTIFIED) &&
	    (!LIST_ISEMPTY(&h3s->h3c->send_list) || !LIST_ISEMPTY(&h3s->h3c->fctl_list))) {
		TRACE_DEVEL("other streams already waiting, going to the queue and leaving", H3_EV_H3S_SEND|H3_EV_H3S_BLK, h3s->h3c->conn, h3s);
		return 0;
	}
	h3s->flags &= ~H3_SF_NOTIFIED;

	if (h3s->h3c->st0 < H3_CS_FRAME_H) {
		TRACE_DEVEL("connection not ready, leaving", H3_EV_H3S_SEND|H3_EV_H3S_BLK, h3s->h3c->conn, h3s);
		return 0;
	}

	if (h3s->h3c->st0 >= H3_CS_ERROR) {
		cs->flags |= CS_FL_ERROR;
		TRACE_DEVEL("connection is in error, leaving in error", H3_EV_H3S_SEND|H3_EV_H3S_BLK|H3_EV_H3S_ERR|H3_EV_STRM_ERR, h3s->h3c->conn, h3s);
		return 0;
	}

	htx = htx_from_buf(buf);

	if (!(h3s->flags & H3_SF_OUTGOING_DATA) && count)
		h3s->flags |= H3_SF_OUTGOING_DATA;

	if (h3s->id == 0) {
		int32_t id = h3c_get_next_sid(h3s->h3c);

		if (id < 0) {
			cs->flags |= CS_FL_ERROR;
			TRACE_DEVEL("couldn't get a stream ID, leaving in error", H3_EV_H3S_SEND|H3_EV_H3S_BLK|H3_EV_H3S_ERR|H3_EV_STRM_ERR, h3s->h3c->conn, h3s);
			return 0;
		}

		eb32_delete(&h3s->by_id);
		h3s->by_id.key = h3s->id = id;
		h3s->h3c->max_id = id;
		h3s->h3c->nb_reserved--;
		eb32_insert(&h3s->h3c->streams_by_id, &h3s->by_id);
	}

	while (h3s->st < H3_SS_HLOC && !(h3s->flags & H3_SF_BLK_ANY) &&
	       count && !htx_is_empty(htx)) {
		idx   = htx_get_head(htx);
		blk   = htx_get_blk(htx, idx);
		btype = htx_get_blk_type(blk);
		bsize = htx_get_blksz(blk);

		switch (btype) {
			case HTX_BLK_REQ_SL:
				/* start-line before headers */
				ret = h3s_bck_make_req_headers(h3s, htx);
				if (ret > 0) {
					total += ret;
					count -= ret;
					if (ret < bsize)
						goto done;
				}
				break;

			case HTX_BLK_RES_SL:
				/* start-line before headers */
				ret = h3s_frt_make_resp_headers(h3s, htx);
				if (ret > 0) {
					total += ret;
					count -= ret;
					if (ret < bsize)
						goto done;
				}
				break;

			case HTX_BLK_DATA:
			case HTX_BLK_EOM:
				/* all these cause the emission of a DATA frame (possibly empty).
				 * This EOM necessarily is one before trailers, as the EOM following
				 * trailers would have been consumed by the trailers parser.
				 */
				ret = h3s_frt_make_resp_data(h3s, buf, count);
				if (ret > 0) {
					htx = htx_from_buf(buf);
					total += ret;
					count -= ret;
					if (ret < bsize)
						goto done;
				}
				break;

			case HTX_BLK_TLR:
			case HTX_BLK_EOT:
				/* This is the first trailers block, all the subsequent ones AND
				 * the EOM will be swallowed by the parser.
				 */
				ret = h3s_make_trailers(h3s, htx);
				if (ret > 0) {
					total += ret;
					count -= ret;
					if (ret < bsize)
						goto done;
				}
				break;

			default:
				htx_remove_blk(htx, blk);
				total += bsize;
				count -= bsize;
				break;
		}
	}

  done:
	if (h3s->st >= H3_SS_HLOC) {
		/* trim any possibly pending data after we close (extra CR-LF,
		 * unprocessed trailers, abnormal extra data, ...)
		 */
		total += count;
		count = 0;
	}

	/* RST are sent similarly to frame acks */
	if (h3s->st == H3_SS_ERROR || h3s->flags & H3_SF_RST_RCVD) {
		TRACE_DEVEL("reporting RST/error to the app-layer stream", H3_EV_H3S_SEND|H3_EV_H3S_ERR|H3_EV_STRM_ERR, h3s->h3c->conn, h3s);
		cs_set_error(cs);
		if (h3s_send_rst_stream(h3s->h3c, h3s) > 0)
			h3s_close(h3s);
	}

	htx_to_buf(htx, buf);

	if (total > 0) {
		if (!(h3s->h3c->wait_event.events & SUB_RETRY_SEND))
			TRACE_DEVEL("data queued, waking up h3c sender", H3_EV_H3S_SEND|H3_EV_H3C_SEND, h3s->h3c->conn, h3s);
			//tasklet_wakeup(h3s->h3c->wait_event.tasklet);

	}
	/* If we're waiting for flow control, and we got a shutr on the
	 * connection, we will never be unlocked, so add an error on
	 * the conn_stream.
	 */
	if (conn_xprt_read0_pending(h3s->h3c->conn) &&
	    !b_data(&h3s->h3c->dbuf) &&
	    (h3s->flags & (H3_SF_BLK_SFCTL | H3_SF_BLK_MFCTL))) {
		TRACE_DEVEL("fctl with shutr, reporting error to app-layer", H3_EV_H3S_SEND|H3_EV_STRM_SEND|H3_EV_STRM_ERR, h3s->h3c->conn, h3s);
		if (cs->flags & CS_FL_EOS)
			cs->flags |= CS_FL_ERROR;
		else
			cs->flags |= CS_FL_ERR_PENDING;
	}

	if (total > 0 && !(h3s->flags & H3_SF_BLK_SFCTL) &&
	    !(h3s->flags & (H3_SF_WANT_SHUTR|H3_SF_WANT_SHUTW))) {
		/* Ok we managed to send something, leave the send_list if we were still there */
		LIST_DEL_INIT(&h3s->list);
	}

	TRACE_LEAVE(H3_EV_H3S_SEND|H3_EV_STRM_SEND, h3s->h3c->conn, h3s);
	return total;
}

/* for debugging with CLI's "show fd" command */
static void h3_show_fd(struct buffer *msg, struct connection *conn)
{
	struct h3c *h3c = conn->ctx;
	struct h3s *h3s = NULL;
	struct eb32_node *node;
	int fctl_cnt = 0;
	int send_cnt = 0;
	int tree_cnt = 0;
	int orph_cnt = 0;
	struct buffer *hmbuf, *tmbuf;

	if (!h3c)
		return;

	list_for_each_entry(h3s, &h3c->fctl_list, list)
		fctl_cnt++;

	list_for_each_entry(h3s, &h3c->send_list, list)
		send_cnt++;

	h3s = NULL;
	node = eb32_first(&h3c->streams_by_id);
	while (node) {
		h3s = container_of(node, struct h3s, by_id);
		tree_cnt++;
		if (!h3s->cs)
			orph_cnt++;
		node = eb32_next(node);
	}

	hmbuf = br_head(h3c->mbuf);
	tmbuf = br_tail(h3c->mbuf);
	chunk_appendf(msg, " h3c.st0=%s .err=%d .maxid=%d .lastid=%d .flg=0x%04x"
		      " .nbst=%u .nbcs=%u .fctl_cnt=%d .send_cnt=%d .tree_cnt=%d"
		      " .orph_cnt=%d .sub=%d .dsi=%d .dbuf=%u@%p+%u/%u .msi=%d"
		      " .mbuf=[%u..%u|%u],h=[%u@%p+%u/%u],t=[%u@%p+%u/%u]",
		      h3c_st_to_str(h3c->st0), h3c->errcode, h3c->max_id, h3c->last_sid, h3c->flags,
		      h3c->nb_streams, h3c->nb_cs, fctl_cnt, send_cnt, tree_cnt, orph_cnt,
		      h3c->wait_event.events, h3c->dsi,
		      (unsigned int)b_data(&h3c->dbuf), b_orig(&h3c->dbuf),
		      (unsigned int)b_head_ofs(&h3c->dbuf), (unsigned int)b_size(&h3c->dbuf),
		      h3c->msi,
		      br_head_idx(h3c->mbuf), br_tail_idx(h3c->mbuf), br_size(h3c->mbuf),
		      (unsigned int)b_data(hmbuf), b_orig(hmbuf),
		      (unsigned int)b_head_ofs(hmbuf), (unsigned int)b_size(hmbuf),
		      (unsigned int)b_data(tmbuf), b_orig(tmbuf),
		      (unsigned int)b_head_ofs(tmbuf), (unsigned int)b_size(tmbuf));

	if (h3s) {
		chunk_appendf(msg, " last_h3s=%p .id=%d .st=%s.flg=0x%04x .rxbuf=%u@%p+%u/%u .cs=%p",
			      h3s, h3s->id, h3s_st_to_str(h3s->st), h3s->flags,
			      (unsigned int)b_data(&h3s->rxbuf), b_orig(&h3s->rxbuf),
			      (unsigned int)b_head_ofs(&h3s->rxbuf), (unsigned int)b_size(&h3s->rxbuf),
			      h3s->cs);
		if (h3s->cs)
			chunk_appendf(msg, " .cs.flg=0x%08x .cs.data=%p",
				      h3s->cs->flags, h3s->cs->data);
	}
}

/* Migrate the the connection to the current thread.
 * Return 0 if successful, non-zero otherwise.
 * Expected to be called with the old thread lock held.
 */
static int h3_takeover(struct connection *conn)
{
	struct h3c *h3c = conn->ctx;

	if (fd_takeover(conn->handle.fd, conn) != 0)
		return -1;
	if (h3c->wait_event.events)
		h3c->conn->xprt->unsubscribe(h3c->conn, h3c->conn->xprt_ctx,
		    h3c->wait_event.events, &h3c->wait_event);
	/* To let the tasklet know it should free itself, and do nothing else,
	 * set its context to NULL.
	 */
	h3c->wait_event.tasklet->context = NULL;
	//tasklet_wakeup(h3c->wait_event.tasklet);
	if (h3c->task) {
		h3c->task->context = NULL;
		/* Wake the task, to let it free itself */
		task_wakeup(h3c->task, TASK_WOKEN_OTHER);

		h3c->task = task_new(tid_bit);
		if (!h3c->task) {
			h3_release(h3c);
			return -1;
		}
		h3c->task->process = h3_timeout_task;
		h3c->task->context = h3c;
	}
	h3c->wait_event.tasklet = tasklet_new();
	if (!h3c->wait_event.tasklet) {
		h3_release(h3c);
		return -1;
	}
	h3c->wait_event.tasklet->process = h3_io_cb;
	h3c->wait_event.tasklet->context = h3c;
	h3c->conn->xprt->subscribe(h3c->conn, h3c->conn->xprt_ctx,
		                   SUB_RETRY_RECV, &h3c->wait_event);

	return 0;
}

/*******************************************************/
/* functions below are dedicated to the config parsers */
/*******************************************************/

/* config parser for global "tune.h3.header-table-size" */
static int h3_parse_header_table_size(char **args, int section_type, struct proxy *curpx,
                                      struct proxy *defpx, const char *file, int line,
                                      char **err)
{
	if (too_many_args(1, args, err, NULL))
		return -1;

	h3_settings_header_table_size = atoi(args[1]);
	if (h3_settings_header_table_size < 4096 || h3_settings_header_table_size > 65536) {
		memprintf(err, "'%s' expects a numeric value between 4096 and 65536.", args[0]);
		return -1;
	}
	return 0;
}

/* config parser for global "tune.h3.initial-window-size" */
static int h3_parse_initial_window_size(char **args, int section_type, struct proxy *curpx,
                                        struct proxy *defpx, const char *file, int line,
                                        char **err)
{
	if (too_many_args(1, args, err, NULL))
		return -1;

	h3_settings_initial_window_size = atoi(args[1]);
	if (h3_settings_initial_window_size < 0) {
		memprintf(err, "'%s' expects a positive numeric value.", args[0]);
		return -1;
	}
	return 0;
}

/* config parser for global "tune.h3.max-concurrent-streams" */
static int h3_parse_max_concurrent_streams(char **args, int section_type, struct proxy *curpx,
                                           struct proxy *defpx, const char *file, int line,
                                           char **err)
{
	if (too_many_args(1, args, err, NULL))
		return -1;

	h3_settings_max_concurrent_streams = atoi(args[1]);
	if ((int)h3_settings_max_concurrent_streams < 0) {
		memprintf(err, "'%s' expects a positive numeric value.", args[0]);
		return -1;
	}
	return 0;
}

/* config parser for global "tune.h3.max-frame-size" */
static int h3_parse_max_frame_size(char **args, int section_type, struct proxy *curpx,
                                   struct proxy *defpx, const char *file, int line,
                                   char **err)
{
	if (too_many_args(1, args, err, NULL))
		return -1;

	h3_settings_max_frame_size = atoi(args[1]);
	if (h3_settings_max_frame_size < 16384 || h3_settings_max_frame_size > 16777215) {
		memprintf(err, "'%s' expects a numeric value between 16384 and 16777215.", args[0]);
		return -1;
	}
	return 0;
}


/****************************************/
/* MUX initialization and instanciation */
/***************************************/

/* The mux operations */
static const struct mux_ops h3_ops = {
	.init = h3_init,
	.wake = h3_wake,
	.snd_buf = h3_snd_buf,
	.rcv_buf = h3_rcv_buf,
	.subscribe = h3_subscribe,
	.unsubscribe = h3_unsubscribe,
	.attach = h3_attach,
	.get_first_cs = h3_get_first_cs,
	.detach = h3_detach,
	.destroy = h3_destroy,
	.avail_streams = h3_avail_streams,
	.used_streams = h3_used_streams,
	.shutr = h3_shutr,
	.shutw = h3_shutw,
	.ctl = h3_ctl,
	.show_fd = h3_show_fd,
	.takeover = h3_takeover,
	.flags = MX_FL_CLEAN_ABRT|MX_FL_HTX,
	.name = "H3",
};

static struct mux_proto_list mux_proto_h3 =
	{ .token = IST("h3"), .mode = PROTO_MODE_QUIC, .side = PROTO_SIDE_BOTH, .mux = &h3_ops };

INITCALL1(STG_REGISTER, register_mux_proto, &mux_proto_h3);

/* config keyword parsers */
static struct cfg_kw_list cfg_kws = {ILH, {
	{ CFG_GLOBAL, "tune.h3.header-table-size",      h3_parse_header_table_size      },
	{ CFG_GLOBAL, "tune.h3.initial-window-size",    h3_parse_initial_window_size    },
	{ CFG_GLOBAL, "tune.h3.max-concurrent-streams", h3_parse_max_concurrent_streams },
	{ CFG_GLOBAL, "tune.h3.max-frame-size",         h3_parse_max_frame_size         },
	{ 0, NULL, NULL }
}};

INITCALL1(STG_REGISTER, cfg_register_keywords, &cfg_kws);