/*
 *
 * Copyright 2019 HAProxy Technologies, Frédéric Lécaille <flecaille@haproxy.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <stdlib.h>
#include <errno.h>

#include <common/chunk.h>

#include <proto/fd.h>
#include <proto/quic_tls.h>

#include <types/global.h>
#include <types/quic.h>
#include <types/quic_tls.h>

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
struct quic_cid {
	unsigned char len;
	unsigned char data[QUIC_CID_MAXLEN];
};

struct quic_packet {
	int from_server;
	int long_header;
	unsigned char type;
	uint32_t version;
	struct quic_cid dcid;
	struct quic_cid scid;
	/* Packet number */
	uint64_t pn;
	/* Packet number length */
	uint32_t pnl;
	uint64_t token_len;
	/* Packet length */
	uint64_t len;
};


struct quic_conn {
	size_t cid_len;
	int aead_algo;
	struct ctx {
		unsigned char initial_secret[32];
		unsigned char client_initial_secret[32];
		unsigned char key[16];
		unsigned char iv[12];
		unsigned char aead_iv[16];
		/* Header protection key.
		 * Note: the header protection is applied after packet protection.
		 * As the header belong to the data, its protection must be removed before removing
		 * the packet protection.
		 */
		unsigned char hp[16];
		const EVP_CIPHER *aead;
	} ctx;
	/* One largest packet number by client/server by number space */
	uint64_t client_max_pn[3];
	uint64_t server_max_pn[3];


	/* XXX Do not insert anything after <cid> which contains a flexible array member!!! XXX */
	struct ebmb_node cid;
};


/* The first two bits of byte #0 gives the 2 logarithm of the encoded length. */
#define QUIC_VARINT_BYTE_0_BITMASK 0x3f
#define QUIC_VARINT_BYTE_0_SHIFT   6

/*
 * Decode a QUIC variable length integer.
 * Note that the result is a 64-bits integer but with the less significant
 * 62 bits as relevant information. The most significant 2 remaining bits encode
 * the length of the integer to be decoded. So, this function can return (uint64_t)-1
 * in case of any error.
 * Return the 64-bits decoded value when succeeded, -1 if not: <buf> provided buffer
 * was not big enough.
 */
uint64_t quic_dec_int(const unsigned char **buf, const unsigned char *end)
{
	uint64_t ret;
	size_t len;

	if (*buf == end)
		return -1;

	len = 1 << (**buf >> QUIC_VARINT_BYTE_0_SHIFT);
	if (*buf + len > end)
		return -1;

	ret = *(*buf)++ & QUIC_VARINT_BYTE_0_BITMASK;
	while (--len)
		ret = (ret << 8) | *(*buf)++;


	return ret;
}

int quic_enc_int(unsigned char **buf, const unsigned char *end, uint64_t val)
{
	switch (val) {
	case (1UL << 30) ... (1UL << 62) - 1:
		if (end - *buf < 8)
			return 0;
		*(*buf)++ = 0xc0 | (val >> 56);
		*(*buf)++ = val >> 48;
		*(*buf)++ = val >> 40;
		*(*buf)++ = val >> 32;
		*(*buf)++ = val >> 24;
		*(*buf)++ = val >> 16;
		*(*buf)++ = val >> 8;
		break;

	case (1UL << 14) ... (1UL << 30) - 1:
		if (end - *buf < 4)
			return 0;
		*(*buf)++ = 0x80 | (val >> 24);
		*(*buf)++ = val >> 16;
		*(*buf)++ = val >> 8;
		break;

	case (1UL <<  6) ... (1UL << 14) - 1:
		if (end - *buf < 2)
			return 0;
		*(*buf)++ = 0x40 | (val >> 8);
		break;

	case 0 ... (1UL <<  6) - 1:
		if (end - *buf < 1)
			return 0;
		break;

	default:
		return 0;
	}
	*(*buf)++ = val;

	return 1;
}

