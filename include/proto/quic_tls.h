/*
 * include/proto/quic_tls.h
 * This file provides definitions for QUIC-TLS.
 *
 * Copyright 2019 HAProxy Technologies, Frédéric Lécaille <flecaille@haproxy.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#ifndef _PROTO_QUIC_TLS_H
#define _PROTO_QUIC_TLS_H

#include <stdlib.h>
#include <openssl/ssl.h>

#include <types/quic_tls.h>

int quic_derive_initial_secret(const EVP_MD *md,
                               unsigned char *initial_secret, size_t initial_secret_sz,
                               const unsigned char *secret, size_t secret_sz);

int quic_tls_derive_initial_secrets(const EVP_MD *md,
                                    unsigned char *rx, size_t rx_sz,
                                    unsigned char *tx, size_t tx_sz,
                                    const unsigned char *secret, size_t secret_sz,
                                    int server);

int quic_tls_derive_packet_protection_keys(const EVP_CIPHER *aead, const EVP_MD *md,
                                           unsigned char *key, size_t keylen,
                                           unsigned char *iv, size_t ivlen,
                                           unsigned char *hp_key, size_t hp_keylen,
                                           const unsigned char *secret, size_t secretlen);

static inline const EVP_CIPHER *tls_aead(const SSL_CIPHER *cipher)
{
	switch (SSL_CIPHER_get_id(cipher)) {
	case TLS1_3_CK_AES_128_GCM_SHA256:
		return EVP_aes_128_gcm();
	case TLS1_3_CK_AES_256_GCM_SHA384:
		return EVP_aes_256_gcm();
#ifndef OPENSSL_IS_BORINGSSL
	/* XXX TO DO XXX */
    /* Note that for chacha20_poly1305, there exists EVP_AEAD_chacha20_poly135() function
     * which returns a pointer to const EVP_AEAD.
     */
	case TLS1_3_CK_CHACHA20_POLY1305_SHA256:
		return EVP_chacha20_poly1305();
	case TLS1_3_CK_AES_128_CCM_SHA256:
		return EVP_aes_128_ccm();
#endif
	default:
		return NULL;
	}
}

static inline const EVP_MD *tls_md(const SSL_CIPHER *cipher)
{
	switch (SSL_CIPHER_get_id(cipher)) {
	case TLS1_3_CK_AES_128_GCM_SHA256:
#ifndef OPENSSL_IS_BORINGSSL
	/* XXX TO DO XXX */
    /* Note that for chacha20_poly1305, there exists EVP_AEAD_chacha20_poly135() function
     * which returns a pointer to const EVP_AEAD.
     */
	case TLS1_3_CK_AES_128_CCM_SHA256:
	case TLS1_3_CK_CHACHA20_POLY1305_SHA256:
#endif
		return EVP_sha256();
	case TLS1_3_CK_AES_256_GCM_SHA384:
		return EVP_sha384();
	default:
		return NULL;
	}
}

static inline const EVP_CIPHER *tls_hp(const SSL_CIPHER *cipher)
{
	switch (SSL_CIPHER_get_id(cipher)) {
#ifndef OPENSSL_IS_BORINGSSL
	/* XXX TO DO XXX */
    /* Note that for chacha20_poly1305, there exists EVP_AEAD_chacha20_poly135() function
     * which returns a pointer to const EVP_AEAD.
     */
	case TLS1_3_CK_CHACHA20_POLY1305_SHA256:
		return EVP_chacha20();
	case TLS1_3_CK_AES_128_CCM_SHA256:
#endif
	case TLS1_3_CK_AES_128_GCM_SHA256:
		return EVP_aes_128_ctr();
	case TLS1_3_CK_AES_256_GCM_SHA384:
		return EVP_aes_256_ctr();
	default:
		return NULL;
	}

}

/* These two following functions map TLS implementation encryption level to ours */
static inline int ssl_to_quic_enc_level(int level)
{
	switch (level) {
	case ssl_encryption_initial:
		return QUIC_TLS_ENC_LEVEL_INITIAL;
	case ssl_encryption_early_data:
		return QUIC_TLS_ENC_LEVEL_EARLY_DATA;
	case ssl_encryption_handshake:
		return QUIC_TLS_ENC_LEVEL_HANDSHAKE;
	case ssl_encryption_application:
		return QUIC_TLS_ENC_LEVEL_APP;
	default:
		return -1;
	}
}

static inline int quic_to_ssl_enc_level(int level)
{
	switch (level) {
	case QUIC_TLS_ENC_LEVEL_INITIAL:
		return ssl_encryption_initial;
	case QUIC_TLS_ENC_LEVEL_EARLY_DATA:
		return ssl_encryption_early_data;
	case QUIC_TLS_ENC_LEVEL_HANDSHAKE:
		return ssl_encryption_handshake;
	case QUIC_TLS_ENC_LEVEL_APP:
		return ssl_encryption_application;
	default:
		return -1;
	}
}

/*
 * Initialize a TLS cryptographic context for the Initial encryption level.
 */
static inline void quic_initial_tls_ctx_init(struct quic_tls_ctx *ctx)
{
	ctx->aead = EVP_aes_128_gcm();
	ctx->md = EVP_sha256();
}

#endif /* _PROTO_QUIC_TLS_H */

