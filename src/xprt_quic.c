/*
 * QUIC transport layer over SOCK_DGRAM sockets.
 *
 * Copyright 2019 HAProxy Technologies, Frédéric Lécaille <flecaille@haproxy.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <netinet/tcp.h>

#include <common/buffer.h>
#include <common/compat.h>
#include <common/config.h>
#include <common/debug.h>
#include <common/standard.h>
#include <common/ticks.h>
#include <common/time.h>

#include <proto/connection.h>
#include <proto/fd.h>
#include <proto/freq_ctr.h>
#include <proto/log.h>
#include <proto/pipe.h>
#include <proto/proxy.h>
#include <proto/quic_frame.h>
#include <proto/quic_tls.h>
#include <proto/ssl_sock.h>
#include <proto/stream_interface.h>
#include <proto/task.h>
#include <proto/trace.h>
#include <proto/xprt_quic.h>

#include <types/global.h>

struct quic_conn_ctx {
	struct connection *conn;
	SSL *ssl;
	BIO *bio;
	int state;
	const struct xprt_ops *xprt;
	void *xprt_ctx;
	struct wait_event wait_event;
	struct wait_event *subs;
};

struct quic_transport_params quid_dflt_transport_params = {
	.max_packet_size    = QUIC_DFLT_MAX_PACKET_SIZE,
	.ack_delay_exponent = QUIC_DFLT_ACK_DELAY_COMPONENT,
	.max_ack_delay      = QUIC_DFLT_MAX_ACK_DELAY,
};

/* trace source and events */
static void quic_trace(enum trace_level level, uint64_t mask, \
                       const struct trace_source *src,
                       const struct ist where, const struct ist func,
                       const void *a1, const void *a2, const void *a3, const void *a4);

static const struct trace_event quic_trace_events[] = {
	{ .mask = QUIC_EV_CONN_NEW,      .name = "new_conn",         .desc = "new QUIC connection" },
	{ .mask = QUIC_EV_CONN_INIT,     .name = "new_conn_init",    .desc = "new QUIC connection initialization" },
	{ .mask = QUIC_EV_CONN_ISEC,     .name = "init_secs",        .desc = "initial secrets derivation" },
	{ .mask = QUIC_EV_CONN_RSEC,     .name = "read_secs",        .desc = "read secrets derivation" },
	{ .mask = QUIC_EV_CONN_WSEC,     .name = "write_secs",       .desc = "write secrets derivation" },
	{ .mask = QUIC_EV_CONN_LPKT,     .name = "lstnr_packet",     .desc = "new listener received packet" },
	{ .mask = QUIC_EV_CONN_SPKT,     .name = "srv_packet",       .desc = "new server received packet" },
	{ .mask = QUIC_EV_CONN_CHPKT,    .name = "chdshk_pkt",       .desc = "clear handhshake packet building" },
	{ .mask = QUIC_EV_CONN_HPKT,     .name = "hdshk_pkt",        .desc = "handhshake packet building" },
	{ .mask = QUIC_EV_CONN_PAPKT,    .name = "phdshk_apkt",      .desc = "post handhshake application packet preparation" },
	{ .mask = QUIC_EV_CONN_PAPKTS,   .name = "phdshk_apkts",     .desc = "post handhshake application packets preparation" },
	{ .mask = QUIC_EV_CONN_HDSHK,    .name = "hdshk",            .desc = "SSL handhshake processing" },
	{ .mask = QUIC_EV_CONN_RMHP,     .name = "rm_hp",            .desc = "Remove header protection" },
	{ .mask = QUIC_EV_CONN_PRSHPKT,  .name = "parse_hpkt",       .desc = "parse handshake packet" },
	{ .mask = QUIC_EV_CONN_PRSAPKT,  .name = "parse_apkt",       .desc = "parse application packet" },
	{ .mask = QUIC_EV_CONN_PRSFRM,   .name = "parse_frm",        .desc = "parse frame" },
	{ .mask = QUIC_EV_CONN_PRSAFRM,  .name = "parse_ack_frm",    .desc = "parse ACK frame" },
	{ .mask = QUIC_EV_CONN_BFRM,     .name = "build_frm",        .desc = "build frame" },
	{ .mask = QUIC_EV_CONN_PHPKTS,   .name = "phdshk_pkts",      .desc = "handhshake packets preparation" },
	{ .mask = QUIC_EV_CONN_PHRPKTS,  .name = "phdshk_rpkts",     .desc = "handhshake packets preparation for retransmission" },
	{ .mask = QUIC_EV_CONN_TRMHP,    .name = "rm_hp_try",        .desc = "header protection removing try" },
	{ .mask = QUIC_EV_CONN_ELRMHP,   .name = "el_rm_hp",         .desc = "handshake enc. level header protection removing" },
	{ .mask = QUIC_EV_CONN_ELRXPKTS, .name = "el_treat_rx_pkts", .desc = "handshake enc. level rx packets treatment" },

	{ .mask = QUIC_EV_CONN_ENEW,     .name = "new_conn_err",     .desc = "error on new QUIC connection" },
	{ .mask = QUIC_EV_CONN_EISEC,    .name = "init_secs_err",    .desc = "error on initial secrets derivation" },
	{ .mask = QUIC_EV_CONN_ERSEC,    .name = "read_secs_err",    .desc = "error on read secrets derivation" },
	{ .mask = QUIC_EV_CONN_EWSEC,    .name = "write_secs_err",   .desc = "error on write secrets derivation" },
	{ .mask = QUIC_EV_CONN_ELPKT,    .name = "lstnr_packet_err", .desc = "error on new listener received packet" },
	{ .mask = QUIC_EV_CONN_ESPKT,    .name = "srv_packet_err",   .desc = "error on new server received packet" },
	{ .mask = QUIC_EV_CONN_ECHPKT,   .name = "chdshk_pkt_err",   .desc = "error on clear handhshake packet building" },
	{ .mask = QUIC_EV_CONN_EHPKT,    .name = "hdshk_pkt_err",    .desc = "error on handhshake packet building" },
	{ .mask = QUIC_EV_CONN_EPAPKT,   .name = "phdshk_apkt_err",  .desc = "error on post handhshake application packet building" },
	{ /* end */ }
};

static const struct name_desc quic_trace_lockon_args[4] = {
	/* arg1 */ { /* already used by the connection */ },
	/* arg2 */ { .name="quic", .desc="QUIC transport" },
	/* arg3 */ { },
	/* arg4 */ { }
};

static const struct name_desc quic_trace_decoding[] = {
#define QUIC_VERB_CLEAN    1
	{ .name="clean",    .desc="only user-friendly stuff, generally suitable for level \"user\"" },
	{ /* end */ }
};


struct trace_source trace_quic = {
	.name = IST("quic"),
	.desc = "QUIC xprt",
	.arg_def = TRC_ARG1_CONN,  /* TRACE()'s first argument is always a connection */
	.default_cb = quic_trace,
	.known_events = quic_trace_events,
	.lockon_args = quic_trace_lockon_args,
	.decoding = quic_trace_decoding,
	.report_events = ~0,  /* report everything by default */
};

#define TRACE_SOURCE    &trace_quic
INITCALL1(STG_REGISTER, trace_register_source, TRACE_SOURCE);

#if 1
__attribute__((format (printf, 3, 4)))
void hexdump(const void *buf, size_t buflen, const char *title_fmt, ...)
{
	int i;
	va_list ap;
	const unsigned char *p;
	char str_buf[2 + 1 + 16 + 1 + 1];

	va_start(ap, title_fmt);
	vfprintf(stderr, title_fmt, ap);
	va_end(ap);

	p = buf;
	str_buf[0] = str_buf[1] = ' ';
	str_buf[2] = '|';

	for (i = 0; i < buflen; i++) {
		if (!(i & 0xf))
			fprintf(stderr, "%08X: ", i);
		fprintf(stderr, " %02x", *p);
		if (isalnum(*p))
			str_buf[(i & 0xf) + 3] = *p;
		else
			str_buf[(i & 0xf) + 3] = '.';
		if ((i & 0xf) == 0xf || i == buflen -1) {
			int k;

			for (k = 0; k < (0x10 - (i & 0xf) - 1); k++)
				fprintf(stderr, "   ");
			str_buf[(i & 0xf) + 4] = '|';
			str_buf[(i & 0xf) + 5 ] = '\0';
			fprintf(stderr, "%s\n", str_buf);
		}
		p++;
	}
}
#else
__attribute__((format (printf, 3, 4)))
void hexdump(const void *buf, size_t buflen, const char *title_fmt, ...) {}
#endif

DECLARE_STATIC_POOL(pool_head_quic_conn, "quic_conn", sizeof(struct quic_conn));

DECLARE_POOL(pool_head_quic_connection_id,
             "quic_connnection_id_pool", sizeof(struct quic_connection_id));

DECLARE_STATIC_POOL(pool_head_quic_rx_packet, "quic_rx_packet_pool", sizeof(struct quic_rx_packet));

DECLARE_STATIC_POOL(pool_head_quic_conn_ctx, "quic_conn_ctx_pool", sizeof(struct quic_conn_ctx));

DECLARE_STATIC_POOL(pool_head_quic_tx_crypto_frm, "quic_tx_crypto_frm_pool", sizeof(struct quic_tx_crypto_frm));

DECLARE_STATIC_POOL(pool_head_quic_crypto_buf, "quic_crypto_buf_pool", sizeof(struct quic_crypto_buf));

DECLARE_STATIC_POOL(pool_head_quic_frame, "quic_frame_pool", sizeof(struct quic_frame));

DECLARE_STATIC_POOL(pool_head_quic_ack_range, "quic_ack_range_pool", sizeof(struct quic_ack_range));

static BIO_METHOD *ha_quic_meth;


static ssize_t qc_build_hdshk_pkt(struct q_buf *buf, struct quic_conn *qc, int pkt_type,
                                  uint64_t *offset, size_t len, struct quic_enc_level *qel);

static int qc_prep_phdshk_pkts(struct quic_conn *qc);

/*
 * the QUIC traces always expect that arg1, if non-null, is of type connection.
 */
static void quic_trace(enum trace_level level, uint64_t mask, const struct trace_source *src,
                       const struct ist where, const struct ist func,
                       const void *a1, const void *a2, const void *a3, const void *a4)
{
	const struct connection *conn = a1;

	if (conn) {
		struct quic_tls_secrets *secs;
		struct quic_conn *qc;

		qc = conn->quic_conn;
		chunk_appendf(&trace_buf, " : conn@%p", conn);
		if ((mask & QUIC_EV_CONN_INIT) && qc) {
			chunk_appendf(&trace_buf, "\n  odcid");
			quic_cid_dump(&trace_buf, &qc->odcid);
			chunk_appendf(&trace_buf, " dcid");
			quic_cid_dump(&trace_buf, &qc->dcid);
			chunk_appendf(&trace_buf, " scid");
			quic_cid_dump(&trace_buf, &qc->scid);
		}
		if ((mask & QUIC_EV_CONN_ISEC) && qc) {
			/* Initial read & write secrets. */
			enum quic_tls_enc_level level = QUIC_TLS_ENC_LEVEL_INITIAL;

			secs = &qc->enc_levels[level].tls_ctx.rx;
			if (secs->flags & QUIC_FL_TLS_SECRETS_SET) {
				chunk_appendf(&trace_buf, "\n  RX el=%c", quic_enc_level_char(level));
				quic_tls_keys_hexdump(&trace_buf, secs);
			}
			secs = &qc->enc_levels[level].tls_ctx.tx;
			if (secs->flags & QUIC_FL_TLS_SECRETS_SET) {
				chunk_appendf(&trace_buf, "\n  TX el=%c", quic_enc_level_char(level));
				quic_tls_keys_hexdump(&trace_buf, secs);
			}
		}
		if ((mask & QUIC_EV_CONN_RSEC)) {
			const long int level = (long int)a2;
			if (level) {
				secs = &qc->enc_levels[ssl_to_quic_enc_level(level)].tls_ctx.rx;
				if (secs->flags & QUIC_FL_TLS_SECRETS_SET) {
					chunk_appendf(&trace_buf, "\n  RX el=%c", quic_enc_level_char(level));
					quic_tls_keys_hexdump(&trace_buf, secs);
				}
			}
		}
		if ((mask & QUIC_EV_CONN_WSEC)) {
			const long int level = (long int)a2;
			if (level) {
				secs = &qc->enc_levels[ssl_to_quic_enc_level(level)].tls_ctx.tx;
				if (secs->flags & QUIC_FL_TLS_SECRETS_SET) {
					chunk_appendf(&trace_buf, "\n  TX el=%c", quic_enc_level_char(level));
					quic_tls_keys_hexdump(&trace_buf, secs);
				}
			}
		}
		if (mask & QUIC_EV_CONN_CHPKT) {
			const long int len = (long int)a2;

			if (qc->crypto_in_flight != QUIC_CRYPTO_IN_FLIGHT_MAX)
				chunk_appendf(&trace_buf, "\n  ifcdata=%lu", qc->crypto_in_flight);
			if (len)
				chunk_appendf(&trace_buf, " pktlen=%ld", len);
		}

		if (mask & QUIC_EV_CONN_HPKT) {
			const struct quic_tx_crypto_frm *cf = a2;

			if (cf)
				chunk_appendf(&trace_buf, "\n  pn=%lu offset=%lu len=%lu ifcdata=%zu",
				              (unsigned long)cf->pn.key, cf->offset, cf->len,
				              qc->crypto_in_flight);
		}
		if (mask & QUIC_EV_CONN_HDSHK) {
			const enum quic_handshake_state *state = a2;
			const long int *err = a3;

			if (state)
				chunk_appendf(&trace_buf, " state=%s", quic_hdshk_state_str(*state));
			if (err)
				chunk_appendf(&trace_buf, " err=%ld", *err);
		}

		if (mask & (QUIC_EV_CONN_TRMHP|QUIC_EV_CONN_ELRMHP)) {
			const struct quic_rx_packet *qpkt = a2;
			const unsigned long *pktlen = a3;

			if (qpkt) {
				chunk_appendf(&trace_buf, "\n  pkt@%p el=%c pnl=%u pn=%lu",
				              qpkt, quic_packet_type_enc_level_char(qpkt->type, qpkt->long_header),
				              qpkt->pnl, qpkt->pn);
				if (qpkt->token_len)
					chunk_appendf(&trace_buf, " toklen=%lu", qpkt->token_len);
				if (qpkt->aad_len)
					chunk_appendf(&trace_buf, " aadlen=%lu", qpkt->aad_len);
				chunk_appendf(&trace_buf, " flags:0x%x len=%lu", qpkt->flags, qpkt->len);
			}
			if (pktlen)
				chunk_appendf(&trace_buf, " (%ld)", *pktlen);
		}

		if (mask & QUIC_EV_CONN_ELRXPKTS) {
			const struct quic_rx_packet *pkt = a2;
			const SSL *ssl = a3;

			if (a2)
				chunk_appendf(&trace_buf, "\n  pkt@%p el=%c pn=%lu offset=%lu len=%lu", pkt,
							  quic_packet_type_enc_level_char(pkt->type, pkt->long_header),
							  pkt->pn, pkt->crypto.offset, pkt->crypto.len);
			if (ssl) {
				enum ssl_encryption_level_t level = SSL_quic_read_level(ssl);
				chunk_appendf(&trace_buf, " ssl_el=%c",
							  quic_enc_level_char(ssl_to_quic_enc_level(level)));
			}
		}
	}

	if (mask & (QUIC_EV_CONN_PRSFRM|QUIC_EV_CONN_BFRM)) {
		const struct quic_frame *frm = a2;

		if (a2)
			chunk_appendf(&trace_buf, " %s", quic_frame_type_string(frm->type));
	}

	if (mask & QUIC_EV_CONN_RMHP) {
		const struct quic_rx_packet *pkt;

		pkt = a2;
		if (pkt) {
			const int *ret = a3;

			chunk_appendf(&trace_buf, " pkt@%p", pkt);
			if (ret && *ret)
				chunk_appendf(&trace_buf, "\n  pnl=%u pn=%lu", pkt->pnl, pkt->pn);
		}
	}

	if (mask & QUIC_EV_CONN_PRSAFRM) {
		const unsigned long *val1 = a2;
		const unsigned long *val2 = a3;
		const unsigned long *val3 = a4;

		if (val1)
			chunk_appendf(&trace_buf, " %lu", *val1);
		if (val2)
			chunk_appendf(&trace_buf, "..%lu", *val2);
		if (val3)
			chunk_appendf(&trace_buf, "..%lu", *val3);
	}
}

#ifndef OPENSSL_IS_BORINGSSL
int ha_quic_set_encryption_secrets(SSL *ssl, enum ssl_encryption_level_t level,
                                   const uint8_t *read_secret,
                                   const uint8_t *write_secret, size_t secret_len)
{
	struct connection *conn = SSL_get_ex_data(ssl, ssl_app_data_index);
	struct quic_tls_ctx *tls_ctx =
		&conn->quic_conn->enc_levels[ssl_to_quic_enc_level(level)].tls_ctx;
	const SSL_CIPHER *cipher = SSL_get_current_cipher(ssl);

	tls_ctx->aead = tls_aead(cipher);
	tls_ctx->md = tls_md(cipher);
	tls_ctx->hp = tls_hp(cipher);

	HEXDUMP(read_secret, secret_len, "read_secret (level %d):\n", level);
	HEXDUMP(write_secret, secret_len, "write_secret:\n");

	if (!quic_tls_derive_keys(tls_ctx->aead, tls_ctx->hp, tls_ctx->md,
	                          tls_ctx->rx.key, sizeof tls_ctx->rx.key,
	                          tls_ctx->rx.iv, sizeof tls_ctx->rx.iv,
	                          tls_ctx->rx.hp_key, sizeof tls_ctx->rx.hp_key,
	                          read_secret, secret_len)) {
		QDPRINTF("%s: RX key derivation failed\n", __func__);
		return 0;
	}

	if (!quic_tls_derive_keys(tls_ctx->aead, tls_ctx->hp, tls_ctx->md,
	                          tls_ctx->tx.key, sizeof tls_ctx->tx.key,
	                          tls_ctx->tx.iv, sizeof tls_ctx->tx.iv,
	                          tls_ctx->tx.hp_key, sizeof tls_ctx->tx.hp_key,
	                          write_secret, secret_len)) {
		QDPRINTF("%s: TX key derivation failed\n", __func__);
		return 0;
	}

	if (objt_server(conn->target) && level == ssl_encryption_application) {
		struct quic_transport_params *tp = &conn->quic_conn->rx_tps;
		const unsigned char *buf;
		size_t buflen;

		SSL_get_peer_quic_transport_params(ssl, &buf, &buflen);
		if (!buflen)
			return 0;

		if (!quic_transport_params_decode(tp, 1, buf, buf + buflen))
			return 0;
	}

	return 1;
}
#else
/*
 * ->set_read_secret callback to derive the RX secrets at <level>
 * encryption level.
 * Returns 1 if succedded, 0 if not.
 */