/* Return a 32-bits integer in <val> from QUIC packet with <buf> as address.
 * Returns 0 if failed (not enough data), 1 if succeeded.
 * Makes <buf> point to the data after this 32-bits value if succeeded.
 * Note that these 32-bits integers are network bytes ordered objects.
 */
static int quic_read_uint32(uint32_t *val, const unsigned char **buf, const unsigned char *end)
{
	if (end - *buf < sizeof *val)
		return 0;

	*val = ntohl(*(uint32_t *)*buf);
	*buf += sizeof *val;

	return 1;
}

/*
 * Derive the initial secret from the CID and QUIC version dependent salt.
 * Returns the size of the derived secret if succeeded, 0 if not.
 */
static int quic_derive_initial_secret(struct quic_conn *conn)
{
	size_t outlen;

	outlen = sizeof conn->ctx.initial_secret;
	if (!quic_hdkf_extract(conn->ctx.initial_secret, &outlen,
	                       conn->cid.key, conn->cid_len,
	                       initial_salt, sizeof initial_salt))
		return 0;

	return outlen;
}

/*
 * Derive the client initial secret from the initial secret.
 * Returns the size of the derived secret if succeeded, 0 if not.
 */
static ssize_t quic_derive_client_initial_secret(struct quic_conn *conn)
{
	size_t outlen;
	const unsigned char label[] = "client in";

	outlen = sizeof conn->ctx.client_initial_secret;
	if (!quic_hdkf_expand_label(conn->ctx.client_initial_secret, &outlen,
	                            conn->ctx.initial_secret, sizeof conn->ctx.initial_secret,
	                            label, sizeof label - 1))
	    return 0;

	hexdump(conn->ctx.client_initial_secret, outlen, "CLIENT INITIAL SECRET:\n");
	return outlen;
}

/*
 * Derive the client secret key from the the client initial secret.
 * Returns the size of the derived key if succeeded, 0 if not.
 */
static ssize_t quic_derive_key(struct quic_conn *conn)
{
	size_t outlen;
	const unsigned char label[] = "quic key";

	outlen = sizeof conn->ctx.key;
	if (!quic_hdkf_expand_label(conn->ctx.key, &outlen,
	                            conn->ctx.client_initial_secret, sizeof conn->ctx.client_initial_secret,
	                            label, sizeof label - 1))
	    return 0;

	hexdump(conn->ctx.key, outlen, "KEY:\n");
	return outlen;
}

/*
 * Derive the client IV from the client initial secret.
 * Returns the size of this IV if succeeded, 0 if not.
 */
static ssize_t quic_derive_iv(struct quic_conn *conn)
{
	size_t outlen;
	const unsigned char label[] = "quic iv";

	outlen = sizeof conn->ctx.iv;
	if (!quic_hdkf_expand_label(conn->ctx.iv, &outlen,
	                            conn->ctx.client_initial_secret, sizeof conn->ctx.client_initial_secret,
	                            label, sizeof label - 1))
	    return 0;

	hexdump(conn->ctx.iv, outlen, "IV:\n");
	return outlen;
}

/*
 * Derive the client header protection key from the client initial secret.
 * Returns the size of this key if succeeded, 0, if not.
 */
static ssize_t quic_derive_hp(struct quic_conn *conn)
{
	size_t outlen;
	const unsigned char label[] = "quic hp";

	outlen = sizeof conn->ctx.hp;
	if (!quic_hdkf_expand_label(conn->ctx.hp, &outlen,
	                            conn->ctx.client_initial_secret, sizeof conn->ctx.client_initial_secret,
	                            label, sizeof label - 1))
	    return 0;

	hexdump(conn->ctx.hp, outlen, "HP:\n");
	return outlen;
}

/*
 * Initialize the client crytographic secrets for a new connection.
 * Must be called after having received a new QUIC client Initial packet.
 * Return 1 if succeeded, 0 if not.
 */
