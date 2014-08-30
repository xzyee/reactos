/* des.h
 *
 * The des block cipher. And triple des.
 */

/* nettle, low-level cryptographics library
 *
 * Copyright (C) 1992, 2001, Dana L. How, Niels Möller
 * 
 * The nettle library is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at your
 * option) any later version.
 * 
 * The nettle library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public License
 * along with the nettle library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02111-1301, USA.
 */

/*
 *	des - fast & portable DES encryption & decryption.
 *	Copyright (C) 1992  Dana L. How
 *	Please see the file `../lib/descore.README' for the complete copyright
 *	notice.
 *
 * Slightly edited by Niels Möller, 1997
 */

#ifndef NETTLE_DES_H_INCLUDED
#define NETTLE_DES_H_INCLUDED

#include "nettle-types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Namespace mangling */
#define des_set_key nettle_des_set_key
#define des_encrypt nettle_des_encrypt
#define des_decrypt nettle_des_decrypt
#define des_check_parity nettle_des_check_parity
#define des_fix_parity nettle_des_fix_parity
#define des3_set_key nettle_des3_set_key
#define des3_encrypt nettle_des3_encrypt
#define des3_decrypt nettle_des3_decrypt

#define DES_KEY_SIZE 8
#define DES_BLOCK_SIZE 8

/* Expanded key length */
#define _DES_KEY_LENGTH 32

	struct des_ctx {
		uint32_t key[_DES_KEY_LENGTH];
	};

/* Returns 1 for good keys and 0 for weak keys. */
	int
	 des_set_key(struct des_ctx *ctx, const uint8_t * key);

	void
	 des_encrypt(const struct des_ctx *ctx,
		     unsigned length, uint8_t * dst, const uint8_t * src);
	void
	 des_decrypt(const struct des_ctx *ctx,
		     unsigned length, uint8_t * dst, const uint8_t * src);

	int
	 des_check_parity(unsigned length, const uint8_t * key);

	void
	 des_fix_parity(unsigned length, uint8_t * dst,
			const uint8_t * src);

#define DES3_KEY_SIZE 24
#define DES3_BLOCK_SIZE DES_BLOCK_SIZE

	struct des3_ctx {
		struct des_ctx des[3];
	};


/* Returns 1 for good keys and 0 for weak keys. */
	int
	 des3_set_key(struct des3_ctx *ctx, const uint8_t * key);

	void
	 des3_encrypt(const struct des3_ctx *ctx,
		      unsigned length, uint8_t * dst, const uint8_t * src);
	void
	 des3_decrypt(const struct des3_ctx *ctx,
		      unsigned length, uint8_t * dst, const uint8_t * src);

#ifdef __cplusplus
}
#endif
#endif				/* NETTLE_DES_H_INCLUDED */