int ha_set_rsec(SSL *ssl, enum ssl_encryption_level_t level,
                const SSL_CIPHER *cipher,
                const uint8_t *secret, size_t secret_len)
{
	struct connection *conn = SSL_get_ex_data(ssl, ssl_app_data_index);
	struct quic_tls_ctx *tls_ctx =
		&conn->quic_conn->enc_levels[ssl_to_quic_enc_level(level)].tls_ctx;

	TRACE_ENTER(QUIC_EV_CONN_RSEC, conn);
	tls_ctx->rx.aead = tls_aead(cipher);
	tls_ctx->rx.md = tls_md(cipher);
	tls_ctx->rx.hp = tls_hp(cipher);

	HEXDUMP(secret, secret_len, "RX secret (level %d):\n", level);
	if (!quic_tls_derive_keys(tls_ctx->rx.aead, tls_ctx->rx.hp, tls_ctx->rx.md,
	                          tls_ctx->rx.key, sizeof tls_ctx->rx.key,
	                          tls_ctx->rx.iv, sizeof tls_ctx->rx.iv,
	                          tls_ctx->rx.hp_key, sizeof tls_ctx->rx.hp_key,
	                          secret, secret_len)) {
		TRACE_DEVEL("RX key derivation failed", QUIC_EV_CONN_RSEC, conn);
		goto err;
	}

	if (objt_server(conn->target) && level == ssl_encryption_application) {
		struct quic_transport_params *tp = &conn->quic_conn->rx_tps;
		const unsigned char *buf;
		size_t buflen;

		SSL_get_peer_quic_transport_params(ssl, &buf, &buflen);
		if (!buflen)
			goto err;

		if (!quic_transport_params_decode(tp, 1, buf, buf + buflen))
			goto err;
	}

	tls_ctx->rx.flags |= QUIC_FL_TLS_SECRETS_SET;
	TRACE_LEAVE(QUIC_EV_CONN_RSEC, conn, (int *)level);

	return 1;

 err:
	TRACE_DEVEL("leaving in error", QUIC_EV_CONN_ERSEC, conn);
	return 0;
}
/*
 * ->set_write_secret callback to derive the TX secrets at <level>
 * encryption level.
 * Returns 1 if succedded, 0 if not.
 */
int ha_set_wsec(SSL *ssl, enum ssl_encryption_level_t level,
                const SSL_CIPHER *cipher,
                const uint8_t *secret, size_t secret_len)
{
	struct connection *conn = SSL_get_ex_data(ssl, ssl_app_data_index);
	struct quic_tls_ctx *tls_ctx =
		&conn->quic_conn->enc_levels[ssl_to_quic_enc_level(level)].tls_ctx;

	TRACE_ENTER(QUIC_EV_CONN_WSEC, conn);
	tls_ctx->tx.aead = tls_aead(cipher);
	tls_ctx->tx.md = tls_md(cipher);
	tls_ctx->tx.hp = tls_hp(cipher);

	HEXDUMP(secret, secret_len, "TX secret (level %d):\n", level);
	if (!quic_tls_derive_keys(tls_ctx->tx.aead, tls_ctx->tx.hp, tls_ctx->tx.md,
	                          tls_ctx->tx.key, sizeof tls_ctx->tx.key,
	                          tls_ctx->tx.iv, sizeof tls_ctx->tx.iv,
	                          tls_ctx->tx.hp_key, sizeof tls_ctx->tx.hp_key,
	                          secret, secret_len)) {
		TRACE_DEVEL("TX key derivation failed", QUIC_EV_CONN_WSEC, conn);
		goto err;
	}
	tls_ctx->tx.flags |= QUIC_FL_TLS_SECRETS_SET;
	TRACE_LEAVE(QUIC_EV_CONN_WSEC, conn, (int *)level);

	return 1;

 err:
	TRACE_DEVEL("leaving in error", QUIC_EV_CONN_EWSEC, conn);
	return 0;
}
#endif

/*
 * This function copies the CRYPTO data provided by the TLS stack found at <data>
 * with <len> as size in CRYPTO buffers dedicated to store the information about
 * outgoing CRYPTO frames so that to be able to replay the CRYPTO data streams.
 * It fails only if it could not managed to allocate enough CRYPTO buffers to
 * store all the data.
 * Note that CRYPTO data may exist at any encryption level except at 0-RTT.
 */
static int quic_crypto_data_cpy(struct quic_enc_level *qel,
                                const unsigned char *data, size_t len)
{
	struct quic_crypto_buf **qcb;
	/* The remaining byte to store in CRYPTO buffers. */
	size_t *nb_buf;
	unsigned char *pos;

	nb_buf = &qel->tx.crypto.nb_buf;
	qcb = &qel->tx.crypto.bufs[*nb_buf - 1];
	pos = (*qcb)->data + (*qcb)->sz;

	while (len > 0) {
		size_t to_copy, room;

		room = QUIC_CRYPTO_BUF_SZ  - (*qcb)->sz;
		to_copy = len > room ? room : len;
		memcpy(pos, data, to_copy);
		/* Increment the total size of this CRYPTO buffers by <to_copy>. */
		qel->tx.crypto.sz += to_copy;
		(*qcb)->sz += to_copy;
		pos += to_copy;
		len -= to_copy;
		data += to_copy;
		if ((*qcb)->sz >= QUIC_CRYPTO_BUF_SZ) {
			struct quic_crypto_buf **tmp;

			tmp = realloc(qel->tx.crypto.bufs,
			              (*nb_buf + 1) * sizeof *qel->tx.crypto.bufs);
			if (tmp) {
				qel->tx.crypto.bufs = tmp;
				qcb = &qel->tx.crypto.bufs[*nb_buf];
				*qcb = pool_alloc(pool_head_quic_crypto_buf);
				if (!*qcb) {
					QDPRINTF("%s: crypto allocation failed\n", __func__);
					return 0;
				}
				(*qcb)->sz = 0;
				pos = (*qcb)->data;
				++*nb_buf;
			}
			else {
				/* XXX deallocate everything */
			}
		}
	}

	return len == 0;
}


/*
 * ->add_handshake_data QUIC TLS callback used by the QUIC TLS stack when it
 * wants to provide the QUIC layer with CRYPTO data.
 * Returns 1 if succeeded, 0 if not.
 */
int ha_quic_add_handshake_data(SSL *ssl, enum ssl_encryption_level_t level,
                               const uint8_t *data, size_t len)
{
	struct connection *conn;
	enum quic_tls_enc_level tls_enc_level;
	struct quic_enc_level *qel;

	conn = SSL_get_ex_data(ssl, ssl_app_data_index);
	tls_enc_level = ssl_to_quic_enc_level(level);
	qel = &conn->quic_conn->enc_levels[tls_enc_level];

	if (tls_enc_level != QUIC_TLS_ENC_LEVEL_INITIAL &&
	    tls_enc_level != QUIC_TLS_ENC_LEVEL_HANDSHAKE)
		return 0;

	if (!qel->tx.crypto.bufs) {
		QDPRINTF("Crypto buffers could not be allacated\n");
		return 0;
	}
	if (!quic_crypto_data_cpy(qel, data, len)) {
		QDPRINTF("Too much crypto data (%zu bytes)\n", len);
		return 0;
	}

	return 1;
}

int ha_quic_flush_flight(SSL *ssl)
{
	struct connection *conn = SSL_get_ex_data(ssl, ssl_app_data_index);

	QDPRINTF("%s\n", __func__);
	tasklet_wakeup(((struct quic_conn_ctx *)conn->xprt_ctx)->wait_event.tasklet);

	return 1;
}

int ha_quic_send_alert(SSL *ssl, enum ssl_encryption_level_t level, uint8_t alert)
{
	QDPRINTF("%s\n", __func__);
	return 1;
}

/* QUIC TLS methods */
static SSL_QUIC_METHOD ha_quic_method = {
#ifdef OPENSSL_IS_BORINGSSL
	.set_read_secret        = ha_set_rsec,
	.set_write_secret       = ha_set_wsec,
#else
	.set_encryption_secrets = ha_quic_set_encryption_secrets,
#endif
	.add_handshake_data     = ha_quic_add_handshake_data,
	.flush_flight           = ha_quic_flush_flight,
	.send_alert             = ha_quic_send_alert,
};

/*
 * Initialize the TLS context of a listener with <bind_conf> as configuration.
 * Returns an error count.
 */
int ssl_quic_initial_ctx(struct bind_conf *bind_conf)
{
	struct proxy *curproxy = bind_conf->frontend;
	struct ssl_bind_conf __maybe_unused *ssl_conf_cur;
	int cfgerr = 0;

#if 0
	/* XXX Did not manage to use this. */
	const char *ciphers =
		"TLS_AES_128_GCM_SHA256:"
		"TLS_AES_256_GCM_SHA384:"
		"TLS_CHACHA20_POLY1305_SHA256:"
		"TLS_AES_128_CCM_SHA256";
#endif
	const char *groups = "P-256:X25519:P-384:P-521";
	long options =
		(SSL_OP_ALL & ~SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS) |
		SSL_OP_SINGLE_ECDH_USE |
		SSL_OP_CIPHER_SERVER_PREFERENCE;
	SSL_CTX *ctx;

	ctx = SSL_CTX_new(TLS_server_method());
	bind_conf->initial_ctx = ctx;

	SSL_CTX_set_options(ctx, options);
#if 0
	if (SSL_CTX_set_cipher_list(ctx, ciphers) != 1) {
		ha_alert("Proxy '%s': unable to set TLS 1.3 cipher list to '%s' "
		         "for bind '%s' at [%s:%d].\n",
		         curproxy->id, ciphers,
		         bind_conf->arg, bind_conf->file, bind_conf->line);
		cfgerr++;
	}
#endif

	if (SSL_CTX_set1_curves_list(ctx, groups) != 1) {
		ha_alert("Proxy '%s': unable to set TLS 1.3 curves list to '%s' "
		         "for bind '%s' at [%s:%d].\n",
		         curproxy->id, groups,
		         bind_conf->arg, bind_conf->file, bind_conf->line);
		cfgerr++;
	}

	SSL_CTX_set_mode(ctx, SSL_MODE_RELEASE_BUFFERS);
	SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
	SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION);
	SSL_CTX_set_default_verify_paths(ctx);

#ifdef SSL_CTRL_SET_TLSEXT_HOSTNAME
#ifdef OPENSSL_IS_BORINGSSL
	SSL_CTX_set_select_certificate_cb(ctx, ssl_sock_switchctx_cbk);
	SSL_CTX_set_tlsext_servername_callback(ctx, ssl_sock_switchctx_err_cbk);
#elif (HA_OPENSSL_VERSION_NUMBER >= 0x10101000L)
	if (bind_conf->ssl_conf.early_data) {
		SSL_CTX_set_options(ctx, SSL_OP_NO_ANTI_REPLAY);
		SSL_CTX_set_max_early_data(ctx, global.tune.bufsize - global.tune.maxrewrite);
	}
	SSL_CTX_set_client_hello_cb(ctx, ssl_sock_switchctx_cbk, NULL);
	SSL_CTX_set_tlsext_servername_callback(ctx, ssl_sock_switchctx_err_cbk);
#else
	SSL_CTX_set_tlsext_servername_callback(ctx, ssl_sock_switchctx_cbk);
#endif
	SSL_CTX_set_tlsext_servername_arg(ctx, bind_conf);
#endif
	SSL_CTX_set_quic_method(ctx, &ha_quic_method);

	return cfgerr;
}

/* Receive up to <count> bytes from connection <conn>'s socket and store them
 * into buffer <buf>. Only one call to recv() is performed, unless the
 * buffer wraps, in which case a second call may be performed. The connection's
 * flags are updated with whatever special event is detected (error, read0,
 * empty). The caller is responsible for taking care of those events and
 * avoiding the call if inappropriate. The function does not call the
 * connection's polling update function, so the caller is responsible for this.
 * errno is cleared before starting so that the caller knows that if it spots an
 * error without errno, it's pending and can be retrieved via getsockopt(SO_ERROR).
 */
static size_t quic_conn_to_buf(struct connection *conn, void *xprt_ctx, struct buffer *buf, size_t count, int flags)
{
	ssize_t ret;
	size_t try, done = 0;

	if (!conn_ctrl_ready(conn))
		return 0;

	if (!fd_recv_ready(conn->handle.fd))
		return 0;

	errno = 0;

	if (unlikely(!(fdtab[conn->handle.fd].ev & FD_POLL_IN))) {
		/* stop here if we reached the end of data */
		if ((fdtab[conn->handle.fd].ev & (FD_POLL_ERR|FD_POLL_HUP)) == FD_POLL_HUP)
			goto read0;

		/* report error on POLL_ERR before connection establishment */
		if ((fdtab[conn->handle.fd].ev & FD_POLL_ERR) && (conn->flags & CO_FL_WAIT_L4_CONN)) {
			conn->flags |= CO_FL_ERROR | CO_FL_SOCK_RD_SH | CO_FL_SOCK_WR_SH;
			goto leave;
		}
	}

	/* read the largest possible block. For this, we perform only one call
	 * to recv() unless the buffer wraps and we exactly fill the first hunk,
	 * in which case we accept to do it once again. A new attempt is made on
	 * EINTR too.
	 */
	while (count > 0) {
		try = b_contig_space(buf);
		if (!try)
			break;

		if (try > count)
			try = count;

		ret = recvfrom(conn->handle.fd, b_tail(buf), try, 0, NULL, 0);

		if (ret > 0) {
			b_add(buf, ret);
			done += ret;
			if (ret < try) {
				/* unfortunately, on level-triggered events, POLL_HUP
				 * is generally delivered AFTER the system buffer is
				 * empty, unless the poller supports POLL_RDHUP. If
				 * we know this is the case, we don't try to read more
				 * as we know there's no more available. Similarly, if
				 * there's no problem with lingering we don't even try
				 * to read an unlikely close from the client since we'll
				 * close first anyway.
				 */
				if (fdtab[conn->handle.fd].ev & FD_POLL_HUP)
					goto read0;

				if ((!fdtab[conn->handle.fd].linger_risk) ||
				    (cur_poller.flags & HAP_POLL_F_RDHUP)) {
					fd_done_recv(conn->handle.fd);
					break;
				}
			}
			count -= ret;
		}
		else if (ret == 0) {
			goto read0;
		}
		else if (errno == EAGAIN || errno == ENOTCONN) {
			fd_cant_recv(conn->handle.fd);
			break;
		}
		else if (errno != EINTR) {
			conn->flags |= CO_FL_ERROR | CO_FL_SOCK_RD_SH | CO_FL_SOCK_WR_SH;
			break;
		}
	}

	if (unlikely(conn->flags & CO_FL_WAIT_L4_CONN) && done)
		conn->flags &= ~CO_FL_WAIT_L4_CONN;

 leave:
	return done;

 read0:
	conn_sock_read0(conn);
	conn->flags &= ~CO_FL_WAIT_L4_CONN;

	/* Now a final check for a possible asynchronous low-level error
	 * report. This can happen when a connection receives a reset
	 * after a shutdown, both POLL_HUP and POLL_ERR are queued, and
	 * we might have come from there by just checking POLL_HUP instead
	 * of recv()'s return value 0, so we have no way to tell there was
	 * an error without checking.
	 */
	if (unlikely(fdtab[conn->handle.fd].ev & FD_POLL_ERR))
		conn->flags |= CO_FL_ERROR | CO_FL_SOCK_RD_SH | CO_FL_SOCK_WR_SH;
	goto leave;
}


/* Send up to <count> pending bytes from buffer <buf> to connection <conn>'s
 * socket. <flags> may contain some CO_SFL_* flags to hint the system about
 * other pending data for example, but this flag is ignored at the moment.
 * Only one call to send() is performed, unless the buffer wraps, in which case
 * a second call may be performed. The connection's flags are updated with
 * whatever special event is detected (error, empty). The caller is responsible
 * for taking care of those events and avoiding the call if inappropriate. The
 * function does not call the connection's polling update function, so the caller
 * is responsible for this. It's up to the caller to update the buffer's contents
 * based on the return value.
 */