static int quic_client_setup_crypto_ctx(struct quic_conn *conn)
{
	if (!quic_derive_initial_secret(conn) ||
	    !quic_derive_client_initial_secret(conn) ||
	    !quic_derive_key(conn) ||
	    !quic_derive_iv(conn) ||
	    !quic_derive_hp(conn))
		return 0;

	return 1;
}


uint64_t *quic_max_pn(struct quic_conn *conn, int server, int long_header, int packet_type)
{
	/* Packet number space */
    int pn_space;

    if (long_header && packet_type == QUIC_PACKET_TYPE_INITIAL) {
        pn_space = 0;
    } else if (long_header && packet_type == QUIC_PACKET_TYPE_HANDSHAKE) {
        pn_space = 1;
    } else {
        pn_space = 2;
    }

    if (server) {
        return &conn->server_max_pn[pn_space];
    } else {
        return &conn->client_max_pn[pn_space];
    }
}

/*
 * See https://quicwg.org/base-drafts/draft-ietf-quic-transport.html#packet-encoding
 * The comments come from this draft.
 */
uint64_t decode_packet_number(uint64_t largest_pn, uint32_t truncated_pn, unsigned int pn_nbits)
{
   uint64_t expected_pn = largest_pn + 1;
   uint64_t pn_win = (uint64_t)1 << pn_nbits;
   uint64_t pn_hwin = pn_win / 2;
   uint64_t pn_mask = pn_win - 1;
   uint64_t candidate_pn;


   // The incoming packet number should be greater than
   // expected_pn - pn_hwin and less than or equal to
   // expected_pn + pn_hwin
   //
   // This means we can't just strip the trailing bits from
   // expected_pn and add the truncated_pn because that might
   // yield a value outside the window.
   //
   // The following code calculates a candidate value and
   // makes sure it's within the packet number window.
   candidate_pn = (expected_pn & ~pn_mask) | truncated_pn;
   if (candidate_pn + pn_hwin <= expected_pn)
      return candidate_pn + pn_win;

   // Note the extra check for underflow when candidate_pn
   // is near zero.
   if (candidate_pn > expected_pn + pn_hwin && candidate_pn > pn_win)
      return candidate_pn - pn_win;

   return candidate_pn;
}

static int quic_remove_header_protection(struct quic_conn *conn, struct quic_packet *pkt,
                                         unsigned char *pn, unsigned char *byte0, const unsigned char *end)
{
	int outlen, i, pnlen;
	uint64_t *largest_pn, packet_number;
	uint32_t truncated_pn = 0;
	unsigned char mask[16] = {0};
	unsigned char *sample;
	EVP_CIPHER_CTX *ctx;

	ctx = EVP_CIPHER_CTX_new();
	if (!ctx)
		return 0;

	sample = pn + QUIC_PACKET_PN_MAXLEN;

	/*
	 * May be required for ECB?:
	 * EVP_CIPHER_CTX_set_padding(ctx, 0);
	 */

	hexdump(sample, 16, "packet sample:\n");
	if (!EVP_DecryptInit_ex(ctx, EVP_aes_128_ctr(), NULL, conn->ctx.hp, NULL))
		goto out;

	EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, 16, NULL);

	if (!EVP_DecryptInit_ex(ctx, NULL, NULL, NULL, sample) ||
	    !EVP_DecryptUpdate(ctx, mask, &outlen, mask, sizeof mask) ||
	    !EVP_DecryptFinal_ex(ctx, mask, &outlen))
	    goto out;


	*byte0 ^= mask[0] & (pkt->type == QUIC_PACKET_TYPE_INITIAL ? 0xf : 0x1f);
	pnlen = (*byte0 & QUIC_PACKET_PNL_BITMASK) + 1;
	for (i = 0; i < pnlen; i++) {
		pn[i] ^= mask[i + 1];
		truncated_pn = (truncated_pn << 8) | pn[i];
	}

	largest_pn = quic_max_pn(conn, 0, *byte0 & QUIC_PACKET_LONG_HEADER_BIT, pkt->type);
	packet_number = decode_packet_number(*largest_pn, truncated_pn, pnlen * 8);
	/* Store remaining information for this unprotected header */
	pkt->pn = packet_number;
	pkt->pnl = pnlen;
	fprintf(stderr, "%s packet_number: %lu\n", __func__, packet_number);

 out:
	EVP_CIPHER_CTX_free(ctx);

	return outlen;
}