static size_t quic_conn_from_buf(struct connection *conn, void *xprt_ctx, const struct buffer *buf, size_t count, int flags)
{
	ssize_t ret;
	size_t try, done;
	int send_flag;

	if (!conn_ctrl_ready(conn))
		return 0;

	if (!fd_send_ready(conn->handle.fd))
		return 0;

	done = 0;
	/* send the largest possible block. For this we perform only one call
	 * to send() unless the buffer wraps and we exactly fill the first hunk,
	 * in which case we accept to do it once again.
	 */
	while (count) {
		try = b_contig_data(buf, done);
		if (try > count)
			try = count;

		send_flag = MSG_DONTWAIT | MSG_NOSIGNAL;
		if (try < count || flags & CO_SFL_MSG_MORE)
			send_flag |= MSG_MORE;

		ret = sendto(conn->handle.fd, b_peek(buf, done), try, send_flag,
		             (struct sockaddr *)conn->dst, get_addr_len(conn->dst));
		if (ret > 0) {
			count -= ret;
			done += ret;

			/* A send succeeded, so we can consier ourself connected */
			conn->flags |= CO_FL_WAIT_L4L6;
			/* if the system buffer is full, don't insist */
			if (ret < try)
				break;
		}
		else if (ret == 0 || errno == EAGAIN || errno == ENOTCONN || errno == EINPROGRESS) {
			/* nothing written, we need to poll for write first */
			fd_cant_send(conn->handle.fd);
			break;
		}
		else if (errno != EINTR) {
			conn->flags |= CO_FL_ERROR | CO_FL_SOCK_RD_SH | CO_FL_SOCK_WR_SH;
			break;
		}
	}
	if (unlikely(conn->flags & CO_FL_WAIT_L4_CONN) && done)
		conn->flags &= ~CO_FL_WAIT_L4_CONN;

	if (done > 0) {
		/* we count the total bytes sent, and the send rate for 32-byte
		 * blocks. The reason for the latter is that freq_ctr are
		 * limited to 4GB and that it's not enough per second.
		 */
		_HA_ATOMIC_ADD(&global.out_bytes, done);
		update_freq_ctr(&global.out_32bps, (done + 16) / 32);
	}
	return done;
}

static int quic_conn_subscribe(struct connection *conn, void *xprt_ctx, int event_type, struct wait_event *es)
{
	return conn_subscribe(conn, xprt_ctx, event_type, es);
}

static int quic_conn_unsubscribe(struct connection *conn, void *xprt_ctx, int event_type, struct wait_event *es)
{
	return conn_unsubscribe(conn, xprt_ctx, event_type, es);
}

/*
 * Decode an expected packet number from <truncated_on> its truncated value,
 * depending on <largest_pn> the largest received packet number, and <pn_nbits>
 * the number of bits used to encode this packet number (its length in bytes * 8).
 * See https://quicwg.org/base-drafts/draft-ietf-quic-transport.html#packet-encoding
 */
static uint64_t decode_packet_number(uint64_t largest_pn,
                                     uint32_t truncated_pn, unsigned int pn_nbits)
{
	uint64_t expected_pn = largest_pn + 1;
	uint64_t pn_win = (uint64_t)1 << pn_nbits;
	uint64_t pn_hwin = pn_win / 2;
	uint64_t pn_mask = pn_win - 1;
	uint64_t candidate_pn;


	candidate_pn = (expected_pn & ~pn_mask) | truncated_pn;
	if (candidate_pn + pn_hwin <= expected_pn)
	  return candidate_pn + pn_win;

	if (candidate_pn > expected_pn + pn_hwin && candidate_pn > pn_win)
	  return candidate_pn - pn_win;

	return candidate_pn;
}

/*
 * Remove the header protection of <pkt> QUIC packet using <tls_ctx> as QUIC TLS
 * cryptographic context.
 * <largest_pn> is the largest received packet number and <pn> the address of
 * the packet number field for this packet with <byte0> address of its first byte.
 * <end> points to one byte past the end of this packet.
 * Returns 1 if succeeded, 0 if not.
 */
static int qc_do_rm_hp(struct quic_rx_packet *pkt, struct quic_tls_ctx *tls_ctx,
                       int64_t largest_pn, unsigned char *pn,
                       unsigned char *byte0, const unsigned char *end)
{
	int ret, outlen, i, pnlen;
	uint64_t packet_number;
	uint32_t truncated_pn = 0;
	unsigned char mask[5] = {0};
	unsigned char *sample;
	EVP_CIPHER_CTX *ctx;
	unsigned char *hp_key;

	TRACE_ENTER(QUIC_EV_CONN_RMHP,, pkt);
	/* Check there is enough data in this packet. */
	if (end - pn < QUIC_PACKET_PN_MAXLEN + sizeof mask) {
		TRACE_DEVEL("too short packet", QUIC_EV_CONN_RMHP,, pkt);
		return 0;
	}

	ctx = EVP_CIPHER_CTX_new();
	if (!ctx) {
		TRACE_DEVEL("memory allocation failed", QUIC_EV_CONN_RMHP,, pkt);
		return 0;
	}

	ret = 0;
	sample = pn + QUIC_PACKET_PN_MAXLEN;

	hp_key = tls_ctx->rx.hp_key;
	if (!EVP_DecryptInit_ex(ctx, tls_ctx->rx.hp, NULL, hp_key, sample) ||
	    !EVP_DecryptUpdate(ctx, mask, &outlen, mask, sizeof mask) ||
	    !EVP_DecryptFinal_ex(ctx, mask, &outlen)) {
		TRACE_DEVEL("decryption failed", QUIC_EV_CONN_RMHP,, pkt);
	    goto out;
	}

	*byte0 ^= mask[0] & (*byte0 & QUIC_PACKET_LONG_HEADER_BIT ? 0xf : 0x1f);
	pnlen = (*byte0 & QUIC_PACKET_PNL_BITMASK) + 1;
	for (i = 0; i < pnlen; i++) {
		pn[i] ^= mask[i + 1];
		truncated_pn = (truncated_pn << 8) | pn[i];
	}

	packet_number = decode_packet_number(largest_pn, truncated_pn, pnlen * 8);
	/* Store remaining information for this unprotected header */
	pkt->pn = packet_number;
	pkt->pnl = pnlen;

	ret = 1;

 out:
	EVP_CIPHER_CTX_free(ctx);
	TRACE_LEAVE(QUIC_EV_CONN_RMHP,, pkt, &ret);

	return ret;
}

/*
 * Encrypt the payload of a QUIC packet with <pn> as number found at <payload>
 * address, with <payload_len> as payload length, <aad> as address of
 * the ADD and <aad_len> as AAD length depending on the <tls_ctx> QUIC TLS
 * context.
 * Returns 1 if succeeded, 0 if not.
 */
static int quic_packet_encrypt(unsigned char *payload, size_t payload_len,
                               unsigned char *aad, size_t aad_len, uint64_t pn,
                               struct quic_tls_ctx *tls_ctx, struct connection *conn)
{
	unsigned char iv[12];
	unsigned char *tx_iv = tls_ctx->tx.iv;
	size_t tx_iv_sz = sizeof tls_ctx->tx.iv;

	if (!quic_aead_iv_build(iv, sizeof iv, tx_iv, tx_iv_sz, pn)) {
		TRACE_DEVEL("AEAD IV building for encryption failed", QUIC_EV_CONN_HPKT, conn);
		return 0;
	}

	if (!quic_tls_encrypt(payload, payload_len, aad, aad_len,
	                      tls_ctx->tx.aead, tls_ctx->tx.key, iv)) {
		TRACE_DEVEL("QUIC packet encryption failed", QUIC_EV_CONN_HPKT, conn);
		return 0;
	}

	return 1;
}

/*
 * Decrypt <qpkt> QUIC packet with <tls_ctx> as QUIC TLS cryptographic context.
 * Returns 1 if succeeded, 0 if not.
 */
static int qc_pkt_decrypt(struct quic_rx_packet *qpkt, struct quic_tls_ctx *tls_ctx)
{
	int ret;
	unsigned char iv[12];
	unsigned char *rx_iv = tls_ctx->rx.iv;
	size_t rx_iv_sz = sizeof tls_ctx->rx.iv;

	if (!quic_aead_iv_build(iv, sizeof iv, rx_iv, rx_iv_sz, qpkt->pn)) {
		QDPRINTF("%s AEAD IV building failed\n", __func__);
		return 0;
	}

	ret = quic_tls_decrypt(qpkt->data + qpkt->aad_len, qpkt->len - qpkt->aad_len,
	                       qpkt->data, qpkt->aad_len,
	                       tls_ctx->rx.aead, tls_ctx->rx.key, iv);
	if (!ret) {
		QDPRINTF("%s: qpkt #%lu long %d decryption failed\n",
		         __func__, qpkt->pn, qpkt->long_header);
		return 0;
	}

	/* Update the packet length (required to parse the frames). */
	qpkt->len = qpkt->aad_len + ret;
	QDPRINTF("QUIC packet #%lu long header? %d decryption done\n",
	         qpkt->pn, !!qpkt->long_header);

	return 1;
}

/*
 * Remove <largest> down to <smallest> node entries from <frms> root of CRYPTO frames
 * deallocating them, these frames being acknowledged.
 * Returns the last node reached to be used for the next range.
 * May be NULL if <largest> node could not be found.
 */
static inline struct eb64_node *
quic_ack_range_crypto_frames(struct eb_root *frms, uint64_t *ifcdata,
                             uint64_t largest, uint64_t smallest)
{
	struct eb64_node *node;
	struct quic_tx_crypto_frm *frm;

	node = eb64_lookup(frms, largest);
	while (node && node->key >= smallest) {
		frm = eb64_entry(&node->node, struct quic_tx_crypto_frm, pn);
		QDPRINTF("Removing CRYPTO frame #%llu\n", frm->pn.key);
		TRACE_PROTO("cfrm ackd", QUIC_EV_CONN_PRSAFRM,,
		             &frm->pn.key, &frm->offset, &frm->len);
		*ifcdata -= frm->len,
		node = eb64_prev(node);
		eb64_delete(&frm->pn);
		pool_free(pool_head_quic_tx_crypto_frm, frm);
	}

	return node;
}

/*
 * Remove <largest> down to <smallest> node entries from <frms> root of acket
 * CRYPTO frames deallocating them and accumulate the CRYPTO frames belonging to
 * the same gap <smallest> -> <next_largest> non inclusive in a unique frame to
 * be retransmitted if any.
 * It is possible that this frame does not exist if the ranges have been already
 * parsed (but not acknowledged).
 * Note that <largest> >= <smallest> > <next_largest>.
 * Also updates <ifcdata> in flight crypto data counter.
 * Returns a frame which results of the aggregation of the lost frames belonging
 * to the same gap.
 */
static inline struct quic_tx_crypto_frm *
quic_ack_range_with_gap_crypto_frames(struct eb_root *frms,
                                      uint64_t *ifcdata,
                                      uint64_t largest, uint64_t smallest,
                                      uint64_t next_largest)
{
	struct eb64_node *node;
	struct quic_tx_crypto_frm *frm;

	node = quic_ack_range_crypto_frames(frms, ifcdata, largest, smallest);
	if (!node)
		return NULL;

	/* Aggregate the consecutive CRYPTO frames belonging to the same gap. */
	frm = eb64_entry(&node->node, struct quic_tx_crypto_frm, pn);
	TRACE_PROTO("to resend",QUIC_EV_CONN_PRSAFRM,, &frm->pn.key, &frm->offset, &frm->len);
	node = eb64_prev(node);
	while (node && node->key > next_largest) {
		struct quic_tx_crypto_frm *prev_frm;

		prev_frm = eb64_entry(&node->node, struct quic_tx_crypto_frm, pn);
		prev_frm->len += frm->len;
		eb64_delete(&frm->pn);
		pool_free(pool_head_quic_tx_crypto_frm, frm);
		frm = prev_frm;
		TRACE_PROTO("to resend",QUIC_EV_CONN_PRSAFRM,,
					&frm->pn.key, &frm->offset, &frm->len);
		node = eb64_prev(node);
	}
	*ifcdata -= frm->len;

	return frm;
}

/*
 * Parse ACK frame into <frm> from a buffer at <buf> address with <end> being at
 * one byte past the end of this buffer.
 * Return 1, if succeeded, 0 if not.
 */
static inline int qc_parse_ack_frm(struct quic_frame *frm, struct quic_conn_ctx *ctx,
                                   struct quic_enc_level *qel,
                                   const unsigned char **pos, const unsigned char *end)
{
	struct quic_ack *ack = &frm->ack;
	uint64_t smallest, largest;

	if (ack->largest_ack > qel->pktns->tx.next_pn) {
		TRACE_DEVEL("ACK for not sent packet", QUIC_EV_CONN_PRSAFRM,
		            ctx->conn, &ack->largest_ack);
		goto err;
	}

	if (ack->first_ack_range > ack->largest_ack) {
		TRACE_DEVEL("too big first ACK range", QUIC_EV_CONN_PRSAFRM,
		            ctx->conn, &ack->first_ack_range);
		goto err;
	}

	largest = ack->largest_ack;
	smallest = largest - ack->first_ack_range;
	TRACE_PROTO("ack range", QUIC_EV_CONN_PRSAFRM,, &largest, &smallest);
	do {
		uint64_t gap, ack_range;
		struct quic_tx_crypto_frm *frm;

		if (!ack->ack_range_num--) {
			quic_ack_range_crypto_frames(&qel->tx.crypto.frms,
			                             &ctx->conn->quic_conn->crypto_in_flight,
			                             largest, smallest);
			break;
		}

		if (!quic_dec_int(&gap, pos, end) || smallest < gap + 2) {
			TRACE_DEVEL("wrong gap value", QUIC_EV_CONN_PRSAFRM,
						ctx->conn, &gap, &smallest);
			goto err;
		}

		if (!quic_dec_int(&ack_range, pos, end) || smallest - gap - 2 < ack_range) {
			TRACE_DEVEL("wrong ack range value", QUIC_EV_CONN_PRSAFRM,
						ctx->conn, &ack_range, &gap, &smallest);
			goto err;
		}

		frm = quic_ack_range_with_gap_crypto_frames(&qel->tx.crypto.frms,
		                                            &ctx->conn->quic_conn->crypto_in_flight,
		                                            largest, smallest, smallest - gap - 2);
		if (frm) {
			eb64_delete(&frm->pn);
			eb64_insert(&qel->tx.crypto.retransmit_frms, &frm->pn);
			ctx->conn->quic_conn->retransmit = 1;
		}
		/* Next range */
		largest = smallest - gap - 2;
		smallest = largest - ack_range;

		TRACE_PROTO("ack range", QUIC_EV_CONN_PRSAFRM,, &largest, &smallest);
	} while (1);

	if (ack->largest_ack > qel->pktns->rx.largest_acked_pn)
		qel->pktns->rx.largest_acked_pn = ack->largest_ack;

	return 1;

 err:
	TRACE_DEVEL("leaving in error", QUIC_EV_CONN_PRSAFRM, ctx->conn);
	return 0;
}

/*
 * Parse all the frames of <qpkt> QUIC packet for QUIC connection with <ctx>
 * as I/O handler context and <qel> as encryption level.
 * Returns 1 if succeeded, 0 if failed.
 */
static int qc_parse_hdshk_pkt(struct quic_rx_packet *qpkt, struct quic_conn_ctx *ctx,
                              struct quic_enc_level *qel)
{
	struct quic_frame frm;
	const unsigned char *pos, *end;

	TRACE_ENTER(QUIC_EV_CONN_PRSHPKT, ctx->conn);
	/* Skip the AAD */
	pos = qpkt->data + qpkt->aad_len;
	end = qpkt->data + qpkt->len;

	while (pos < end) {
		if (!qc_parse_frm(&frm, &pos, end))
			goto err;

		switch (frm.type) {
		case QUIC_FT_CRYPTO:
			if (frm.crypto.offset != qel->rx.crypto.offset)
				qpkt->flags |= QUIC_FL_RX_PACKET_OUT_OF_ORDER;

			/* Store the CRYPTO frame information. */
			qpkt->crypto.offset = frm.crypto.offset;
			qpkt->crypto.len = frm.crypto.len;
			qpkt->crypto.data = frm.crypto.data;
			/* ack-eliciting frame. */
			qpkt->flags |= QUIC_FL_RX_PACKET_ACK_ELICITING;
			break;
		case QUIC_FT_PADDING:
			if (pos != end) {
				QDPRINTF("Wrong frame (remainging: %ld padding len: %lu)\n",
				         end - pos, frm.padding.len);
			}
			break;
		case QUIC_FT_ACK:
			if (!qc_parse_ack_frm(&frm, ctx, qel, &pos, end))
				goto err;

			tasklet_wakeup(ctx->wait_event.tasklet);
			break;
		case QUIC_FT_PING:
			qpkt->flags |= QUIC_FL_RX_PACKET_ACK_ELICITING;
			break;
		case QUIC_FT_CONNECTION_CLOSE:
			break;
		default:
			goto err;
		}
	}

	TRACE_LEAVE(QUIC_EV_CONN_PRSHPKT, ctx->conn);
	return 1;

 err:
	TRACE_DEVEL("leaving in error", QUIC_EV_CONN_PRSHPKT, ctx->conn);
	return 0;
}

/*
 * Prepare as much as possible handshakes packets to retransmit for the QUIC
 * connection wich <ctx> as I/O handler context.
 * Returns 1 if succeeded, or 0 if something wrong happened.
 */