static void quic_aead_iv_build(struct quic_conn *conn, uint64_t pn, uint32_t pnl)
{
	int i;
	unsigned int shift;
	unsigned char *iv = conn->ctx.iv;
	unsigned char *aead_iv = conn->ctx.aead_iv;
	size_t iv_size = sizeof conn->ctx.iv;

	for (i = 0; i < iv_size - sizeof pn; i++)
		*aead_iv++ = *iv++;

	shift = 56;
	for (i = iv_size - sizeof pn; i < iv_size; i++, shift -= 8)
		*aead_iv++ = *iv++ ^ (pn >> shift);
	hexdump(conn->ctx.aead_iv, iv_size, "BUILD IV:\n");
}

/*
 * https://quicwg.org/base-drafts/draft-ietf-quic-tls.html#aead
 *
 * 5.3. AEAD Usage
 *
 * Packets are protected prior to applying header protection (Section 5.4).
 * The unprotected packet header is part of the associated data (A). When removing
 * packet protection, an endpoint first removes the header protection.
 * (...)
 * These ciphersuites have a 16-byte authentication tag and produce an output 16
 * bytes larger than their input.
 * The key and IV for the packet are computed as described in Section 5.1. The nonce,
 * N, is formed by combining the packet protection IV with the packet number. The 62
 * bits of the reconstructed QUIC packet number in network byte order are left-padded
 * with zeros to the size of the IV. The exclusive OR of the padded packet number and
 * the IV forms the AEAD nonce.
 *
 * The associated data, A, for the AEAD is the contents of the QUIC header, starting
 * from the flags byte in either the short or long header, up to and including the
 * unprotected packet number.
 *
 * The input plaintext, P, for the AEAD is the payload of the QUIC packet, as described
 * in [QUIC-TRANSPORT].
 *
 * The output ciphertext, C, of the AEAD is transmitted in place of P.
 *
 * Some AEAD functions have limits for how many packets can be encrypted under the same
 * key and IV (see for example [AEBounds]). This might be lower than the packet number limit.
 * An endpoint MUST initiate a key update (Section 6) prior to exceeding any limit set for
 * the AEAD that is in use.
 */
static int quic_decrypt_payload(struct quic_conn *conn, struct quic_packet *pkt,
                                unsigned char *pn, unsigned char *buf, const unsigned char *end)
{
	int algo;
	int  outlen, payload_len, aad_len;
	unsigned char *payload;
	size_t off;

	EVP_CIPHER_CTX *ctx;

	algo = pkt->type == QUIC_PACKET_TYPE_INITIAL ? TLS1_3_CK_AES_128_GCM_SHA256 : conn->aead_algo;

	aad_len = pn + pkt->pnl - buf;

	/* The payload is after the Packet Number field. */
	payload = pn + pkt->pnl;
	payload_len = pkt->len - pkt->pnl;
	off = 0;

	hexdump(payload, payload_len, "Payload to decrypt:\n");

	quic_aead_iv_build(conn, pkt->pn, pkt->pnl);

	ctx = EVP_CIPHER_CTX_new();
	switch (algo) {
		case TLS1_3_CK_AES_128_GCM_SHA256:
			if (!EVP_DecryptInit_ex(ctx, EVP_aes_128_gcm(), NULL, conn->ctx.key, conn->ctx.aead_iv) ||
			    !EVP_DecryptUpdate(ctx, NULL, &outlen, buf, aad_len) ||
			    !EVP_DecryptUpdate(ctx, payload, &outlen, payload, payload_len - 16))
			    return -1;

			off += outlen;

			if (!EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, 16, payload + payload_len - 16) ||
			    !EVP_DecryptFinal_ex(ctx, payload + off, &outlen))
				return -1;

			off += outlen;

			hexdump(payload, off, "Decrypted payload(%zu):\n", off);
			break;
	}

	EVP_CIPHER_CTX_free(ctx);

	return 1;
}

ssize_t quic_packet_read_header(struct quic_packet *qpkt,
                                 unsigned char **buf, const unsigned char *end,
                                 struct listener *l)
{
	unsigned char *beg;
	unsigned char dcid_len, scid_len;
	uint64_t len;
	unsigned char *pn = NULL; /* Packet number */
	struct quic_conn *conn;

	if (end - *buf <= QUIC_PACKET_MINLEN)
		goto err;

	/* Fixed bit */
	if (!(**buf & QUIC_PACKET_FIXED_BIT))
		/* XXX TO BE DISCARDED */
		goto err;

	beg = *buf;
	/* Header form */
	qpkt->long_header = **buf & QUIC_PACKET_LONG_HEADER_BIT;
	/* Packet type */
	qpkt->type = (*(*buf)++ >> QUIC_PACKET_TYPE_SHIFT) & QUIC_PACKET_TYPE_BITMASK;
	/* Version */
	if (!quic_read_uint32(&qpkt->version, (const unsigned char **)buf, end))
		goto err;

	if (!qpkt->version) { /* XXX TO DO XXX Version negotiation packet */ };

	if (qpkt->long_header) {
		/* Destination Connection ID Length */
		dcid_len = *(*buf)++;
		/* We want to be sure we can read <dcid_len> bytes and one more for <scid_len> value */
		if (dcid_len > QUIC_CID_MAXLEN || end - *buf < dcid_len + 1)
			/* XXX MUST BE DROPPED */
			goto err;

		if (dcid_len)
			memcpy(qpkt->dcid.data, *buf, dcid_len);
		qpkt->dcid.len = dcid_len;
		*buf += dcid_len;

		if (qpkt->dcid.len)
			hexdump(qpkt->dcid.data, qpkt->dcid.len, "\n%s: DCID:\n", __func__);

		/* Source Connection ID Length */
		scid_len = *(*buf)++;
		if (scid_len > QUIC_CID_MAXLEN || end - *buf < scid_len)
			/* XXX MUST BE DROPPED */
			goto err;

		if (scid_len)
			memcpy(qpkt->scid.data, *buf, scid_len);
		qpkt->scid.len = scid_len;
		*buf += scid_len;

		if (qpkt->dcid.len) {
			struct ebmb_node *node;

			node = ebmb_lookup(&l->quic_clients, qpkt->dcid.data, qpkt->dcid.len);
			if (!node) {

				conn = calloc(1, sizeof *conn + qpkt->dcid.len);
				if (conn) {
					conn->cid_len = qpkt->dcid.len;
					memcpy(conn->cid.key, qpkt->dcid.data, qpkt->dcid.len);
					ebmb_insert(&l->quic_clients, &conn->cid, conn->cid_len);
				}
			}
			else {
				conn = ebmb_entry(node, struct quic_conn, cid);
			}
		}
	}
	else {
		/* XXX TO DO: Short header XXX */
	}

	if (qpkt->type == QUIC_PACKET_TYPE_INITIAL) {
		uint64_t token_len;

		fprintf(stderr, "QUIC_PACKET_TYPE_INITIAL packet\n");
		token_len = quic_dec_int((const unsigned char **)buf, end);
		if (token_len == -1 || end - *buf < token_len)
			goto err;

		/* XXX TO DO XXX 0 value means "the token is not present".
		 * A server which sends an Initial packet must not set the token.
		 * So, a client which receives an Initial packet with a token
		 * MUST discard the packet or generate a connection error with
		 * PROTOCOL_VIOLATION as type.
		 * The token must be provided in a Retry packet or NEW_TOKEN frame.
		 */
		qpkt->token_len = token_len;

		quic_client_setup_crypto_ctx(conn);
	}