static int qc_prep_hdshk_rpkts(struct quic_conn_ctx *ctx)
{
	struct quic_conn *qc;
	enum quic_tls_enc_level tel, next_tel;
	struct quic_enc_level *qel;
	struct eb_root *frms;
	struct eb64_node *node;
	int reuse_wbuf;

	TRACE_ENTER(QUIC_EV_CONN_PHRPKTS, ctx->conn);
	qc = ctx->conn->quic_conn;
	if (!quic_get_tls_enc_levels(&tel, &next_tel, ctx->state)) {
		TRACE_DEVEL("unknown enc. levels", QUIC_EV_CONN_PHRPKTS, ctx->conn);
		goto err;
	}

	reuse_wbuf = 0;
	qel = &qc->enc_levels[tel];
	frms = &qel->tx.crypto.retransmit_frms;
	node = eb64_first(frms);
	while (node) {
		uint64_t offset;
		struct q_buf *wbuf;
		struct quic_tx_crypto_frm *frm;

		wbuf = q_wbuf(qc);
		frm = eb64_entry(&node->node, struct quic_tx_crypto_frm, pn);
		while (frm->len) {
			ssize_t ret;

			if (!q_buf_empty(wbuf) && !reuse_wbuf)
				goto out;

			reuse_wbuf = 0;
			offset = frm->offset;
			ret = qc_build_hdshk_pkt(wbuf, qc,
			                         quic_tls_level_pkt_type(tel),
			                         &frm->offset, frm->len, qel);
			switch (ret) {
			case -2:
				goto err;
			case -1:
				wbuf = q_next_wbuf(qc);
				continue;
			case 0:
				goto out;
			default:
				frm->len -= frm->offset - offset;
				if (frm->len)
					wbuf = q_next_wbuf(qc);
			}
		}
		node = eb64_next(node);
		eb64_delete(&frm->pn);
		pool_free(pool_head_quic_tx_crypto_frm, frm);
		if (!node && tel == QUIC_TLS_ENC_LEVEL_INITIAL) {
			/* Have a look at the next level. */
			tel = next_tel;
			qel = &qc->enc_levels[tel];
			frms = &qel->tx.crypto.retransmit_frms;
			node =  eb64_first(frms);
			if (!node) {
				/* If there is no more data for the next level, let's
				 * consume a buffer.
				 */
				wbuf = q_next_wbuf(qc);
			}
			else {
				/* Try to reuse the same buffer. */
				reuse_wbuf = 1;
			}
		}
		else {
			wbuf = q_next_wbuf(qc);
		}
	}

 out:
	TRACE_LEAVE(QUIC_EV_CONN_PHRPKTS, ctx->conn);
	if (eb_is_empty(frms))
		qc->retransmit = 0;

	return 1;

 err:
	TRACE_DEVEL("leaving in error", QUIC_EV_CONN_PHRPKTS, ctx->conn);
	return 0;
}

/*
 * Prepare as much as possible handshake packets for the QUIC connection
 * with <ctx> as I/O handler context.
 * Returns 1 if succeeded, or 0 if something wrong happened.
 */
static int qc_prep_hdshk_pkts(struct quic_conn_ctx *ctx)
{
	struct quic_conn *qc;
	enum quic_tls_enc_level tel, next_tel;
	struct quic_enc_level *qel;
	struct q_buf *wbuf;
	/* A boolean to flag <wbuf> as reusable, even if not empty. */
	int reuse_wbuf;

	TRACE_ENTER(QUIC_EV_CONN_PHPKTS, ctx->conn);
	qc = ctx->conn->quic_conn;
	if (!quic_get_tls_enc_levels(&tel, &next_tel, ctx->state)) {
		TRACE_DEVEL("unknown enc. levels", QUIC_EV_CONN_PHPKTS, ctx->conn);
		goto err;
	}

	reuse_wbuf = 0;
	wbuf = q_wbuf(qc);
	qel = &qc->enc_levels[tel];
	/*
	 * When entering this function, the writter buffer must be empty.
	 * Most of the time it points to the reader buffer.
	 */
	while ((q_buf_empty(wbuf) || reuse_wbuf)) {
		ssize_t ret;

		if (c_buf_consumed(qel) && !(qel->pktns->flags & QUIC_FL_PKTNS_ACK_REQUIRED))
			break;

		reuse_wbuf = 0;
		ret = qc_build_hdshk_pkt(wbuf, qc,
		                         quic_tls_level_pkt_type(tel),
		                         &qel->tx.crypto.offset,
		                         c_buf_remain(qel, qel->tx.crypto.offset), qel);
		switch (ret) {
		case -2:
			goto err;
		case -1:
			/* Not enough room in <wbuf>. */
			wbuf = q_next_wbuf(qc);
			continue;
		case 0:
			goto out;
		default:
			/* Special case for Initial packets: when they have all
			 * been sent, select the next level.
			 */
			if (c_buf_consumed(qel) && tel == QUIC_TLS_ENC_LEVEL_INITIAL) {
				tel = next_tel;
				qel = &qc->enc_levels[tel];
				if (c_buf_consumed(qel)) {
					/* If there is no more data for the next level, let's
					 * consume a buffer. This is the case for a client
					 * which sends only one Initial packet, then wait
					 * for additional CRYPTO data from the server to enter the
					 * next level.
					 */
					wbuf = q_next_wbuf(qc);
				}
				else {
					/* Let's try to reuse this buffer. */
					reuse_wbuf = 1;
				}
			}
			else {
				wbuf = q_next_wbuf(qc);
			}
		}
	}

 out:
	TRACE_LEAVE(QUIC_EV_CONN_PHPKTS, ctx->conn);
	return 1;

 err:
	TRACE_DEVEL("leaving in error", QUIC_EV_CONN_PHPKTS, ctx->conn);
	return 0;
}

/*
 * Send the QUIC packets which have been prepared for QUIC connections
 * with <ctx> as I/O handler context.
 */
static int qc_send_ppkts(struct quic_conn_ctx *ctx)
{
	struct quic_conn *qc;
	struct buffer tmpbuf = { };
	struct q_buf *rbuf;

	qc = ctx->conn->quic_conn;
	for (rbuf = q_rbuf(qc); !q_buf_empty(rbuf) ; rbuf = q_next_rbuf(qc)) {
		tmpbuf.area = (char *)rbuf->area;
		tmpbuf.size = tmpbuf.data = rbuf->data;

	    if (ctx->xprt->snd_buf(qc->conn, qc->conn->xprt_ctx,
	                           &tmpbuf, tmpbuf.data, 0) <= 0)
		    break;

	    /* Reset this buffer to make it available for the next packet to prepare. */
	    q_buf_reset(rbuf);
	}

	return 1;
}

/*
 * Build all the frames which must be sent just after the handshake have succeeded.
 * This is essentially NEW_CONNECTION_ID frames. A QUIC server must also send
 * a HANDSHAKE_DONE frame.
 * Return 1 if succeeded, 0 if not.
 */
static int quic_build_post_handshake_frames(struct quic_conn *conn)
{
	int i;
	struct quic_frame *frm;

	/* Only servers must send a HANDSHAKE_DONE frame. */
	if (!objt_server(conn->conn->target)) {
		frm = pool_alloc(pool_head_quic_frame);
		frm->type = QUIC_FT_HANDSHAKE_DONE;
		LIST_ADDQ(&conn->tx.frms_to_send, &frm->list);
	}

	for (i = 1; i < conn->rx_tps.active_connection_id_limit; i++) {
		struct quic_connection_id *cid;

		frm = pool_alloc(pool_head_quic_frame);
		memset(frm, 0, sizeof *frm);
		cid = new_quic_connection_id(&conn->cids, i);
		if (!frm || !cid)
			goto err;

		quic_connection_id_to_frm_cpy(frm, cid);
		LIST_ADDQ(&conn->tx.frms_to_send, &frm->list);
	}

    return 1;

 err:
	free_quic_conn_cids(conn);
	return 0;
}

/* Deallocate <l> list of ACK ranges. */
void free_ack_range_list(struct list *l)
{
	struct quic_ack_range *curr, *next;

	list_for_each_entry_safe(curr, next, l, list) {
		LIST_DEL(&curr->list);
		free(curr);
	}
}

/*
 * Update <l> list of ACK ranges with <pn> new packet number.
 */
int quic_update_ack_ranges_list(struct quic_ack_ranges *ack_ranges, int64_t pn)
{
	struct list *l = &ack_ranges->list;
	size_t *sz = &ack_ranges->sz;

	struct quic_ack_range *curr, *prev, *next;
	struct quic_ack_range *new_sack;

	prev = NULL;

	if (LIST_ISEMPTY(l)) {
		new_sack = pool_alloc(pool_head_quic_ack_range);
		new_sack->first = new_sack->last = pn;
		LIST_ADD(l, &new_sack->list);
		++*sz;
		return 1;
	}

	list_for_each_entry_safe(curr, next, l, list) {
		/* Already existing packet number */
		if (pn >= curr->first && pn <= curr->last)
			break;

		if (pn > curr->last + 1) {
			new_sack = pool_alloc(pool_head_quic_ack_range);
			new_sack->first = new_sack->last = pn;
			if (prev) {
				/* Insert <new_sack> between <prev> and <curr> */
				new_sack->list.n = &curr->list;
				new_sack->list.p = &prev->list;
				prev->list.n = &new_sack->list;
				curr->list.p = &new_sack->list;
			}
			else {
				LIST_ADD(l, &new_sack->list);
			}
			++*sz;
			break;
		}
		else if (curr->last + 1 == pn) {
			curr->last = pn;
			break;
		}
		else if (curr->first == pn + 1) {
			if (&next->list != l && pn == next->last + 1) {
				next->last = curr->last;
				LIST_DEL(&curr->list);
				free(curr);
				--*sz;
			}
			else {
				curr->first = pn;
			}
			break;
		}
		else if (&next->list == l) {
			new_sack = pool_alloc(pool_head_quic_ack_range);
			new_sack->first = new_sack->last = pn;
			LIST_ADDQ(l, &new_sack->list);
			++*sz;
			break;
		}
		prev = curr;
	}

	return 1;
}

/*
 * Parse all the frames of <qpkt> QUIC packet from <ctx> I/O connection handler context
 * at <qel> encryption level.
 * Return 1 if succeeded, 0 if not.
 */
int qc_parse_apkt(struct quic_rx_packet *qpkt, struct quic_conn_ctx *ctx)
{
	struct quic_frame frm;
	const unsigned char *pos, *end;
	struct quic_enc_level *qel;

	TRACE_ENTER(QUIC_EV_CONN_PRSAPKT, ctx->conn);
	qel = &ctx->conn->quic_conn->enc_levels[QUIC_TLS_ENC_LEVEL_APP];
	/* Skip the AAD */
	pos = qpkt->data + qpkt->aad_len;
	end = qpkt->data + qpkt->len;

	while (pos < end) {
		if (!qc_parse_frm(&frm, &pos, end))
			goto err;

		switch (frm.type) {
			case QUIC_FT_CRYPTO:
				qpkt->flags |= QUIC_FL_RX_PACKET_ACK_ELICITING;
				break;

			case QUIC_FT_PADDING:
				/* This frame must be the last found in the packet. */
				if (pos != end) {
					QDPRINTF("Wrong frame! (%ld len: %lu)\n", end - pos, frm.padding.len);
					goto err;
				}
				break;

			case QUIC_FT_ACK:
				if (!qc_parse_ack_frm(&frm, ctx, qel, &pos, end))
					goto err;

				break;

			case QUIC_FT_PING:
				qpkt->flags |= QUIC_FL_RX_PACKET_ACK_ELICITING;
				break;

			case QUIC_FT_CONNECTION_CLOSE:
			case QUIC_FT_CONNECTION_CLOSE_APP:
				break;
			case QUIC_FT_NEW_CONNECTION_ID:
			case QUIC_FT_STREAM_A:
			case QUIC_FT_STREAM_B:
				qpkt->flags |= QUIC_FL_RX_PACKET_ACK_ELICITING;
				break;
			default:
				goto err;
		}
	}

	TRACE_LEAVE(QUIC_EV_CONN_PRSAPKT, ctx->conn);
	return 1;

 err:
	TRACE_DEVEL("leaving in error", QUIC_EV_CONN_PRSAPKT, ctx->conn);
	return 0;
}

/*
 * Remove the header protection of packets at <el> encryption level.
 * Always succeeds.
 */
static inline void qc_rm_hp_pkts(struct quic_enc_level *el, struct quic_conn_ctx *ctx)
{
	struct quic_tls_ctx *tls_ctx;
	struct quic_rx_packet *pqpkt, *qqpkt;

	TRACE_ENTER(QUIC_EV_CONN_ELRMHP, ctx->conn);
	tls_ctx = &el->tls_ctx;
	list_for_each_entry_safe(pqpkt, qqpkt, &el->rx.pqpkts, list) {
		if (!qc_do_rm_hp(pqpkt, tls_ctx, el->pktns->rx.largest_pn,
		                 pqpkt->data + pqpkt->pn_offset,
		                 pqpkt->data, pqpkt->data + pqpkt->len)) {
			TRACE_PROTO("hp removing error", QUIC_EV_CONN_ELRMHP, ctx->conn);
			/* XXX TO DO XXX */
		}
		else {
			/* Store the packet into the tree of packets to decrypt. */
			pqpkt->pn_node.key = pqpkt->pn;
			eb64_insert(&el->rx.qpkts, &pqpkt->pn_node);
			/* The AAD includes the packet number field */
			pqpkt->aad_len = pqpkt->pn_offset + pqpkt->pnl;
			TRACE_PROTO("hp removed", QUIC_EV_CONN_ELRMHP, ctx->conn, pqpkt);
		}
		LIST_DEL(&pqpkt->list);
	}

  out:
	TRACE_LEAVE(QUIC_EV_CONN_ELRMHP, ctx->conn);
}

/*
 * Process all the packets at <el> encryption level.
 * Return 1 if succeeded, 0 if not.
 */
static inline int qc_treat_rx_pkts(struct quic_enc_level *el, struct quic_conn_ctx *ctx)
{
	struct quic_tls_ctx *tls_ctx;
	struct eb64_node *node;

	TRACE_ENTER(QUIC_EV_CONN_ELRXPKTS, ctx->conn);
	tls_ctx = &el->tls_ctx;
	node = eb64_first(&el->rx.qpkts);
	while (node) {
		struct quic_rx_packet *pkt;

		pkt = eb64_entry(&node->node, struct quic_rx_packet, pn_node);
		if (!(pkt->flags & QUIC_FL_RX_PACKET_OUT_OF_ORDER)) {
			int drop;

			drop = 0;
		    if (!qc_pkt_decrypt(pkt, tls_ctx)) {
				/* Drop the packet */
				TRACE_PROTO("packet decryption failed -> dropped",
				            QUIC_EV_CONN_ELRXPKTS, ctx->conn, pkt);
				node = eb64_next(node);
				eb64_delete(&pkt->pn_node);
				pool_free(pool_head_quic_rx_packet, pkt);
				continue;
			}

			if ((pkt->long_header && !qc_parse_hdshk_pkt(pkt, ctx, el)) ||
			    (!pkt->long_header && !qc_parse_apkt(pkt, ctx)))
				drop = 1;

			if (drop) {
				/* Drop the packet */
				TRACE_PROTO("packet parsing failed -> dropped",
				            QUIC_EV_CONN_ELRXPKTS, ctx->conn, pkt);
				node = eb64_next(node);
				eb64_delete(&pkt->pn_node);
				pool_free(pool_head_quic_rx_packet, pkt);
				continue;
			}

			if (pkt->flags & QUIC_FL_RX_PACKET_ACK_ELICITING) {
				el->pktns->rx.nb_ack_eliciting++;
				if (!(el->pktns->rx.nb_ack_eliciting & 1))
					el->pktns->flags |= QUIC_FL_PKTNS_ACK_REQUIRED;
			}

			/* Update the largest packet number. */
			if (pkt->pn > el->pktns->rx.largest_pn)
				el->pktns->rx.largest_pn = pkt->pn;

			/* Update the list of ranges to acknowledge. */
			quic_update_ack_ranges_list(&el->pktns->rx.ack_ranges, pkt->pn);
		}
		node = eb64_next(node);
		if (pkt->crypto.len) {
			if (pkt->crypto.offset == el->rx.crypto.offset) {
				HEXDUMP(pkt->crypto.data, pkt->crypto.len, "CRYPTO frame:\n");
				if (SSL_provide_quic_data(ctx->ssl, SSL_quic_read_level(ctx->ssl),
										  pkt->crypto.data, pkt->crypto.len) != 1) {
					TRACE_PROTO("SSL_provide_quic_data() error",
					            QUIC_EV_CONN_ELRXPKTS, ctx->conn, pkt, ctx->ssl);
					goto err;
				}
				TRACE_PROTO("in order CRYPTO data ",
				            QUIC_EV_CONN_ELRXPKTS, ctx->conn, pkt);
				el->rx.crypto.offset += pkt->crypto.len;
				eb64_delete(&pkt->pn_node);
				pool_free(pool_head_quic_rx_packet, pkt);
			}
		}
		else {
			eb64_delete(&pkt->pn_node);
			pool_free(pool_head_quic_rx_packet, pkt);
		}
	}

	TRACE_LEAVE(QUIC_EV_CONN_ELRXPKTS, ctx->conn);
	return 1;

 err:
	TRACE_DEVEL("leaving in error", QUIC_EV_CONN_ELRXPKTS, ctx->conn);
	return 0;
}

/*
 * Called during handshakes to parse and build Initial and Handshake packets for QUIC
 * connections with <ctx> as I/O handler context.
 * Returns 1 if succeeded, 0 if not.
 */
static int qc_do_hdshk(struct quic_conn_ctx *ctx)
{
	struct quic_conn *quic_conn;
	enum quic_tls_enc_level tel, next_tel;
	struct quic_enc_level *enc_level, *next_enc_level;
	struct quic_tls_ctx *tls_ctx;
	int ret;

	TRACE_ENTER(QUIC_EV_CONN_HDSHK, ctx->conn, &ctx->state);

	quic_conn = ctx->conn->quic_conn;
	if (!quic_get_tls_enc_levels(&tel, &next_tel, ctx->state))
		goto err;

	enc_level = &quic_conn->enc_levels[tel];
	next_enc_level = &quic_conn->enc_levels[next_tel];

 next_level:
	tls_ctx = &enc_level->tls_ctx;

	/* If the header protection key for this level has been derived,
	 * remove the packet header protections.
	 */
	if (!LIST_ISEMPTY(&enc_level->rx.pqpkts) &&
	    (tls_ctx->rx.flags & QUIC_FL_TLS_SECRETS_SET))
		qc_rm_hp_pkts(enc_level, ctx);

	if (!eb_is_empty(&enc_level->rx.qpkts) &&
		!qc_treat_rx_pkts(enc_level, ctx))
		goto err;

	if (quic_conn->retransmit && !qc_prep_hdshk_rpkts(ctx))
		goto err;

	if (!quic_conn->retransmit && !qc_prep_hdshk_pkts(ctx))
		goto err;

	if (!qc_send_ppkts(ctx))
		goto err;

	/*
	 * Check if there is something to do for the next level.
	 */
	if ((next_enc_level->tls_ctx.rx.flags & QUIC_FL_TLS_SECRETS_SET) &&
	    (!LIST_ISEMPTY(&next_enc_level->rx.pqpkts) || !eb_is_empty(&next_enc_level->rx.qpkts))) {
		enc_level = next_enc_level;
		if (ctx->state == QUIC_HS_ST_CLIENT_INITIAL)
			ctx->state = QUIC_HS_ST_CLIENT_HANDSHAKE;
		goto next_level;
	}

	ret = SSL_do_handshake(ctx->ssl);
	if (ret != 1) {
		ret = SSL_get_error(ctx->ssl, ret);
		if (ret == SSL_ERROR_WANT_READ || ret == SSL_ERROR_WANT_WRITE)
			goto out;

		TRACE_DEVEL("SSL handshake error", QUIC_EV_CONN_HDSHK, ctx->conn, &ctx->state, &ret);
		goto err;
	}

	TRACE_DEVEL("SSL handshake OK", QUIC_EV_CONN_HDSHK, ctx->conn, &ctx->state);

	if (ctx->state == QUIC_HS_ST_SERVER_HANDSHAKE ||
	    ctx->state == QUIC_HS_ST_CLIENT_HANDSHAKE)
		ctx->conn->flags &= ~CO_FL_SSL_WAIT_HS;

	ret = SSL_process_quic_post_handshake(ctx->ssl);
	if (ret != 1) {
		TRACE_DEVEL("SSL post handshake error",
		            QUIC_EV_CONN_HDSHK, ctx->conn, &ctx->state);
		goto err;
	}

	TRACE_DEVEL("SSL post handshake succeeded",
	            QUIC_EV_CONN_HDSHK, ctx->conn, &ctx->state);

	if (!quic_build_post_handshake_frames(quic_conn) ||
	    !qc_prep_phdshk_pkts(quic_conn) ||
	    !qc_send_ppkts(ctx))
		goto err;

 out:
	TRACE_LEAVE(QUIC_EV_CONN_HDSHK, ctx->conn, &ctx->state);
	return 1;

 err:
	TRACE_DEVEL("leaving in error", QUIC_EV_CONN_HDSHK, ctx->conn, &ctx->state);
	return 0;
}

/*
 * Called after successful handshake to parse Application level encrypted
 * packets for QUIC connection with <ctx> as I/O handler context.
 * Return 1 if succeeded, 0 if failed.
 */
static int quic_treat_packets(struct quic_conn_ctx *ctx)
{
	struct quic_conn *quic_conn;
	struct quic_enc_level *enc_level;
	struct quic_tls_ctx *tls_ctx;
	struct eb_root *rx_qpkts;
	struct quic_rx_packet *qpkt;
	struct eb64_node *qpkt_node;

	quic_conn = ctx->conn->quic_conn;
	enc_level = &quic_conn->enc_levels[QUIC_TLS_ENC_LEVEL_APP];
	if (eb_is_empty(&enc_level->rx.qpkts)) {
		QDPRINTF("empty tree for APP level encryption\n");
		enc_level = &quic_conn->enc_levels[QUIC_TLS_ENC_LEVEL_HANDSHAKE];
	}
	tls_ctx = &enc_level->tls_ctx;
	rx_qpkts = &enc_level->rx.qpkts;

	qpkt_node = eb64_first(rx_qpkts);
	while (qpkt_node) {
		qpkt = eb64_entry(&qpkt_node->node, struct quic_rx_packet, pn_node);
		if (!qc_pkt_decrypt(qpkt, tls_ctx)) {
			/* Drop the packet */
			QDPRINTF( "packet #%lu long? %d dropped\n", qpkt->pn, !!qpkt->long_header);
			qpkt_node = eb64_next(qpkt_node);
			eb64_delete(&qpkt->pn_node);
			pool_free(pool_head_quic_rx_packet, qpkt);
			continue;
		}

		if (!qc_parse_apkt(qpkt, ctx)) {
			QDPRINTF( "Could not parse the packet frames\n");
			/* Drop the packet */
			QDPRINTF( "packet #%lu long? %d dropped\n", qpkt->pn, !!qpkt->long_header);
			qpkt_node = eb64_next(qpkt_node);
			eb64_delete(&qpkt->pn_node);
			pool_free(pool_head_quic_rx_packet, qpkt);
			continue;
		}

		if (qpkt->flags & QUIC_FL_RX_PACKET_ACK_ELICITING) {
			enc_level->pktns->rx.nb_ack_eliciting++;
			if (!(enc_level->pktns->rx.nb_ack_eliciting & 1))
				enc_level->pktns->flags |= QUIC_FL_PKTNS_ACK_REQUIRED;
		}

		/* Update the list of ranges to acknowledge. */
		quic_update_ack_ranges_list(&enc_level->pktns->rx.ack_ranges, qpkt->pn);

		qpkt_node = eb64_next(qpkt_node);
		eb64_delete(&qpkt->pn_node);
		pool_free(pool_head_quic_rx_packet, qpkt);
	}

	return 1;
}

/* QUIC connection packet handler task. */
static struct task *quic_conn_io_cb(struct task *t, void *context, unsigned short state)
{
	struct quic_conn_ctx *ctx = context;

	QDPRINTF("%s: tid: %u\n", __func__, tid);
	if (ctx->conn->flags & CO_FL_SSL_WAIT_HS) {
		if (!qc_do_hdshk(ctx))
			QDPRINTF("%s SSL handshake error\n", __func__);
	}
	else {
		quic_treat_packets(ctx);
	}

	return NULL;
}

/* We can't have an underlying XPRT, so just return -1 to signify failure */
static int quic_conn_remove_xprt(struct connection *conn, void *xprt_ctx, void *toremove_ctx, const struct xprt_ops *newops, void *newctx)
{
	QDPRINTF("%s\n", __func__);
	/* This is the lowest xprt we can have, so if we get there we didn't
	 * find the xprt we wanted to remove, that's a bug
	 */
	BUG_ON(1);
	return -1;
}

/*
 * Allocate a new QUIC connection and return it if succeeded, NULL if not.
 */
static struct quic_conn *quic_new_conn(uint32_t version)
{
	struct quic_conn *quic_conn;

	quic_conn = pool_alloc(pool_head_quic_conn);
	if (quic_conn) {
		memset(quic_conn, 0, sizeof *quic_conn);
		quic_conn->version = version;
	}

	return quic_conn;
}

/*
 * Unitialize <qel> QUIC encryption level.
 * Never fails.
 */
static void quic_conn_enc_level_uninit(struct quic_enc_level *qel)
{
	int i;

	for (i = 0; i < qel->tx.crypto.nb_buf; i++) {
		if (qel->tx.crypto.bufs[i]) {
			pool_free(pool_head_quic_crypto_buf, qel->tx.crypto.bufs[i]);
			qel->tx.crypto.bufs[i] = NULL;
		}
	}
	free(qel->tx.crypto.bufs);
	qel->tx.crypto.bufs = NULL;
}

/*
 * Initialize <qel> QUIC TLS encryption level, allocating everything needed.
 * Returns 1 if succeeded, 0 if not.
 */
static int quic_conn_enc_level_init(struct quic_enc_level *qel)
{
	qel->tls_ctx.rx.aead = qel->tls_ctx.tx.aead = NULL;
	qel->tls_ctx.rx.md   = qel->tls_ctx.tx.md = NULL;
	qel->tls_ctx.rx.hp   = qel->tls_ctx.tx.hp = NULL;
	qel->tls_ctx.rx.flags = 0;
	qel->tls_ctx.tx.flags = 0;

	qel->rx.qpkts = EB_ROOT;
	LIST_INIT(&qel->rx.pqpkts);

	/* Allocate only one buffer. */
	qel->tx.crypto.bufs = malloc(sizeof *qel->tx.crypto.bufs);
	if (!qel->tx.crypto.bufs)
		goto err;

	qel->tx.crypto.bufs[0] = pool_alloc(pool_head_quic_crypto_buf);
	if (!qel->tx.crypto.bufs[0]) {
		QDPRINTF("%s: could not allocated any crypto buffer\n", __func__);
		goto err;
	}

	qel->tx.crypto.bufs[0]->sz = 0;
	qel->tx.crypto.nb_buf = 1;

	qel->tx.crypto.sz = 0;
	qel->tx.crypto.offset = 0;
	qel->tx.crypto.frms = EB_ROOT;
	qel->tx.crypto.retransmit_frms = EB_ROOT;

	return 1;

 err:
	free(qel->tx.crypto.bufs);
	qel->tx.crypto.bufs = NULL;
	return 0;
}

/*
 * Release the memory allocated for <buf> array of buffers, with <nb> as size.
 * Never fails.
 */
static inline void free_quic_conn_tx_bufs(struct q_buf **bufs, size_t nb)
{
	struct q_buf **p;

	if (!bufs)
		return;

	p = bufs;
	while (--nb) {
		if (!*p) {
			p++;
			continue;
		}
		free((*p)->area);
		(*p)->area = NULL;
		free(*p);
		*p = NULL;
		p++;
	}
	free(bufs);
}

/*
 * Allocate an array or <nb> buffers of <sz> bytes each.
 * Return this array if succeeded, NULL if failed.
 */
static inline struct q_buf **quic_conn_tx_bufs_alloc(size_t nb, size_t sz)
{
	int i;
	struct q_buf **bufs, **p;

	bufs = calloc(nb, sizeof *bufs);
	if (!bufs)
		return NULL;

	i = 0;
	p = bufs;
	while (i++ < nb) {
		*p = calloc(1, sizeof **p);
		if (!*p)
			goto err;

		(*p)->area = malloc(sz);
		if (!(*p)->area)
		    goto err;

		(*p)->pos = (*p)->area;
		(*p)->end = (*p)->area + sz;
		(*p)->data = 0;
		p++;
	}

	return bufs;

 err:
	free_quic_conn_tx_bufs(bufs, nb);
	return NULL;
}

/*
 * Release all the memory allocated for <conn> QUIC connection. */
static void quic_conn_free(struct quic_conn *conn)
{
	int i;

	free_quic_conn_cids(conn);
	for (i = 0; i < QUIC_TLS_ENC_LEVEL_MAX; i++)
		quic_conn_enc_level_uninit(&conn->enc_levels[i]);
	free_quic_conn_tx_bufs(conn->tx.bufs, conn->tx.nb_buf);
	pool_free(pool_head_quic_conn, conn);
}

/*
 * Initialize <conn> QUIC connection with <quic_initial_clients> as root of QUIC
 * connections used to identify the first Initial packets of client connecting
 * to listeners. This parameter must be NULL for QUIC connections attached
 * to listeners. <dcid> is the destination connection ID with <dcid_len> as length.
 * <scid> is the source connection ID with <scid_len> as length.
 * Returns 1 if succeeded, 0 if not.
 */
static int qc_new_conn_init(struct quic_conn *conn,
                            struct eb_root *quic_initial_clients,
                            struct eb_root *quic_clients,
                            unsigned char *dcid, size_t dcid_len,
                            unsigned char *scid, size_t scid_len)
{
	int i;
	/* Initial CID. */
	struct quic_connection_id *icid;

	TRACE_ENTER(QUIC_EV_CONN_INIT, conn->conn);
	conn->cids = EB_ROOT;
	QDPRINTF("%s: new quic_conn @%p\n", __func__, conn);
	/* QUIC Server (or listener). */
	if (objt_listener(conn->conn->target)) {
		/* Copy the initial DCID. */
		conn->odcid.len = dcid_len;
		if (conn->odcid.len)
			memcpy(conn->odcid.data, dcid, dcid_len);

		/* Copy the SCID as our DCID for this connection. */
		if (scid_len)
			memcpy(conn->dcid.data, scid, scid_len);
		conn->dcid.len = scid_len;
	}
	/* QUIC Client (outoging connection to servers) */
	else {
		if (dcid_len)
			memcpy(conn->dcid.data, dcid, dcid_len);
		conn->dcid.len = dcid_len;
	}

	/* Initialize the output buffer */
	conn->obuf.pos = conn->obuf.data;

	icid = new_quic_connection_id(&conn->cids, 0);
	if (!icid)
		return 0;

	/* Select our SCID which is the first CID with 0 as sequence number. */
	conn->scid = icid->cid;

	/* Insert the DCID the QUIC client has choosen (only for listeners) */
	if (objt_listener(conn->conn->target))
		ebmb_insert(quic_initial_clients, &conn->odcid_node, conn->odcid.len);

	/* Insert our SCID, the connection ID for the QUIC client. */
	ebmb_insert(quic_clients, &conn->scid_node, conn->scid.len);

	/* Packet number spaces initialization. */
	for (i = 0; i < QUIC_TLS_PKTNS_MAX; i++) {
		quic_pktns_init(&conn->pktns[i]);
	}
	/* QUIC encryption level context initialization. */
	for (i = 0; i < QUIC_TLS_ENC_LEVEL_MAX; i++) {
		if (!quic_conn_enc_level_init(&conn->enc_levels[i]))
			goto err;
		/* Initialize the packet number space. */
		conn->enc_levels[i].pktns = &conn->pktns[quic_tls_pktns(i)];
	}

	LIST_INIT(&conn->tx.frms_to_send);
	conn->tx.bufs = quic_conn_tx_bufs_alloc(QUIC_CONN_TX_BUFS_NB, QUIC_CONN_TX_BUF_SZ);
	if (!conn->tx.bufs)
		goto err;

	conn->tx.nb_buf = QUIC_CONN_TX_BUFS_NB;
	conn->tx.wbuf = conn->tx.rbuf = 0;

	conn->retransmit = 0;
	conn->crypto_in_flight = 0;
	TRACE_LEAVE(QUIC_EV_CONN_INIT, conn->conn);

	return 1;

 err:
	TRACE_DEVEL("leaving in error", QUIC_EV_CONN_INIT, conn->conn);
	quic_conn_free(conn);
	return 0;
}

/*
 * Derive the initial secrets with <ctx> as QUIC TLS context which is the
 * cryptographic context for the first encryption level (Initial) from
 * <cid> connection ID with <cidlen> as length (in bytes) for a server or not
 * depending on <server> boolean value.
 * Return 1 if succeeded or 0 if not.
 */
static int qc_new_isecs(struct connection *conn,
                        const unsigned char *cid, size_t cidlen, int server)
{
	unsigned char initial_secret[32];
	/* Initial secret to be derived for incoming packets */
	unsigned char rx_init_sec[32];
	/* Initial secret to be derived for outgoing packets */
	unsigned char tx_init_sec[32];
	struct quic_tls_secrets *rx_ctx, *tx_ctx;
	struct quic_tls_ctx *ctx;

	TRACE_ENTER(QUIC_EV_CONN_ISEC, conn);
	ctx = &conn->quic_conn->enc_levels[QUIC_TLS_ENC_LEVEL_INITIAL].tls_ctx;
	quic_initial_tls_ctx_init(ctx);
	if (!quic_derive_initial_secret(ctx->rx.md,
	                                initial_secret, sizeof initial_secret,
	                                cid, cidlen))
		goto err;

	if (!quic_tls_derive_initial_secrets(ctx->rx.md,
	                                     rx_init_sec, sizeof rx_init_sec,
	                                     tx_init_sec, sizeof tx_init_sec,
	                                     initial_secret, sizeof initial_secret, server))
		goto err;

	rx_ctx = &ctx->rx;
	tx_ctx = &ctx->tx;
	if (!quic_tls_derive_keys(ctx->rx.aead, ctx->rx.hp, ctx->rx.md,
	                          rx_ctx->key, sizeof rx_ctx->key,
	                          rx_ctx->iv, sizeof rx_ctx->iv,
	                          rx_ctx->hp_key, sizeof rx_ctx->hp_key,
	                          rx_init_sec, sizeof rx_init_sec))
		goto err;

	rx_ctx->flags |= QUIC_FL_TLS_SECRETS_SET;
	if (!quic_tls_derive_keys(ctx->tx.aead, ctx->tx.hp, ctx->tx.md,
	                          tx_ctx->key, sizeof tx_ctx->key,
	                          tx_ctx->iv, sizeof tx_ctx->iv,
	                          tx_ctx->hp_key, sizeof tx_ctx->hp_key,
	                          tx_init_sec, sizeof tx_init_sec))
		goto err;

	tx_ctx->flags |= QUIC_FL_TLS_SECRETS_SET;
	TRACE_LEAVE(QUIC_EV_CONN_ISEC, conn);

	return 1;

 err:
	TRACE_DEVEL("leaving in error", QUIC_EV_CONN_EISEC, conn);
	return 0;
}

/*
 * Initialize a QUIC connection (quic_conn struct) to be attached to <conn>
 * connection with <xprt_ctx> as address of the xprt context.
 * Returns 1 if succeeded, 0 if not.
 */