	if (qpkt->type != QUIC_PACKET_TYPE_RETRY && qpkt->version) {
		len = quic_dec_int((const unsigned char **)buf, end);
		if (len == -1 || end - *buf < len)
			goto err;

		qpkt->len = len;
		/*
		 * The packet number is here. This is also the start minus QUIC_PACKET_PN_MAXLEN
		 * of the sample used to add/remove the header protection.
		 */
		pn = *buf;

		hexdump(pn, 2, "Packet Number two first bytes:\n");
		if (qpkt->type == QUIC_PACKET_TYPE_INITIAL) {
			quic_remove_header_protection(conn, qpkt, pn, beg, end);
			quic_decrypt_payload(conn, qpkt, pn, beg, end);
		}
	}

	fprintf(stderr, "\ttoken_len: %lu len: %lu pnl: %u\n",
	        qpkt->token_len, qpkt->len, qpkt->pnl);

	return *buf - beg;

 err:
	return -1;
}

ssize_t quic_packets_read(char *buf, size_t len, struct listener *l)
{
	unsigned char *pos;
	const unsigned char *end;
	struct quic_packet qpkt = {0};

	pos = (unsigned char *)buf;
	end = pos + len;

	if (quic_packet_read_header(&qpkt, &pos, end, l) == -1)
		goto err;

	/* XXX Servers SHOULD be able to read longer (than QUIC_CID_MAXLEN)
	 * connection IDs from other QUIC versions in order to properly form a
	 * version negotiation packet.
	 */

    /* https://tools.ietf.org/pdf/draft-ietf-quic-transport-22.pdf#53:
     *
	 * Valid packets sent to clients always include a Destination Connection
     * ID that matches a value the client selects.  Clients that choose to
     * receive zero-length connection IDs can use the address/port tuple to
     * identify a connection.  Packets that don’t match an existing
     * connection are discarded.
     */
	fprintf(stderr, "long header? %d packet type: 0x%02x version: 0x%08x\n",
	        !!qpkt.long_header, qpkt.type, qpkt.version);

	return pos - (unsigned char *)buf;

 err:
	return -1;
}

/* XXX TODO: adapt these comments */
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

size_t quic_conn_to_buf(int fd, void *ctx)
{
	ssize_t ret;
	size_t done = 0;
	struct listener *l = ctx;

	if (!fd_recv_ready(fd))
		return 0;

	if (unlikely(!(fdtab[fd].ev & FD_POLL_IN))) {
		/* report error on POLL_ERR before connection establishment */
		if ((fdtab[fd].ev & FD_POLL_ERR))
			goto out;
	}

	do {
		ret = recvfrom(fd, trash.area, trash.size, 0, NULL, 0);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN)
				fd_cant_recv(fd);
			break;
		}
		else {
			hexdump(trash.area, 64, "%s: recvfrom()\n", __func__);
			done = trash.data = ret;
			/*
			 * Senders MUST NOT coalesce QUIC packets for different connections into a single
			 * UDP datagram. Receivers SHOULD ignore any subsequent packets with a different
			 * Destination Connection ID than the first packet in the datagram.
			 */
			quic_packets_read(trash.area, trash.size, l);
		}
	} while (0);

 out:
	return done;
}


/* XXX TODO: adapt these comments */
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
__attribute__((unused))
static size_t quic_conn_from_buf(int fd, void *xprt_ctx, const struct buffer *buf, size_t count, int flags)
{
	ssize_t ret;
	size_t try, done;
	int send_flag;

	fprintf(stderr, "# %s ctx @%p\n", __func__, xprt_ctx);

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

		ret = send(fd, b_peek(buf, done), try, send_flag);

		if (ret > 0) {
			count -= ret;
			done += ret;

			/* if the system buffer is full, don't insist */
			if (ret < try)
				break;
		}
		else if (ret == 0 || errno == EAGAIN) {
			/* nothing written, we need to poll for write first */
			fd_cant_send(fd);
			break;
		}
		else if (errno != EINTR) {
			/* XXX TODO */
			break;
		}
	}

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