static int qc_conn_init(struct connection *conn, void **xprt_ctx)
{
	struct quic_conn_ctx *ctx;

	TRACE_ENTER(QUIC_EV_CONN_NEW, conn);

	if (*xprt_ctx)
		return 0;

	if (!conn_ctrl_ready(conn))
		return 0;

	ctx = pool_alloc(pool_head_quic_conn_ctx);
	if (!ctx) {
		conn->err_code = CO_ER_SYS_MEMLIM;
		goto err;
	}

	ctx->wait_event.tasklet = tasklet_new();
	if (!ctx->wait_event.tasklet) {
		conn->err_code = CO_ER_SYS_MEMLIM;
		goto err;
	}

	ctx->wait_event.tasklet->process = quic_conn_io_cb;
	ctx->wait_event.tasklet->context = ctx;
	ctx->wait_event.events = 0;
	ctx->conn = conn;
	ctx->subs = NULL;
	ctx->xprt_ctx = NULL;

	ctx->xprt = xprt_get(XPRT_QUIC);
	if (objt_server(conn->target)) {
		/* Server */
		struct server *srv = __objt_server(conn->target);
		unsigned char dcid[QUIC_CID_LEN];
		struct quic_conn *quic_conn;

		if (RAND_bytes(dcid, sizeof dcid) != 1)
			goto err;

		conn->quic_conn = quic_new_conn(QUIC_PROTOCOL_VERSION_DRAFT_28);
		if (!conn->quic_conn)
			goto err;

		quic_conn = conn->quic_conn;
		quic_conn->conn = conn;
		if (!qc_new_conn_init(quic_conn, NULL, &srv->cids,
		                      dcid, sizeof dcid, NULL, 0))
			goto err;

		if (!qc_new_isecs(conn, dcid, sizeof dcid, 0)) {
			QDPRINTF("Could not derive initial secrets\n");
			goto err;
		}

		ctx->state = QUIC_HS_ST_CLIENT_INITIAL;
		if (ssl_bio_and_sess_init(conn, srv->ssl_ctx.ctx,
		                          &ctx->ssl, &ctx->bio, ha_quic_meth, ctx) == -1) {
			QDPRINTF("Could not initiliaze SSL ctx\n");
			goto err;
		}

		quic_conn->params = srv->quic_params;
		/* Copy the initial source connection ID. */
		quic_cid_cpy(&quic_conn->params.initial_source_connection_id, &quic_conn->scid);
		quic_conn->enc_params_len =
			quic_transport_params_encode(quic_conn->enc_params,
			                             quic_conn->enc_params + sizeof quic_conn->enc_params,
			                             &quic_conn->params, 0);
		if (!quic_conn->enc_params_len) {
			QDPRINTF("QUIC transport parameters encoding failed");
			goto err;
		}
		SSL_set_quic_transport_params(ctx->ssl, quic_conn->enc_params, quic_conn->enc_params_len);
		SSL_set_connect_state(ctx->ssl);
	}
	else if (objt_listener(conn->target)) {
		/* Listener */
		struct bind_conf *bc = __objt_listener(conn->target)->bind_conf;

		ctx->state = QUIC_HS_ST_SERVER_INITIAL;

		if (ssl_bio_and_sess_init(conn, bc->initial_ctx,
		                          &ctx->ssl, &ctx->bio, ha_quic_meth, ctx) == -1)
			goto err;

		SSL_set_accept_state(ctx->ssl);
	}

	*xprt_ctx = ctx;

	/* Leave init state and start handshake */
	conn->flags |= CO_FL_SSL_WAIT_HS | CO_FL_WAIT_L6_CONN;
	/* Start the handshake */
	tasklet_wakeup(ctx->wait_event.tasklet);

	TRACE_LEAVE(QUIC_EV_CONN_NEW, conn);

	return 0;

 err:
	if (ctx->wait_event.tasklet)
		tasklet_free(ctx->wait_event.tasklet);
	pool_free(pool_head_quic_conn_ctx, ctx);
	TRACE_DEVEL("leaving in error", QUIC_EV_CONN_NEW|QUIC_EV_CONN_ENEW, conn);
	return -1;
}

/* Release the SSL context of <srv> server. */
void quic_conn_free_srv_ctx(struct server *srv)
{
	QDPRINTF("%s\n", __func__);
	if (srv->ssl_ctx.ctx)
		SSL_CTX_free(srv->ssl_ctx.ctx);
}

/*
 * Prepare the SSL context for <srv> server.
 * Returns an error count.
 */
int quic_conn_prepare_srv_ctx(struct server *srv)
{
	struct proxy *curproxy = srv->proxy;
	int cfgerr = 0;
	SSL_CTX *ctx = NULL;
	int verify = SSL_VERIFY_NONE;
	long mode =
		SSL_MODE_ENABLE_PARTIAL_WRITE |
		SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER |
		SSL_MODE_RELEASE_BUFFERS |
		SSL_MODE_SMALL_BUFFERS;

	/* Make sure openssl opens /dev/urandom before the chroot */
	if (!ssl_initialize_random()) {
		ha_alert("OpenSSL random data generator initialization failed.\n");
		cfgerr++;
	}

	ctx = SSL_CTX_new(TLS_client_method());
	SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
	SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION);

	SSL_CTX_set_mode(ctx, mode);
	QDPRINTF("%s SSL ctx mode: %ld\n", __func__, mode);

	srv->ssl_ctx.ctx = ctx;
	if (srv->ssl_ctx.client_crt) {
		if (SSL_CTX_use_PrivateKey_file(srv->ssl_ctx.ctx, srv->ssl_ctx.client_crt, SSL_FILETYPE_PEM) <= 0) {
			ha_alert("config : %s '%s', server '%s': unable to load SSL private key from PEM file '%s'.\n",
			         proxy_type_str(curproxy), curproxy->id,
			         srv->id, srv->ssl_ctx.client_crt);
			cfgerr++;
		}
		else if (SSL_CTX_use_certificate_chain_file(srv->ssl_ctx.ctx, srv->ssl_ctx.client_crt) <= 0) {
			ha_alert("config : %s '%s', server '%s': unable to load ssl certificate from PEM file '%s'.\n",
			         proxy_type_str(curproxy), curproxy->id,
			         srv->id, srv->ssl_ctx.client_crt);
			cfgerr++;
		}
		else if (SSL_CTX_check_private_key(srv->ssl_ctx.ctx) <= 0) {
			ha_alert("config : %s '%s', server '%s': inconsistencies between private key and certificate loaded from PEM file '%s'.\n",
			         proxy_type_str(curproxy), curproxy->id,
			         srv->id, srv->ssl_ctx.client_crt);
			cfgerr++;
		}
	}

	if (global.ssl_server_verify == SSL_SERVER_VERIFY_REQUIRED)
		verify = SSL_VERIFY_PEER;
	switch (srv->ssl_ctx.verify) {
	case SSL_SOCK_VERIFY_NONE:
		verify = SSL_VERIFY_NONE;
		break;
	case SSL_SOCK_VERIFY_REQUIRED:
		verify = SSL_VERIFY_PEER;
		break;
	}
	SSL_CTX_set_verify(srv->ssl_ctx.ctx, verify,
	                   (srv->ssl_ctx.verify_host || (verify & SSL_VERIFY_PEER)) ? ssl_sock_srv_verifycbk : NULL);
	if (verify & SSL_VERIFY_PEER) {
		if (srv->ssl_ctx.ca_file) {
			/* set CAfile to verify */
			if (!ssl_set_verify_locations_file(srv->ssl_ctx.ctx, srv->ssl_ctx.ca_file)) {
				ha_alert("Proxy '%s', server '%s' [%s:%d] unable to set CA file '%s'.\n",
				         curproxy->id, srv->id,
				         srv->conf.file, srv->conf.line, srv->ssl_ctx.ca_file);
				cfgerr++;
			}
		}
		else {
			if (global.ssl_server_verify == SSL_SERVER_VERIFY_REQUIRED)
				ha_alert("Proxy '%s', server '%s' [%s:%d] verify is enabled by default "
				         "but no CA file specified. If you're running on a LAN where "
				         "you're certain to trust the server's certificate, please set "
				         "an explicit 'verify none' statement on the 'server' line, or "
				         "use 'ssl-server-verify none' in the global section to disable "
				         "server-side verifications by default.\n",
				         curproxy->id, srv->id,
				         srv->conf.file, srv->conf.line);
			else
				ha_alert("Proxy '%s', server '%s' [%s:%d] verify is enabled but no CA file specified.\n",
				         curproxy->id, srv->id,
				         srv->conf.file, srv->conf.line);
			cfgerr++;
		}
#ifdef X509_V_FLAG_CRL_CHECK
		if (srv->ssl_ctx.crl_file) {
			X509_STORE *store = SSL_CTX_get_cert_store(srv->ssl_ctx.ctx);

			if (!ssl_set_cert_crl_file(store, srv->ssl_ctx.crl_file)) {
				ha_alert("Proxy '%s', server '%s' [%s:%d] unable to configure CRL file '%s'.\n",
				         curproxy->id, srv->id,
				         srv->conf.file, srv->conf.line, srv->ssl_ctx.crl_file);
				cfgerr++;
			}
			else {
				X509_STORE_set_flags(store, X509_V_FLAG_CRL_CHECK|X509_V_FLAG_CRL_CHECK_ALL);
			}
		}
#endif
	}

	SSL_CTX_set_session_cache_mode(srv->ssl_ctx.ctx, SSL_SESS_CACHE_CLIENT |
	                               SSL_SESS_CACHE_NO_INTERNAL_STORE);
	SSL_CTX_sess_set_new_cb(srv->ssl_ctx.ctx, ssl_sess_new_srv_cb);
	if (srv->ssl_ctx.ciphers &&
		!SSL_CTX_set_cipher_list(srv->ssl_ctx.ctx, srv->ssl_ctx.ciphers)) {
		ha_alert("Proxy '%s', server '%s' [%s:%d] : unable to set SSL cipher list to '%s'.\n",
		         curproxy->id, srv->id,
		         srv->conf.file, srv->conf.line, srv->ssl_ctx.ciphers);
		cfgerr++;
	}

#if (HA_OPENSSL_VERSION_NUMBER >= 0x10101000L)
	if (srv->ssl_ctx.ciphersuites &&
		!SSL_CTX_set_ciphersuites(srv->ssl_ctx.ctx, srv->ssl_ctx.ciphersuites)) {
		ha_alert("Proxy '%s', server '%s' [%s:%d] : unable to set TLS 1.3 cipher suites to '%s'.\n",
		         curproxy->id, srv->id,
		         srv->conf.file, srv->conf.line, srv->ssl_ctx.ciphersuites);
		cfgerr++;
	}
#endif
	SSL_CTX_set_quic_method(ctx, &ha_quic_method);

    return cfgerr;
}

/* transport-layer operations for QUIC connections. */
static struct xprt_ops quic_conn = {
	.snd_buf  = quic_conn_from_buf,
	.rcv_buf  = quic_conn_to_buf,
	.subscribe = quic_conn_subscribe,
	.unsubscribe = quic_conn_unsubscribe,
	.remove_xprt = quic_conn_remove_xprt,
	.shutr    = NULL,
	.shutw    = NULL,
	.close    = NULL,
	.init     = qc_conn_init,
	.prepare_bind_conf = ssl_sock_prepare_bind_conf,
	.destroy_bind_conf = ssl_sock_destroy_bind_conf,
	.prepare_srv = quic_conn_prepare_srv_ctx,
	.destroy_srv = quic_conn_free_srv_ctx,
	.name     = "QUIC",
};


__attribute__((constructor))
static void __quic_conn_init(void)
{
	ha_quic_meth = BIO_meth_new(0x666, "ha QUIC methods");
	xprt_register(XPRT_QUIC, &quic_conn);
}

__attribute__((destructor))
static void __quic_conn_deinit(void)
{
	BIO_meth_free(ha_quic_meth);
}

/*
 * Inspired from session_accept_fd().
 * Instantiate a new connection (connection struct) to be attached to <quic_conn>
 * QUIC connection of <l> listener.
 * Returns 1 if succeeded, 0 if not.
 */
static int quic_new_cli_conn(struct quic_conn *quic_conn,
                             struct listener *l, struct sockaddr_storage *saddr)
{
	struct connection *cli_conn;
	struct proxy *p = l->bind_conf->frontend;
	struct session *sess;

	if (unlikely((cli_conn = conn_new()) == NULL))
		goto out;

	if (!sockaddr_alloc(&cli_conn->dst))
		goto out_free_conn;

	QDPRINTF("%s conn: @%p\n", __func__, cli_conn);
	quic_conn->conn = cli_conn;
	cli_conn->quic_conn = quic_conn;

	/* XXX Not sure it is safe to keep this statement. */
	cli_conn->handle.fd = l->fd;
	if (saddr)
		*cli_conn->dst = *saddr;
	cli_conn->flags |= CO_FL_ADDR_FROM_SET;
	cli_conn->target = &l->obj_type;
	cli_conn->proxy_netns = l->netns;

	conn_prepare(cli_conn, l->proto, l->bind_conf->xprt);

#if 0
	/* XXX DO NOT fd_insert() l->fd again with another I/O handler XXX
	 * This should be the case for an outgoing QUIC connection (haproxy as QUIC client).
	 */
	conn_ctrl_init(cli_conn);
#else
	cli_conn->flags |= CO_FL_CTRL_READY;
#endif

	/* wait for a PROXY protocol header */
	if (l->options & LI_O_ACC_PROXY)
		cli_conn->flags |= CO_FL_ACCEPT_PROXY;

	/* wait for a NetScaler client IP insertion protocol header */
	if (l->options & LI_O_ACC_CIP)
		cli_conn->flags |= CO_FL_ACCEPT_CIP;

	if (conn_xprt_init(cli_conn) < 0)
		goto out_free_conn;

	/* Add the handshake pseudo-XPRT */
	if (cli_conn->flags & (CO_FL_ACCEPT_PROXY | CO_FL_ACCEPT_CIP)) {
		if (xprt_add_hs(cli_conn) != 0)
			goto out_free_conn;
	}
	sess = session_new(p, l, &cli_conn->obj_type);
	if (!sess)
		goto out_free_conn;

	conn_set_owner(cli_conn, sess, NULL);


	/* OK let's complete stream initialization since there is no handshake */
	if (conn_complete_session(cli_conn) >= 0)
		return 1;

	/* error unrolling */
 out_free_sess:
	 /* prevent call to listener_release during session_free. It will be
	  * done below, for all errors. */
	sess->listener = NULL;
	session_free(sess);
 out_free_conn:
	conn_stop_tracking(cli_conn);
	conn_xprt_close(cli_conn);
	conn_free(cli_conn);
 out:

	return 0;
}

/*
 * Parse into <qpkt> a long header located at <*buf> buffer, <end> begin a pointer to the end
 * past one byte of this buffer.
 */
static inline int quic_packet_read_long_header(unsigned char **buf, const unsigned char *end,
                                               struct quic_rx_packet *qpkt)
{
	unsigned char dcid_len, scid_len;

	/* Version */
	if (!quic_read_uint32(&qpkt->version, (const unsigned char **)buf, end))
		return 0;

	if (!qpkt->version) { /* XXX TO DO XXX Version negotiation packet */ };

	/* Destination Connection ID Length */
	dcid_len = *(*buf)++;
	/* We want to be sure we can read <dcid_len> bytes and one more for <scid_len> value */
	if (dcid_len > QUIC_CID_MAXLEN || end - *buf < dcid_len + 1)
		/* XXX MUST BE DROPPED */
		return 0;

	if (dcid_len) {
		/*
		 * Check that the length of this received DCID matches the CID lengths
		 * of our implementation for non Initials packets only.
		 */
		if (qpkt->type != QUIC_PACKET_TYPE_INITIAL && dcid_len != QUIC_CID_LEN)
			return 0;

		memcpy(qpkt->dcid.data, *buf, dcid_len);
	}

	qpkt->dcid.len = dcid_len;
	*buf += dcid_len;

	/* Source Connection ID Length */
	scid_len = *(*buf)++;
	if (scid_len > QUIC_CID_MAXLEN || end - *buf < scid_len)
		/* XXX MUST BE DROPPED */
		return 0;

	if (scid_len)
		memcpy(qpkt->scid.data, *buf, scid_len);
	qpkt->scid.len = scid_len;
	*buf += scid_len;

	return 1;
}

/*
 * Try to remove the header protecttion of <qpkt> QUIC packet attached to <conn>
 * QUIC connection with <buf> as packet number field address, <end> a pointer to one
 * byte past the end of the buffer containing this packet and <beg> the address of
 * the packet first byte.
 * If succeeded, this function updates <*buf> to point to the next packet in the buffer.
 * Returns 1 if succeeded, 0 if not.
 */
static inline int qc_try_rm_hp(struct quic_rx_packet *qpkt,
                               unsigned char **buf, unsigned char *beg,
                               const unsigned char *end,
                               struct quic_conn *conn)
{
	unsigned char *pn = NULL; /* Packet number field */
	enum quic_tls_enc_level tel;
	struct quic_enc_level *qel;
	size_t pktlen;
	/* Only for traces. */
	struct quic_rx_packet *qpkt_trace;

	qpkt_trace = NULL;
	TRACE_ENTER(QUIC_EV_CONN_TRMHP, conn->conn);
	/*
	 * The packet number is here. This is also the start minus
	 * QUIC_PACKET_PN_MAXLEN of the sample used to add/remove the header
	 * protection.
	 */
	pn = *buf;
	pktlen = pn - beg + qpkt->len;
	if (pktlen > sizeof qpkt->data) {
		TRACE_PROTO("Too big packet", QUIC_EV_CONN_TRMHP, conn->conn,, &pktlen);
		goto err;
	}

	tel = qpkt->long_header ?
		quic_packet_type_enc_level(qpkt->type) : QUIC_TLS_ENC_LEVEL_APP;
	qel = &conn->enc_levels[tel];

	if (qel->tls_ctx.rx.flags & QUIC_FL_TLS_SECRETS_SET) {
		/*
		 * Note that the following function enables us to unprotect the packet
		 * number and its length subsequently used to decrypt the entire
		 * packets.
		 */
		if (!qc_do_rm_hp(qpkt, &qel->tls_ctx,
		                 qel->pktns->rx.largest_pn, pn, beg, end)) {
			TRACE_PROTO("hp error", QUIC_EV_CONN_TRMHP, conn->conn);
			goto err;
		}

		QDPRINTF("%s inserting packet number: %lu enc. level: %d\n",
		         __func__, qpkt->pn, tel);

		/* Store the packet */
		qpkt->pn_node.key = qpkt->pn;
		eb64_insert(&qel->rx.qpkts, &qpkt->pn_node);
		/* The AAD includes the packet number field found at <pn>. */
		qpkt->aad_len = pn - beg + qpkt->pnl;
		qpkt_trace = qpkt;
	}
	else {
		TRACE_PROTO("hp not removed", QUIC_EV_CONN_TRMHP, conn->conn);
		qpkt->pn_offset = pn - beg;
		LIST_ADDQ(&qel->rx.pqpkts, &qpkt->list);
	}

	/* Increase the total length of this packet by the header length. */
	qpkt->len += pn - beg;
	memcpy(qpkt->data, beg, qpkt->len);
	/* Updtate the offset of <*buf> for the next QUIC packet. */
	*buf = beg + qpkt->len;

	TRACE_LEAVE(QUIC_EV_CONN_TRMHP, conn->conn, qpkt_trace);
	return 1;

 err:
	TRACE_DEVEL("leaving in error", QUIC_EV_CONN_TRMHP, conn->conn, qpkt_trace);
	return 0;
}

typedef ssize_t qpkt_read_func(unsigned char **buf,
                               const unsigned char *end,
                               struct quic_rx_packet *qpkt, void *ctx,
                               struct sockaddr_storage *saddr,
                               socklen_t *saddrlen);

static ssize_t qc_srv_pkt_rcv(unsigned char **buf, const unsigned char *end,
                              struct quic_rx_packet *qpkt, void *ctx,
                              struct sockaddr_storage *saddr, socklen_t *saddrlen)
{
	unsigned char *beg;
	uint64_t len;
	struct quic_conn *conn;
	struct eb_root *cids;
	struct ebmb_node *node;
	struct connection *srv_conn;
	struct quic_conn_ctx *conn_ctx;

	conn = NULL;
	TRACE_ENTER(QUIC_EV_CONN_SPKT);
	if (end <= *buf)
		goto err;

	/* Fixed bit */
	if (!(**buf & QUIC_PACKET_FIXED_BIT))
		/* XXX TO BE DISCARDED */
		goto err;

	srv_conn = ctx;
	beg = *buf;
	/* Header form */
	qpkt->long_header = **buf & QUIC_PACKET_LONG_HEADER_BIT;
	/* Packet type XXX does not exist for short headers XXX */
	qpkt->type = (*(*buf)++ >> QUIC_PACKET_TYPE_SHIFT) & QUIC_PACKET_TYPE_BITMASK;
	if (qpkt->long_header) {
		size_t cid_lookup_len;

		if (!quic_packet_read_long_header(buf, end, qpkt))
			goto err;

		/* For Initial packets, and for servers (QUIC clients connections),
		 * there is no Initial connection IDs storage.
		 */
		if (qpkt->type == QUIC_PACKET_TYPE_INITIAL) {
			cids = &((struct server *)__objt_server(srv_conn->target))->cids;
			cid_lookup_len = qpkt->dcid.len;
		}
		else {
			cids = &((struct server *)__objt_server(srv_conn->target))->cids;
			cid_lookup_len = QUIC_CID_LEN;
		}

		node = ebmb_lookup(cids, qpkt->dcid.data, cid_lookup_len);
		if (!node) {
			QDPRINTF("Connection not found.\n");
			goto err;
		}
		conn = ebmb_entry(node, struct quic_conn, scid_node);

		if (qpkt->type == QUIC_PACKET_TYPE_INITIAL) {
			conn->dcid.len = qpkt->scid.len;
			if (qpkt->scid.len)
				memcpy(conn->dcid.data, qpkt->scid.data, qpkt->scid.len);
		}

		if (qpkt->type == QUIC_PACKET_TYPE_INITIAL) {
			uint64_t token_len;

			if (!quic_dec_int(&token_len, (const unsigned char **)buf, end) || end - *buf < token_len)
				goto err;

			/* XXX TO DO XXX 0 value means "the token is not present".
			 * A server which sends an Initial packet must not set the token.
			 * So, a client which receives an Initial packet with a token
			 * MUST discard the packet or generate a connection error with
			 * PROTOCOL_VIOLATION as type.
			 * The token must be provided in a Retry packet or NEW_TOKEN frame.
			 */
			qpkt->token_len = token_len;
		}
	}
	else {
		/* XXX TO DO: Short header XXX */
		if (end - *buf < QUIC_CID_LEN) {
			QDPRINTF("Too short short headder\n");
			goto err;
		}
		cids = &((struct server *)__objt_server(srv_conn->target))->cids;
		node = ebmb_lookup(cids, *buf, QUIC_CID_LEN);
		if (!node) {
			QDPRINTF("Unknonw connection ID\n");
			goto err;
		}

		conn = ebmb_entry(node, struct quic_conn, scid_node);
		*buf += QUIC_CID_LEN;
	}

	/*
	 * Only packets packets with long headers and not RETRY or VERSION as type
	 * have a length field.
	 */
	if (qpkt->long_header && qpkt->type != QUIC_PACKET_TYPE_RETRY && qpkt->version) {
		if (!quic_dec_int(&len, (const unsigned char **)buf, end) || end - *buf < len) {
			QDPRINTF("Could not decode the packet length or "
			         "too short packet (%zu, %zu)\n", len, end - *buf);
			goto err;
		}
		qpkt->len = len;
	}
	else if (!qpkt->long_header) {
		/* A short packet is the last one of an UDP datagram. */
		qpkt->len = end - *buf;
	}
	QDPRINTF("%s packet length: %zu\n", __func__, qpkt->len);

	if (!qc_try_rm_hp(qpkt, buf, beg, end, conn))
		goto err;

	conn_ctx = conn->conn->xprt_ctx;
	/* Wake the tasklet of the QUIC connection packet handler. */
	if (conn_ctx)
		tasklet_wakeup(conn_ctx->wait_event.tasklet);

	TRACE_LEAVE(QUIC_EV_CONN_SPKT, conn ? conn->conn : NULL);

	return qpkt->len;

 err:
	TRACE_DEVEL("Leaing in error", QUIC_EV_CONN_ESPKT, conn ? conn->conn : NULL);
	return -1;
}

static ssize_t qc_lstnr_pkt_rcv(unsigned char **buf, const unsigned char *end,
                                struct quic_rx_packet *qpkt, void *ctx,
                                struct sockaddr_storage *saddr, socklen_t *saddrlen)
{
	unsigned char *beg;
	uint64_t len;
	struct quic_conn *conn;
	struct eb_root *cids;
	struct ebmb_node *node;
	struct listener *l;
	struct quic_conn_ctx *conn_ctx;

	conn = NULL;
	TRACE_ENTER(QUIC_EV_CONN_LPKT);
	if (end <= *buf)
		goto err;

	/* Fixed bit */
	if (!(**buf & QUIC_PACKET_FIXED_BIT))
		/* XXX TO BE DISCARDED */
		goto err;

	l = ctx;
	beg = *buf;
	/* Header form */
	qpkt->long_header = **buf & QUIC_PACKET_LONG_HEADER_BIT;
	/* Packet type XXX does not exist for short headers XXX */
	qpkt->type = (*(*buf)++ >> QUIC_PACKET_TYPE_SHIFT) & QUIC_PACKET_TYPE_BITMASK;
	if (qpkt->long_header) {
		size_t cid_lookup_len;
		unsigned char dcid_len;
		size_t saddr_len;

		if (!quic_packet_read_long_header(buf, end, qpkt))
			goto err;

		dcid_len = qpkt->dcid.len;
		saddr_len = 0;
		/*
		 * DCIDs of first packets coming from clients may have the same values.
		 * Let's distinguish them concatenating the socket addresses to the DCIDs.
		 */
		if (qpkt->type == QUIC_PACKET_TYPE_INITIAL)
			saddr_len = quic_cid_saddr_cat(&qpkt->dcid, saddr);

		/* For Initial packets, and for servers (QUIC clients connections),
		 * there is no Initial connection IDs storage.
		 */
		if (qpkt->type == QUIC_PACKET_TYPE_INITIAL) {
			cids = &l->icids;
			cid_lookup_len = qpkt->dcid.len;
		}
		else {
			cids = &l->cids;
			cid_lookup_len = QUIC_CID_LEN;
		}

		node = ebmb_lookup(cids, qpkt->dcid.data, cid_lookup_len);
		if (!node) {
			struct quic_cid *odcid;

			if (qpkt->type != QUIC_PACKET_TYPE_INITIAL) {
				QDPRINTF("Connection not found.\n");
				goto err;
			}

			conn =  quic_new_conn(qpkt->version);
			if (!conn)
				goto err;

			if (!quic_new_cli_conn(conn, l, saddr)) {
				free(conn);
				goto err;
			}

			if (!qc_new_conn_init(conn, &l->icids, &l->cids,
			                      qpkt->dcid.data, cid_lookup_len,
			                      qpkt->scid.data, qpkt->scid.len))
				goto err;

			odcid = &conn->params.original_destination_connection_id;
			/* Copy the transport parameters. */
			conn->params = l->bind_conf->quic_params;
			/* Copy original_destination_connection_id transport parameter. */
			memcpy(odcid->data, &qpkt->dcid, dcid_len);
			odcid->len = dcid_len;
			/* Copy the initial source connection ID. */
			quic_cid_cpy(&conn->params.initial_source_connection_id, &conn->scid);
			conn->enc_params_len =
				quic_transport_params_encode(conn->enc_params,
				                             conn->enc_params + sizeof conn->enc_params,
				                             &conn->params, 1);
			if (!conn->enc_params_len)
				goto err;

			conn_ctx = conn->conn->xprt_ctx;
			SSL_set_quic_transport_params(conn_ctx->ssl, conn->enc_params, conn->enc_params_len);
		}
		else {
			if (qpkt->type == QUIC_PACKET_TYPE_INITIAL)
				conn = ebmb_entry(node, struct quic_conn, odcid_node);
			else
				conn = ebmb_entry(node, struct quic_conn, scid_node);
		}

		if (qpkt->type == QUIC_PACKET_TYPE_INITIAL) {
			uint64_t token_len;
			struct quic_tls_ctx *ctx = &conn->enc_levels[QUIC_TLS_ENC_LEVEL_INITIAL].tls_ctx;

			if (!quic_dec_int(&token_len, (const unsigned char **)buf, end) || end - *buf < token_len)
				goto err;

			/* XXX TO DO XXX 0 value means "the token is not present".
			 * A server which sends an Initial packet must not set the token.
			 * So, a client which receives an Initial packet with a token
			 * MUST discard the packet or generate a connection error with
			 * PROTOCOL_VIOLATION as type.
			 * The token must be provided in a Retry packet or NEW_TOKEN frame.
			 */
			qpkt->token_len = token_len;
			/*
			 * NOTE: the socket address it concatenated to the destination ID choosen by the client
			 * for Initial packets.
			 */
			if (!ctx->rx.hp && !qc_new_isecs(conn->conn, qpkt->dcid.data,
			                                 qpkt->dcid.len - saddr_len, 1)) {
				QDPRINTF("Could not derive initial secrets\n");
				goto err;
			}
		}
	}
	else {
		/* XXX TO DO: Short header XXX */
		if (end - *buf < QUIC_CID_LEN) {
			QDPRINTF("Too short short headder\n");
			goto err;
		}
		cids = &l->cids;
		node = ebmb_lookup(cids, *buf, QUIC_CID_LEN);
		if (!node) {
			QDPRINTF("Unknonw connection ID\n");
			goto err;
		}
		conn = ebmb_entry(node, struct quic_conn, scid_node);
		*buf += QUIC_CID_LEN;
	}

	/*
	 * Only packets packets with long headers and not RETRY or VERSION as type
	 * have a length field.
	 */
	if (qpkt->long_header && qpkt->type != QUIC_PACKET_TYPE_RETRY && qpkt->version) {
		if (!quic_dec_int(&len, (const unsigned char **)buf, end) || end - *buf < len) {
			QDPRINTF("Could not decode the packet length or "
			         "too short packet (%zu, %zu)\n", len, end - *buf);
			goto err;
		}
		qpkt->len = len;
	}
	else if (!qpkt->long_header) {
		/* A short packet is the last one of an UDP datagram. */
		qpkt->len = end - *buf;
	}
	QDPRINTF("%s packet length: %zu\n", __func__, qpkt->len);

	if (!qc_try_rm_hp(qpkt, buf, beg, end, conn))
		goto err;

	/* Update the state if needed. */
	conn_ctx = conn->conn->xprt_ctx;
	if (conn_ctx->state == QUIC_HS_ST_SERVER_INITIAL && qpkt->type == QUIC_PACKET_TYPE_HANDSHAKE)
		conn_ctx->state = QUIC_HS_ST_SERVER_HANDSHAKE;

	/* Wake the tasklet of the QUIC connection packet handler. */
	if (conn_ctx)
		tasklet_wakeup(conn_ctx->wait_event.tasklet);

	TRACE_LEAVE(QUIC_EV_CONN_LPKT, conn ? conn->conn : NULL);

	return qpkt->len;

 err:
	TRACE_DEVEL("Leaving in error", QUIC_EV_CONN_ELPKT, conn ? conn->conn : NULL);
	QDPRINTF("%s failed\n", __func__);
	return -1;
}

/*
 * This function builds into <buf> buffer a QUIC long packet header whose size may be computed
 * in advance. This is the reponsability of the caller to check there is enough room in this
 * buffer to build a long header.
 * Returns 0 if <type> QUIC packet type is not supported by long header, or 1 if succeeded.
 */
static int quic_build_packet_long_header(unsigned char **buf, const unsigned char *end,
                                         int type, size_t pn_len, struct quic_conn *conn)
{
	if (type > QUIC_PACKET_TYPE_RETRY)
		return 0;

	/* #0 byte flags */
	*(*buf)++ = QUIC_PACKET_FIXED_BIT | QUIC_PACKET_LONG_HEADER_BIT |
		(type << QUIC_PACKET_TYPE_SHIFT) | (pn_len - 1);
	/* Version */
	quic_write_uint32(buf, end, conn->version);
	*(*buf)++ = conn->dcid.len;
	/* Destination connection ID */
	if (conn->dcid.len) {
		memcpy(*buf, conn->dcid.data, conn->dcid.len);
		*buf += conn->dcid.len;
	}
	/* Source connection ID */
	*(*buf)++ = conn->scid.len;
	if (conn->scid.len) {
		memcpy(*buf, conn->scid.data, conn->scid.len);
		*buf += conn->scid.len;
	}

	return 1;
}

/*
 * This function builds into <buf> buffer a QUIC long packet header whose size may be computed
 * in advance. This is the reponsability of the caller to check there is enough room in this
 * buffer to build a long header.
 * Returns 0 if <type> QUIC packet type is not supported by long header, or 1 if succeeded.
 */
static int quic_build_packet_short_header(unsigned char **buf, const unsigned char *end,
                                          size_t pn_len, struct quic_conn *conn)
{
	/* #0 byte flags */
	*(*buf)++ = QUIC_PACKET_FIXED_BIT | (pn_len - 1);
	/* Destination connection ID */
	if (conn->dcid.len) {
		memcpy(*buf, conn->dcid.data, conn->dcid.len);
		*buf += conn->dcid.len;
	}

	return 1;
}

/*
 * Apply QUIC header protection to the packet with <buf> as first byte address,
 * <pn> as address of the Packet number field, <pnlen> being this field length
 * with <aead> as AEAD cipher and <key> as secret key.
 * Returns 1 if succeeded or 0 if failed.
 */
static int quic_apply_header_protection(unsigned char *buf, unsigned char *pn, size_t pnlen,
                                        const EVP_CIPHER *aead, const unsigned char *key)
{
	int i, ret, outlen;
	EVP_CIPHER_CTX *ctx;
	/*
	 * We need an IV of at least 5 bytes: one byte for bytes #0
	 * and at most 4 bytes for the packet number
	 */
	unsigned char mask[5] = {0};

	ret = 0;
	ctx = EVP_CIPHER_CTX_new();
	if (!ctx)
		return 0;

	if (!EVP_EncryptInit_ex(ctx, aead, NULL, key, pn + QUIC_PACKET_PN_MAXLEN) ||
	    !EVP_EncryptUpdate(ctx, mask, &outlen, mask, sizeof mask) ||
	    !EVP_EncryptFinal_ex(ctx, mask, &outlen))
		goto out;

	*buf ^= mask[0] & (*buf & QUIC_PACKET_LONG_HEADER_BIT ? 0xf : 0x1f);
	for (i = 0; i < pnlen; i++)
		pn[i] ^= mask[i + 1];

	ret = 1;

 out:
	EVP_CIPHER_CTX_free(ctx);

	return ret;
}

/*
 * Build a QUIC ACK frame into <buf> buffer from <qars> list of ack ranges.
 * <qars> MUST not be empty.
 * Return 0 if failed, or the strictly positive length of the ACK frame if not.
 */
static inline ssize_t quic_do_build_ack_frame(struct buffer *buf,
                                              struct quic_ack_ranges *qars)
{
	struct quic_frame ack_frm = { .type = QUIC_FT_ACK, };
	unsigned char *pos = (unsigned char *)b_orig(buf);

	ack_frm.tx_ack.ack_delay = 0;
	ack_frm.tx_ack.ack_ranges = qars;
	if (!qc_build_frm(&pos, pos + buf->size, &ack_frm))
		return 0;

	return pos - (unsigned char *)b_orig(buf);
}

/*
 * This function builds a clear handshake packet used during a QUIC TLS handshakes
 * into <wbuf> the current <wbuf> for <conn> QUIC connection with <qel> as QUIC
 * TLS encryption level for outgoing packets filling it with as much as CRYPTO
 * data as possible from <offset> offset in the CRYPTO data stream. Note that
 * this offset value is updated by the length of the CRYPTO frame used to embed
 * the CRYPTO data if this packet and only if the packet is successfully built.
 * The trailing QUIC_TLS_TAG_LEN bytes of this packet are not built. But they are
 * reserved so that to be sure there is enough room to build this AEAD TAG after
 * having successfully returned from this function and to be sure the position
 * pointer of <wbuf> may be safely incremented by QUIC_TLS_TAG_LEN. After having
 * returned from this funciton, <wbuf> position will point one past the last
 * byte of the payload with the confidence there is at least QUIC_TLS_TAG_LEN bytes
 * available packet to encrypt this packet.
 * This function also update the value of <buf_pn> pointer to point to the packet
 * number field in this packet. <pn_len> will also have the packet number
 * length as value.
 *
 * Return the length of the packet if succeeded minus QUIC_TLS_TAG_LEN, or -1 if
 * failed (not enough room in <wbuf> to build this packet plus QUIC_TLS_TAG_LEN
 * bytes), -2 if there are too much CRYPTO data in flight to build a packet.
 */
static ssize_t qc_do_build_hdshk_pkt(struct q_buf *wbuf, int pkt_type,
                                     unsigned char **buf_pn, size_t *pn_len,
                                     uint64_t *offset, size_t crypto_len,
                                     struct quic_enc_level *qel,
                                     struct quic_conn *conn)
{
	unsigned char *beg, *pos;
	const unsigned char *end;
	/* This packet type. */
	/* Packet number. */
	int64_t pn;
	/* The Length QUIC packet field value which is the length
	 * of the remaining data after this field after encryption.
	 */
	size_t len;
	size_t token_fields_len;
	/* The size of the CRYPTO frame heaeder (without the data). */
	size_t frm_header_sz;
	struct quic_frame frm = { .type = QUIC_FT_CRYPTO, };
	struct quic_crypto *crypto = &frm.crypto;
	size_t padding_len;
	ssize_t ack_frm_len;
	struct buffer *ack_buf;

	TRACE_ENTER(QUIC_EV_CONN_CHPKT, conn->conn);
	beg = pos = q_buf_getpos(wbuf);
	end = q_buf_end(wbuf);
	if (crypto_len) {
		crypto_len = crypto_len > QUIC_CRYPTO_IN_FLIGHT_MAX - conn->crypto_in_flight ?
			QUIC_CRYPTO_IN_FLIGHT_MAX - conn->crypto_in_flight : crypto_len;
		if (!crypto_len) {
			TRACE_DEVEL("ifcdada limit reached", QUIC_EV_CONN_CHPKT, conn->conn);
			goto out;
		}

		crypto->data = c_buf_getpos(qel, *offset);
		crypto->offset = *offset;
		/* Crypto frame header size (without data and data length) */
		frm_header_sz = sizeof frm.type + quic_int_getsize(crypto->offset);
	}
	else {
		frm_header_sz = 0;
	}

	/* For a server, the token field of an Initial packet is empty. */
	token_fields_len = pkt_type == QUIC_PACKET_TYPE_INITIAL ? 1 : 0;

	/* Check there is enough room to build the header followed by a token. */
	if (end - pos < QUIC_LONG_PACKET_MINLEN + conn->dcid.len +
	    conn->scid.len + token_fields_len)
		goto err;

	/* packet number */
	pn = qel->pktns->tx.next_pn + 1;

	/* packet number length */
	*pn_len = quic_packet_number_length(pn, qel->pktns->rx.largest_acked_pn);

	quic_build_packet_long_header(&pos, end, pkt_type, *pn_len, conn);

	/* Encode the token length (0) for an Initial packet. */
	if (pkt_type == QUIC_PACKET_TYPE_INITIAL)
		*pos++ = 0;

	/* Build an ACK frame if required. */
	ack_frm_len = 0;
	ack_buf = get_trash_chunk();
	if ((qel->pktns->flags & QUIC_FL_PKTNS_ACK_REQUIRED) &&
	    !LIST_ISEMPTY(&qel->pktns->rx.ack_ranges.list)) {
		ack_frm_len = quic_do_build_ack_frame(ack_buf, &qel->pktns->rx.ack_ranges);
		if (!ack_frm_len)
			goto err;

		qel->pktns->flags &= ~QUIC_FL_PKTNS_ACK_REQUIRED;
	}

	/* Length field value without the CRYPTO frame data length. */
	len = ack_frm_len + *pn_len + frm_header_sz + QUIC_TLS_TAG_LEN;
	if (crypto_len) {
		crypto->len = max_stream_data_size(end - pos, len, crypto_len);
		/* Add the CRYPTO data length to the packet length (after encryption) and
		 * the length of this length.
		 */
		len += quic_int_getsize(crypto->len) + crypto->len;
	}

	padding_len = 0;
	if (objt_server(conn->conn->target) &&
	    pkt_type == QUIC_PACKET_TYPE_INITIAL &&
	    len < QUIC_INITIAL_PACKET_MINLEN)
		len += padding_len = QUIC_INITIAL_PACKET_MINLEN - len;

	/*
	 * Length (of the remaining data). Must not fail because, the buffer size
	 * has been checked above.
	 */
	quic_enc_int(&pos, end, len);

	/* Packet number field address. */
	*buf_pn = pos;

	/* Packet number encoding. */
	quic_packet_number_encode(&pos, end, pn, *pn_len);

	if (ack_frm_len) {
		memcpy(pos, b_orig(ack_buf), ack_frm_len);
		pos += ack_frm_len;
	}

	/* Crypto frame */
	if (crypto_len && !qc_build_frm(&pos, end, &frm))
		goto err;

	/* Build a PADDING frame if needed. */
	if (padding_len) {
		frm.type = QUIC_FT_PADDING;
		frm.padding.len = padding_len;
		if (!qc_build_frm(&pos, end, &frm))
			goto err;
	}

	if (crypto_len)
		*offset += crypto->len;

 out:
	TRACE_LEAVE(QUIC_EV_CONN_CHPKT, conn->conn, (int *)(pos - beg));
	return pos - beg;

 err:
	TRACE_DEVEL("leaving in error (buffer full)", QUIC_EV_CONN_ECHPKT, conn->conn);
	return -1;
}

/*
 * Build a handshake packet into <buf> packet buffer with <pkt_type> as packet
 * type for <qc> QUIC connection from CRYPTO data stream at <*offset> offset to
 * be encrypted at <qel> encryption level.
 * Return -2 if the packet could not be encrypted for any reason, -1 if there was
 * not enough room in <buf> to build the packet, or the size of the built packet
 * if succeeded (may be zero if there is too much crypto data in flight to build the packet).
 */
static ssize_t qc_build_hdshk_pkt(struct q_buf *buf, struct quic_conn *qc, int pkt_type,
                                  uint64_t *offset, size_t len, struct quic_enc_level *qel)
{
	/* The pointer to the packet number field. */
	unsigned char *buf_pn;
	unsigned char *beg, *end, *payload;
	int64_t pn;
	size_t pn_len, payload_len, aad_len;
	ssize_t pkt_len;
	struct quic_tls_ctx *tls_ctx;
	struct quic_tx_crypto_frm *cf;
	uint64_t next_offset;

	TRACE_ENTER(QUIC_EV_CONN_HPKT, qc->conn);
	cf = NULL;
	beg = q_buf_getpos(buf);

	pn_len = 0;
	buf_pn = NULL;
	next_offset = *offset;
	pkt_len = qc_do_build_hdshk_pkt(buf, pkt_type, &buf_pn, &pn_len,
	                               &next_offset, len, qel, qc);
	if (pkt_len <= 0)
		return pkt_len;

	end = beg + pkt_len;
	pn = qel->pktns->tx.next_pn + 1;
	payload = buf_pn + pn_len;
	payload_len = end - payload;
	aad_len = payload - beg;

	tls_ctx = &qel->tls_ctx;
	if (!quic_packet_encrypt(payload, payload_len, beg, aad_len, pn, tls_ctx, qc->conn))
		return -2;

	end += QUIC_TLS_TAG_LEN;
	if (!quic_apply_header_protection(beg, buf_pn, pn_len,
	                                  tls_ctx->tx.hp, tls_ctx->tx.hp_key)) {
		TRACE_DEVEL("Could not apply the header protection", QUIC_EV_CONN_HPKT, qc->conn);
		return -2;
	}

	/*
	 * Now that a correct packet is built, let us set the position pointer of
	 * <buf> buf for the next packet.
	 */
	q_buf_setpos(buf, end);
	/* Consume a packet number. */
	++qel->pktns->tx.next_pn;

	if (next_offset - *offset) {
		cf = pool_alloc(pool_head_quic_tx_crypto_frm);
		if (!cf) {
			TRACE_DEVEL("CRYPTO frame allocation failed", QUIC_EV_CONN_HPKT, qc->conn);
			return -2;
		}
		/* The length of this TX CRYPTO frame is deduced from the offsets. */
		cf->len = next_offset - *offset;
		cf->pn.key = qel->pktns->tx.next_pn;
		/* Set the offset value to the current value before updating it. */
		cf->offset = *offset;
		/* Insert the CRYPTO frame. */
		eb64_insert(&qel->tx.crypto.frms, &cf->pn);
		/* Increment the offset of this crypto data stream */
		*offset += cf->len;
		/* Increment the CRYPTO data in flight counter. */
		qc->crypto_in_flight += cf->len;
	}

	/* Increment the number of bytes in <buf> buffer by the length of this packet. */
	buf->data += end - beg;

	TRACE_LEAVE(QUIC_EV_CONN_HPKT, qc->conn, cf);

	return end - beg;
}

/*
 * Prepare a clear post handhskake packet for <conn> QUIC connnection.
 * Return the length of this packet if succeeded, -1 <wbuf> was full.
 */
static ssize_t qc_do_build_phdshk_apkt(struct q_buf *wbuf, uint64_t pn, size_t *pn_len,
                                       unsigned char **buf_pn, struct quic_enc_level *qel,
                                       struct quic_conn *conn)
{
	const unsigned char *beg, *end;
	unsigned char *pos;
	struct quic_frame *frm, *sfrm;

	/* Packet number length */
	*pn_len = quic_packet_number_length(pn, qel->pktns->rx.largest_acked_pn);
	/* Check there is enough room to build this packet (without payload). */
	if (q_buf_room(wbuf) <
	    QUIC_TLS_TAG_LEN + QUIC_SHORT_PACKET_MINLEN +
	    sizeof_quic_cid(&conn->dcid) + *pn_len)
		return -1;

	beg = pos = q_buf_getpos(wbuf);
	/* Reserve enough room at the end of the packet for the AEAD TAG. */
	end = q_buf_end(wbuf) - QUIC_TLS_TAG_LEN;
	quic_build_packet_short_header(&pos, end, *pn_len, conn);
	/* Packet number field. */
	*buf_pn = pos;
	/* Packet number encoding. */
	quic_packet_number_encode(&pos, end, pn, *pn_len);

	/* Encode a maximum of frames. */
	list_for_each_entry_safe(frm, sfrm, &conn->tx.frms_to_send, list) {
		unsigned char *ppos;

		ppos = pos;
		if (!qc_build_frm(&ppos, end, frm)) {
			QDPRINTF("%s: Could not build frame %s\n",
			         __func__, quic_frame_type_string(frm->type));
			break;
		}
		LIST_DEL(&frm->list);
		pos = ppos;
	}

	return pos - beg;
}

/*
 * Prepare a post handhskake packet at Application encryption level for <conn>
 * QUIC connnection.
 * Return the length of this packet if succeeded, -1 if <wbuf> was full,
 * -2 in case of major error (encryption failure).
 */
static ssize_t qc_build_phdshk_apkt(struct q_buf *wbuf, struct quic_conn *conn)
{
	/* A pointer to the packet number fiel in <buf> */
	unsigned char *buf_pn;
	unsigned char *beg, *end, *payload;
	struct quic_enc_level *qel;
	struct quic_tls_ctx *tls_ctx;
	size_t pn_len, aad_len, payload_len;
	ssize_t pkt_len;
	uint64_t pn;

	TRACE_ENTER(QUIC_EV_CONN_PAPKT, conn->conn);
	beg = q_buf_getpos(wbuf);
	qel = &conn->enc_levels[QUIC_TLS_ENC_LEVEL_APP];
	pn = qel->pktns->tx.next_pn + 1;

	pkt_len = qc_do_build_phdshk_apkt(wbuf, pn, &pn_len, &buf_pn, qel, conn);
	if (pkt_len < 0) {
		QDPRINTF("%s returns %zd\n", __func__, pkt_len);
		return pkt_len;
	}

	end = beg + pkt_len;
	payload = buf_pn + pn_len;
	payload_len = end - payload;
	aad_len = payload - beg;

	tls_ctx = &qel->tls_ctx;
	if (!quic_packet_encrypt(payload, payload_len, beg, aad_len, pn, tls_ctx, conn->conn))
		return -2;

	if (!quic_apply_header_protection(beg, buf_pn, pn_len,
	                                  tls_ctx->tx.hp, tls_ctx->tx.hp_key)) {
		QDPRINTF("%s: could not apply header protection\n", __func__);
		return -2;
	}

	/* Consume a packet number. */
	++qel->pktns->tx.next_pn;

	end += QUIC_TLS_TAG_LEN;
	q_buf_setpos(wbuf, end);
	/* Increment the account of written data. */
	wbuf->data += end - beg;

	TRACE_LEAVE(QUIC_EV_CONN_PAPKT, conn->conn);

	return end - beg;
}

/*
 * Prepare a maximum of QUIC Application level packets from <ctx> QUIC
 * connection I/O handler context.
 * Returns 1 if succeeded, 0 if not.
 */
static int qc_prep_phdshk_pkts(struct quic_conn *qc)
{
	struct q_buf *wbuf;

	TRACE_ENTER(QUIC_EV_CONN_PAPKTS, qc->conn);
	wbuf = q_wbuf(qc);
	while (q_buf_empty(wbuf) && !LIST_ISEMPTY(&qc->tx.frms_to_send)) {
		ssize_t ret;

		ret = qc_build_phdshk_apkt(wbuf, qc);
		switch (ret) {
		case -2:
			return 0;
		default:
			/* Not enough room left in <wbuf>. */
			wbuf = q_next_wbuf(qc);
			continue;
		}
	}
	TRACE_LEAVE(QUIC_EV_CONN_PAPKTS, qc->conn);

	return 1;
}

/*
 * Read all the QUIC packets found in <buf> with <len> as length (typically a UDP
 * datagram), <ctx> being the QUIC I/O handler context, from QUIC connections,
 * calling <func> function;
 * Return the number of bytes read if succeded, -1 if not.
 */
static ssize_t quic_packets_read(char *buf, size_t len, void *ctx,
                                 struct sockaddr_storage *saddr, socklen_t *saddrlen,
                                 qpkt_read_func *func)
{
	unsigned char *pos;
	const unsigned char *end;

	pos = (unsigned char *)buf;
	end = pos + len;

	do {
		int ret;
		struct quic_rx_packet *qpkt;

		qpkt = pool_alloc(pool_head_quic_rx_packet);
		if (!qpkt) {
			QDPRINTF("Not enough memory to allocate a new packet\n");
			goto err;
		}
		memset(qpkt, 0, sizeof(*qpkt));
		ret = func(&pos, end, qpkt, ctx, saddr, saddrlen);
		if (ret == -1) {
			pool_free(pool_head_quic_rx_packet, qpkt);
			goto err;
		}
		QDPRINTF("long header? %d packet type: 0x%02x \n", !!qpkt->long_header, qpkt->type);
	} while (pos < end);

	return pos - (unsigned char *)buf;

 err:
	return -1;
}

/*
 * QUIC I/O handler for connection to local listeners or remove servers
 * depending on <listener> boolean value, with <fd> as socket file
 * descriptor and <ctx> as context.
 */
static size_t quic_conn_handler(int fd, void *ctx, qpkt_read_func *func)
{
	ssize_t ret;
	size_t done = 0;
	struct buffer *buf = get_trash_chunk();
	/* Source address */
	struct sockaddr_storage saddr = {0};
	socklen_t saddrlen = sizeof saddr;

	if (!fd_recv_ready(fd))
		return 0;

	do {
		ret = recvfrom(fd, buf->area, buf->size, 0,
		               (struct sockaddr *)&saddr, &saddrlen);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN)
				fd_cant_recv(fd);
			goto out;
		}
	} while (0);

	QDPRINTF("-------------------------------------------"
	         "-----------------\n%s: recvfrom() server (%ld)\n", __func__, ret);

	done = buf->data = ret;
	quic_packets_read(buf->area, buf->data, ctx, &saddr, &saddrlen, func);

 out:
	return done;
}

/*
 * QUIC I/O handler for connections to local listeners with <fd> as socket
 * file descriptor.
 */
void quic_fd_handler(int fd)
{
	if (fdtab[fd].ev & FD_POLL_IN)
		quic_conn_handler(fd, fdtab[fd].owner, &qc_lstnr_pkt_rcv);
}

/*
 * QUIC I/O handler for connections to remote servers with <fd> as socket
 * file descriptor.
 */
void quic_conn_fd_handler(int fd)
{
	if (fdtab[fd].ev & FD_POLL_IN)
		quic_conn_handler(fd, fdtab[fd].owner, &qc_srv_pkt_rcv);
}

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
