/*
 * Copyright (c) 2014-2015 The Linux Foundation. All rights reserved.
 *
 * Previously licensed under the ISC license by Qualcomm Atheros, Inc.
 *
 *
 * Permission to use, copy, modify, and/or distribute this software for
 * any purpose with or without fee is hereby granted, provided that the
 * above copyright notice and this permission notice appear in all
 * copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * This file was originally distributed by Qualcomm Atheros, Inc.
 * under proprietary terms before Copyright ownership was assigned
 * to the Linux Foundation.
 */

/*============================================================================
   FILE:         cds_utils.c

   OVERVIEW:     This source file contains definitions for CDS crypto APIs
                The four APIs mentioned in this file are used for
                initializing, and de-initializing a crypto context, and
                obtaining truly random data (for keys), as well as
                SHA1 HMAC, and AES encrypt and decrypt routines.

                The routines include:
                cds_crypto_init() - Initializes Crypto module
                cds_crypto_deinit() - De-initializes Crypto module
                cds_rand_get_bytes() - Generates random byte
                cds_sha1_hmac_str() - Generate the HMAC-SHA1 of a string given a key
                cds_encrypt_aes() - Generate AES Encrypted byte stream
                cds_decrypt_aes() - Decrypts an AES Encrypted byte stream

   DEPENDENCIES:
   ============================================================================*/

/*----------------------------------------------------------------------------
 * Include Files
 * -------------------------------------------------------------------------*/

#include "cdf_trace.h"
#include "cds_utils.h"
#include "cdf_memory.h"
#include "cds_crypto.h"

#include <linux/err.h>
#include <linux/random.h>
#include <linux/crypto.h>
#include <linux/scatterlist.h>
#include <linux/completion.h>
#include <linux/ieee80211.h>
#include <crypto/hash.h>
#include <crypto/aes.h>

#include "cds_ieee80211_common.h"
/*----------------------------------------------------------------------------
 * Preprocessor Definitions and Constants
 * -------------------------------------------------------------------------*/
#define AAD_LEN 20
#define IV_SIZE_AES_128 16
#define CMAC_IPN_LEN 6
#define CMAC_TLEN 8             /* CMAC TLen = 64 bits (8 octets) */

/*----------------------------------------------------------------------------
 * Type Declarations
 * -------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------
 * Global Data Definitions
 * -------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------
 * Static Variable Definitions
 * -------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------
   Function Definitions and Documentation
 * -------------------------------------------------------------------------*/
#ifdef CONFIG_ICNSS
#ifdef WLAN_FEATURE_11W
static inline void xor_128(const u8 *a, const u8 *b, u8 *out)
{
	u8 i;

	for (i = 0; i < AES_BLOCK_SIZE; i++)
		out[i] = a[i] ^ b[i];
}

static inline void leftshift_onebit(const u8 *input, u8 *output)
{
	int i, overflow = 0;

	for (i = (AES_BLOCK_SIZE - 1); i >= 0; i--) {
		output[i] = input[i] << 1;
		output[i] |= overflow;
		overflow = (input[i] & 0x80) ? 1 : 0;
	}
	return;
}

static void generate_subkey(struct crypto_cipher *tfm, u8 *k1, u8 *k2)
{
	u8 l[AES_BLOCK_SIZE], tmp[AES_BLOCK_SIZE];
	u8 const_rb[AES_BLOCK_SIZE] = {
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x87
	};
	u8 const_zero[AES_BLOCK_SIZE] = {
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	};

	crypto_cipher_encrypt_one(tfm, l, const_zero);

	if ((l[0] & 0x80) == 0) {       /* If MSB(l) = 0, then k1 = l << 1 */
		leftshift_onebit(l, k1);
	} else {                /* Else k1 = ( l << 1 ) (+) Rb */
		leftshift_onebit(l, tmp);
		xor_128(tmp, const_rb, k1);
	}

	if ((k1[0] & 0x80) == 0) {
		leftshift_onebit(k1, k2);
	} else {
		leftshift_onebit(k1, tmp);
		xor_128(tmp, const_rb, k2);
	}
}

static inline void padding(u8 *lastb, u8 *pad, u16 length)
{
	u8 j;

	/* original last block */
	for (j = 0; j < AES_BLOCK_SIZE; j++) {
		if (j < length)
			pad[j] = lastb[j];
		else if (j == length)
			pad[j] = 0x80;
		else
			pad[j] = 0x00;
	}
}

static void cds_cmac_calc_mic(struct crypto_cipher *tfm,
		u8 *m, u16 length, u8 *mac)
{
	u8 x[AES_BLOCK_SIZE], y[AES_BLOCK_SIZE];
	u8 m_last[AES_BLOCK_SIZE], padded[AES_BLOCK_SIZE];
	u8 k1[AES_KEYSIZE_128], k2[AES_KEYSIZE_128];
	int cmpBlk;
	int i, nBlocks = (length + 15) / AES_BLOCK_SIZE;

	generate_subkey(tfm, k1, k2);

	if (nBlocks == 0) {
		nBlocks = 1;
		cmpBlk = 0;
	} else {
		cmpBlk = ((length % AES_BLOCK_SIZE) == 0) ? 1 : 0;
	}

	if (cmpBlk) {           /* Last block is complete block */
		xor_128(&m[AES_BLOCK_SIZE * (nBlocks - 1)], k1, m_last);
	} else {                /* Last block is not complete block */
		padding(&m[AES_BLOCK_SIZE * (nBlocks - 1)], padded,
			length % AES_BLOCK_SIZE);
		xor_128(padded, k2, m_last);
	}

	for (i = 0; i < AES_BLOCK_SIZE; i++)
		x[i] = 0;

	for (i = 0; i < (nBlocks - 1); i++) {
		xor_128(x, &m[AES_BLOCK_SIZE * i], y);  /* y = Mi (+) x */
		crypto_cipher_encrypt_one(tfm, x, y);   /* x = AES-128(KEY, y) */
	}

	xor_128(x, m_last, y);
	crypto_cipher_encrypt_one(tfm, x, y);

	memcpy(mac, x, CMAC_TLEN);
}
#endif
#endif

/*--------------------------------------------------------------------------

   \brief cds_crypto_init() - Initializes Crypto module

   The cds_crypto_init() function initializes Crypto module.

   \param phCryptProv - pointer to the Crypt handle

   \return CDF_STATUS_SUCCESS - Successfully generated random memory.

          CDF_STATUS_E_FAULT  - pbBuf is an invalid pointer.

          CDF_STATUS_E_FAILURE - default return value if it fails due to
          unknown reasons

  ***CDF_STATUS_E_RESOURCES - System resources (other than memory)
          are unavailable
   \sa

    ( *** return value not considered yet )
   --------------------------------------------------------------------------*/
CDF_STATUS cds_crypto_init(uint32_t *phCryptProv)
{
	CDF_STATUS uResult = CDF_STATUS_E_FAILURE;

	/* This implementation doesn't require a crypto context */
	*phCryptProv = 0;
	uResult = CDF_STATUS_SUCCESS;
	return (uResult);
}

CDF_STATUS cds_crypto_deinit(uint32_t hCryptProv)
{
	CDF_STATUS uResult = CDF_STATUS_E_FAILURE;

	/* CryptReleaseContext succeeded */
	uResult = CDF_STATUS_SUCCESS;

	return (uResult);
}

/*--------------------------------------------------------------------------

   \brief cds_rand_get_bytes() - Generates random byte

   The cds_rand_get_bytes() function generate random bytes.

   Buffer should be allocated before calling cds_rand_get_bytes().

   Attempting to initialize an already initialized lock results in
   a failure.

   \param lock - pointer to the opaque lock object to initialize

   \return CDF_STATUS_SUCCESS - Successfully generated random memory.

          CDF_STATUS_E_FAULT  - pbBuf is an invalid pointer.

          CDF_STATUS_E_FAILURE - default return value if it fails due to
          unknown reasons

  ***CDF_STATUS_E_RESOURCES - System resources (other than memory)
          are unavailable
   \sa

    ( *** return value not considered yet )
   --------------------------------------------------------------------------*/
CDF_STATUS
cds_rand_get_bytes(uint32_t cryptHandle, uint8_t *pbBuf, uint32_t numBytes)
{
	CDF_STATUS uResult = CDF_STATUS_E_FAILURE;

	/* check for invalid pointer */
	if (NULL == pbBuf) {
		uResult = CDF_STATUS_E_FAULT;
		return (uResult);
	}

	get_random_bytes(pbBuf, numBytes);
	/* "Random sequence generated." */
	uResult = CDF_STATUS_SUCCESS;
	return (uResult);
}

#ifdef WLAN_FEATURE_11W
uint8_t cds_get_mmie_size()
{
	return sizeof(struct ieee80211_mmie);
}

/*--------------------------------------------------------------------------

   \brief cds_increase_seq() - Increase the IPN aka Sequence number by one unit

   The cds_increase_seq() function increases the IPN by one unit.

   \param ipn - pointer to the IPN aka Sequence number [6 bytes]

   --------------------------------------------------------------------------*/
static void cds_increase_seq(uint8_t *ipn)
{
	uint64_t value = 0;
	if (ipn) {
		value = (0xffffffffffff) & (*((uint64_t *) ipn));
		value = value + 1;
		cdf_mem_copy(ipn, &value, IEEE80211_MMIE_IPNLEN);
	}
}

/*--------------------------------------------------------------------------

   \brief cds_attach_mmie() - attches the complete MMIE at the end of frame

   The cds_attach_mmie() calculates the entire MMIE and attaches at the end
   of Broadcast/Multicast robust management frames.

   \param igtk - pointer  group key which will be used to calculate
                the 8 byte MIC.
   \param ipn - pointer ipn, it is also known as sequence number
   \param key_id - key identication number
   \param frm - pointer to the start of the frame.
   \param efrm - pointer to the end of the frame.
   \param frmLen - size of the entire frame.

   \return - this function will return true on success and false on
            failure.

   --------------------------------------------------------------------------*/

bool
cds_attach_mmie(uint8_t *igtk, uint8_t *ipn, uint16_t key_id,
		uint8_t *frm, uint8_t *efrm, uint16_t frmLen)
{
	struct ieee80211_mmie *mmie;
	struct ieee80211_frame *wh;
	uint8_t aad[AAD_LEN], mic[CMAC_TLEN], *input = NULL;
	uint8_t previous_ipn[IEEE80211_MMIE_IPNLEN] = { 0 };
	uint16_t nBytes = 0;
	int ret = 0;
	struct crypto_cipher *tfm;

	/*  This is how received frame look like
	 *
	 *        <------------frmLen---------------------------->
	 *
	 *        +---------------+----------------------+-------+
	 *        | 802.11 HEADER | Management framebody | MMIE  |
	 *        +---------------+----------------------+-------+
	 *                                                       ^
	 *                                                       |
	 *                                                      efrm
	 *   This is how MMIE from above frame look like
	 *
	 *
	 *        <------------ 18 Bytes----------------------------->
	 *        +--------+---------+---------+-----------+---------+
	 *        |Element | Length  | Key id  |   IPN     |  MIC    |
	 *        |  id    |         |         |           |         |
	 *        +--------+---------+---------+-----------+---------+
	 * Octet     1         1         2         6            8
	 *
	 */

	/* Check if frame is invalid length */
	if (((efrm - frm) != frmLen) || (frmLen < sizeof(*wh))) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "%s: Invalid frame length", __func__);
		return false;
	}
	mmie = (struct ieee80211_mmie *)(efrm - sizeof(*mmie));

	/* Copy Element id */
	mmie->element_id = IEEE80211_ELEMID_MMIE;

	/* Copy Length */
	mmie->length = sizeof(*mmie) - 2;

	/* Copy Key id */
	mmie->key_id = key_id;

	/*
	 * In case of error, revert back to original IPN
	 * to do that copy the original IPN into previous_ipn
	 */
	cdf_mem_copy(&previous_ipn[0], ipn, IEEE80211_MMIE_IPNLEN);
	cds_increase_seq(ipn);
	cdf_mem_copy(mmie->sequence_number, ipn, IEEE80211_MMIE_IPNLEN);

	/*
	 * Calculate MIC and then copy
	 */
	tfm = cds_crypto_alloc_cipher("aes", 0, CRYPTO_ALG_ASYNC);
	if (IS_ERR(tfm)) {
		ret = PTR_ERR(tfm);
		tfm = NULL;
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "%s: crypto_alloc_cipher failed (%d)", __func__, ret);
		goto err_tfm;
	}

	ret = crypto_cipher_setkey(tfm, igtk, AES_KEYSIZE_128);
	if (ret) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "%s: crypto_cipher_setkey failed (%d)", __func__,
			  ret);
		goto err_tfm;
	}

	/* Construct AAD */
	wh = (struct ieee80211_frame *)frm;

	/* Generate BIP AAD: FC(masked) || A1 || A2 || A3 */

	/* FC type/subtype */
	aad[0] = wh->i_fc[0];
	/* Mask FC Retry, PwrMgt, MoreData flags to zero */
	aad[1] = wh->i_fc[1] & ~(IEEE80211_FC1_RETRY | IEEE80211_FC1_PWR_MGT |
				 IEEE80211_FC1_MORE_DATA);
	/* A1 || A2 || A3 */
	cdf_mem_copy(aad + 2, wh->i_addr_all, 3 * IEEE80211_ADDR_LEN);

	/* MIC = AES-128-CMAC(IGTK, AAD || Management Frame Body || MMIE, 64) */
	nBytes = AAD_LEN + (frmLen - sizeof(struct ieee80211_frame));
	input = (uint8_t *) cdf_mem_malloc(nBytes);
	if (NULL == input) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "%s: Memory allocation failed", __func__);
		ret = CDF_STATUS_E_NOMEM;
		goto err_tfm;
	}

	/*
	 * Copy the AAD, Management frame body, and
	 * MMIE with 8 bit MIC zeroed out
	 */
	cdf_mem_zero(input, nBytes);
	cdf_mem_copy(input, aad, AAD_LEN);
	/* Copy Management Frame Body and MMIE without MIC */
	cdf_mem_copy(input + AAD_LEN,
		     (uint8_t *) (efrm -
				  (frmLen - sizeof(struct ieee80211_frame))),
		     nBytes - AAD_LEN - CMAC_TLEN);

	cds_cmac_calc_mic(tfm, input, nBytes, mic);
	cdf_mem_free(input);

	CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_INFO_HIGH,
		  "CMAC(T)= %02X %02X %02X %02X %02X %02X %02X %02X",
		  mic[0], mic[1], mic[2], mic[3],
		  mic[4], mic[5], mic[6], mic[7]);
	cdf_mem_copy(mmie->mic, mic, IEEE80211_MMIE_MICLEN);

err_tfm:
	if (ret) {
		cdf_mem_copy(ipn, previous_ipn, IEEE80211_MMIE_IPNLEN);
	}

	if (tfm)
		cds_crypto_free_cipher(tfm);
	return !ret ? true : false;
}

bool
cds_is_mmie_valid(uint8_t *igtk, uint8_t *ipn, uint8_t *frm, uint8_t *efrm)
{
	struct ieee80211_mmie *mmie;
	struct ieee80211_frame *wh;
	uint8_t *rx_ipn, aad[AAD_LEN], mic[CMAC_TLEN], *input;
	uint16_t nBytes = 0;
	int ret = 0;
	struct crypto_cipher *tfm;

	/* Check if frame is invalid length */
	if ((efrm < frm) || ((efrm - frm) < sizeof(*wh))) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "Invalid frame length");
		return false;
	}

	mmie = (struct ieee80211_mmie *)(efrm - sizeof(*mmie));

	/* Check Element ID */
	if ((mmie->element_id != IEEE80211_ELEMID_MMIE) ||
	    (mmie->length != (sizeof(*mmie) - 2))) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "IE is not Mgmt MIC IE or Invalid length");
		/* IE is not Mgmt MIC IE or invalid length */
		return false;
	}

	/* Validate IPN */
	rx_ipn = mmie->sequence_number;
	if (OS_MEMCMP(rx_ipn, ipn, CMAC_IPN_LEN) <= 0) {
		/* Replay error */
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "Replay error mmie ipn %02X %02X %02X %02X %02X %02X"
			  " drvr ipn %02X %02X %02X %02X %02X %02X",
			  rx_ipn[0], rx_ipn[1], rx_ipn[2], rx_ipn[3], rx_ipn[4],
			  rx_ipn[5], ipn[0], ipn[1], ipn[2], ipn[3], ipn[4],
			  ipn[5]);
		return false;
	}
	tfm = cds_crypto_alloc_cipher("aes", 0, CRYPTO_ALG_ASYNC);
	if (IS_ERR(tfm)) {
		ret = PTR_ERR(tfm);
		tfm = NULL;
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "crypto_alloc_cipher failed (%d)", ret);
		goto err_tfm;
	}

	ret = crypto_cipher_setkey(tfm, igtk, AES_KEYSIZE_128);
	if (ret) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "crypto_cipher_setkey failed (%d)", ret);
		goto err_tfm;
	}

	/* Construct AAD */
	wh = (struct ieee80211_frame *)frm;

	/* Generate BIP AAD: FC(masked) || A1 || A2 || A3 */

	/* FC type/subtype */
	aad[0] = wh->i_fc[0];
	/* Mask FC Retry, PwrMgt, MoreData flags to zero */
	aad[1] = wh->i_fc[1] & ~(IEEE80211_FC1_RETRY | IEEE80211_FC1_PWR_MGT |
				 IEEE80211_FC1_MORE_DATA);
	/* A1 || A2 || A3 */
	cdf_mem_copy(aad + 2, wh->i_addr_all, 3 * IEEE80211_ADDR_LEN);

	/* MIC = AES-128-CMAC(IGTK, AAD || Management Frame Body || MMIE, 64) */
	nBytes = AAD_LEN + (efrm - (uint8_t *) (wh + 1));
	input = (uint8_t *) cdf_mem_malloc(nBytes);
	if (NULL == input) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "Memory allocation failed");
		ret = CDF_STATUS_E_NOMEM;
		goto err_tfm;
	}

	/* Copy the AAD, MMIE with 8 bit MIC zeroed out */
	cdf_mem_zero(input, nBytes);
	cdf_mem_copy(input, aad, AAD_LEN);
	cdf_mem_copy(input + AAD_LEN, (uint8_t *) (wh + 1),
		     nBytes - AAD_LEN - CMAC_TLEN);

	cds_cmac_calc_mic(tfm, input, nBytes, mic);
	cdf_mem_free(input);

	CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
		  "CMAC(T)= %02X %02X %02X %02X %02X %02X %02X %02X",
		  mic[0], mic[1], mic[2], mic[3],
		  mic[4], mic[5], mic[6], mic[7]);

	if (OS_MEMCMP(mic, mmie->mic, CMAC_TLEN) != 0) {
		/* MMIE MIC mismatch */
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "BC/MC MGMT frame MMIE MIC check Failed"
			  " rmic %02X %02X %02X %02X %02X %02X %02X %02X"
			  " cmic %02X %02X %02X %02X %02X %02X %02X %02X",
			  mmie->mic[0], mmie->mic[1], mmie->mic[2],
			  mmie->mic[3], mmie->mic[4], mmie->mic[5],
			  mmie->mic[6], mmie->mic[7], mic[0], mic[1], mic[2],
			  mic[3], mic[4], mic[5], mic[6], mic[7]);
		return false;
	}

	/* Update IPN */
	cdf_mem_copy(ipn, rx_ipn, CMAC_IPN_LEN);

err_tfm:
	if (tfm)
		cds_crypto_free_cipher(tfm);

	return !ret ? true : false;
}

#endif /* WLAN_FEATURE_11W */
/**
 * cds_sha1_hmac_str
 *
 * FUNCTION:
 * Generate the HMAC-SHA1 of a string given a key.
 *
 * LOGIC:
 * Standard HMAC processing from RFC 2104. The code is provided in the
 * appendix of the RFC.
 *
 * ASSUMPTIONS:
 * The RFC is correct.
 *
 * @param text text to be hashed
 * @param textLen length of text
 * @param key key to use for HMAC
 * @param keyLen length of key
 * @param digest holds resultant SHA1 HMAC (20B)
 *
 * @return CDF_STATUS_SUCCSS if the operation succeeds
 *
 */

struct hmac_sha1_result {
	struct completion completion;
	int err;
};

static void hmac_sha1_complete(struct crypto_async_request *req, int err)
{
	struct hmac_sha1_result *r = req->data;
	if (err == -EINPROGRESS)
		return;
	r->err = err;
	complete(&r->completion);
}

int
hmac_sha1(uint8_t *key, uint8_t ksize, char *plaintext, uint8_t psize,
	  uint8_t *output, uint8_t outlen)
{
	int ret = 0;
	struct crypto_ahash *tfm;
	struct scatterlist sg;
	struct ahash_request *req;
	struct hmac_sha1_result tresult;
	void *hash_buff = NULL;

	unsigned char hash_result[64];
	int i;

	memset(output, 0, outlen);

	init_completion(&tresult.completion);

	tfm = cds_crypto_alloc_ahash("hmac(sha1)", CRYPTO_ALG_TYPE_AHASH,
				 CRYPTO_ALG_TYPE_AHASH_MASK);
	if (IS_ERR(tfm)) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "crypto_alloc_ahash failed");
		ret = PTR_ERR(tfm);
		goto err_tfm;
	}

	req = ahash_request_alloc(tfm, GFP_KERNEL);
	if (!req) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "failed to allocate request for hmac(sha1)");
		ret = -ENOMEM;
		goto err_req;
	}

	ahash_request_set_callback(req, CRYPTO_TFM_REQ_MAY_BACKLOG,
				   hmac_sha1_complete, &tresult);

	hash_buff = kzalloc(psize, GFP_KERNEL);
	if (!hash_buff) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "failed to kzalloc hash_buff");
		ret = -ENOMEM;
		goto err_hash_buf;
	}

	memset(hash_result, 0, 64);
	memcpy(hash_buff, plaintext, psize);
	sg_init_one(&sg, hash_buff, psize);

	if (ksize) {
		crypto_ahash_clear_flags(tfm, ~0);
		ret = cds_crypto_ahash_setkey(tfm, key, ksize);
		if (ret) {
			CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
				  "crypto_ahash_setkey failed");
			goto err_setkey;
		}
	}

	ahash_request_set_crypt(req, &sg, hash_result, psize);
	ret = cds_crypto_ahash_digest(req);
	CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR, "ret 0x%x", ret);

	switch (ret) {
	case 0:
		for (i = 0; i < outlen; i++)
			output[i] = hash_result[i];
		break;
	case -EINPROGRESS:
	case -EBUSY:
		ret = wait_for_completion_interruptible(&tresult.completion);
		if (!ret && !tresult.err) {
			for (i = 0; i < outlen; i++)
				output[i] = hash_result[i];
			INIT_COMPLETION(tresult.completion);
			break;
		} else {
			CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
				  "wait_for_completion_interruptible failed");
			if (!ret)
				ret = tresult.err;
			goto out;
		}
	default:
		goto out;
	}

out:
err_setkey:
	kfree(hash_buff);
err_hash_buf:
	ahash_request_free(req);
err_req:
	cds_crypto_free_ahash(tfm);
err_tfm:
	return ret;
}

CDF_STATUS cds_sha1_hmac_str(uint32_t cryptHandle,      /* Handle */
			     uint8_t *pText,    /* pointer to data stream */
			     uint32_t textLen,  /* length of data stream */
			     uint8_t *pKey,     /* pointer to authentication key */
			     uint32_t keyLen,   /* length of authentication key */
			     uint8_t digest[CDS_DIGEST_SHA1_SIZE])
{                               /* caller digest to be filled in */
	int ret = 0;

	ret = hmac_sha1(pKey,   /* uint8_t *key, */
			(uint8_t) keyLen,       /* uint8_t ksize, */
			(char *)pText,  /* char *plaintext, */
			(uint8_t) textLen,      /* uint8_t psize, */
			digest, /* uint8_t *output, */
			CDS_DIGEST_SHA1_SIZE    /* uint8_t outlen */
			);

	if (ret != 0) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "hmac_sha1() call failed");
		return CDF_STATUS_E_FAULT;
	}

	return CDF_STATUS_SUCCESS;
}

/**
 * cds_md5_hmac_str
 *
 * FUNCTION:
 * Generate the HMAC-MD5 of a string given a key.
 *
 * LOGIC:
 * Standard HMAC processing from RFC 2104. The code is provided in the
 * appendix of the RFC.
 *
 * ASSUMPTIONS:
 * The RFC is correct.
 *
 * @param text text to be hashed
 * @param textLen length of text
 * @param key key to use for HMAC
 * @param keyLen length of key
 * @param digest holds resultant MD5 HMAC (20B)
 *
 * @return CDF_STATUS_SUCCSS if the operation succeeds
 *
 */
struct hmac_md5_result {
	struct completion completion;
	int err;
};

static void hmac_md5_complete(struct crypto_async_request *req, int err)
{
	struct hmac_md5_result *r = req->data;
	if (err == -EINPROGRESS)
		return;
	r->err = err;
	complete(&r->completion);
}

int
hmac_md5(uint8_t *key, uint8_t ksize, char *plaintext, uint8_t psize,
	 uint8_t *output, uint8_t outlen)
{
	int ret = 0;
	struct crypto_ahash *tfm;
	struct scatterlist sg;
	struct ahash_request *req;
	struct hmac_md5_result tresult = {.err = 0 };
	void *hash_buff = NULL;

	unsigned char hash_result[64];
	int i;

	memset(output, 0, outlen);

	init_completion(&tresult.completion);

	tfm = cds_crypto_alloc_ahash("hmac(md5)", CRYPTO_ALG_TYPE_AHASH,
				 CRYPTO_ALG_TYPE_AHASH_MASK);
	if (IS_ERR(tfm)) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "crypto_alloc_ahash failed");
		ret = PTR_ERR(tfm);
		goto err_tfm;
	}

	req = ahash_request_alloc(tfm, GFP_KERNEL);
	if (!req) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "failed to allocate request for hmac(md5)");
		ret = -ENOMEM;
		goto err_req;
	}

	ahash_request_set_callback(req, CRYPTO_TFM_REQ_MAY_BACKLOG,
				   hmac_md5_complete, &tresult);

	hash_buff = kzalloc(psize, GFP_KERNEL);
	if (!hash_buff) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "failed to kzalloc hash_buff");
		ret = -ENOMEM;
		goto err_hash_buf;
	}

	memset(hash_result, 0, 64);
	memcpy(hash_buff, plaintext, psize);
	sg_init_one(&sg, hash_buff, psize);

	if (ksize) {
		crypto_ahash_clear_flags(tfm, ~0);
		ret = cds_crypto_ahash_setkey(tfm, key, ksize);
		if (ret) {
			CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
				  "crypto_ahash_setkey failed");
			goto err_setkey;
		}
	}

	ahash_request_set_crypt(req, &sg, hash_result, psize);
	ret = cds_crypto_ahash_digest(req);

	CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR, "ret 0x%x", ret);

	switch (ret) {
	case 0:
		for (i = 0; i < outlen; i++)
			output[i] = hash_result[i];
		break;
	case -EINPROGRESS:
	case -EBUSY:
		ret = wait_for_completion_interruptible(&tresult.completion);
		if (!ret && !tresult.err) {
			INIT_COMPLETION(tresult.completion);
			break;
		} else {
			CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
				  "wait_for_completion_interruptible failed");
			if (!ret)
				ret = tresult.err;
			goto out;
		}
	default:
		goto out;
	}

out:
err_setkey:
	kfree(hash_buff);
err_hash_buf:
	ahash_request_free(req);
err_req:
	cds_crypto_free_ahash(tfm);
err_tfm:
	return ret;
}

CDF_STATUS cds_md5_hmac_str(uint32_t cryptHandle,       /* Handle */
			    uint8_t *pText,     /* pointer to data stream */
			    uint32_t textLen,   /* length of data stream */
			    uint8_t *pKey,      /* pointer to authentication key */
			    uint32_t keyLen,    /* length of authentication key */
			    uint8_t digest[CDS_DIGEST_MD5_SIZE])
{                               /* caller digest to be filled in */
	int ret = 0;

	ret = hmac_md5(pKey,    /* uint8_t *key, */
		       (uint8_t) keyLen,        /* uint8_t ksize, */
		       (char *)pText,   /* char *plaintext, */
		       (uint8_t) textLen,       /* uint8_t psize, */
		       digest,  /* uint8_t *output, */
		       CDS_DIGEST_MD5_SIZE      /* uint8_t outlen */
		       );

	if (ret != 0) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "hmac_md5() call failed");
		return CDF_STATUS_E_FAULT;
	}

	return CDF_STATUS_SUCCESS;
}

struct ecb_aes_result {
	struct completion completion;
	int err;
};

static void ecb_aes_complete(struct crypto_async_request *req, int err)
{
	struct ecb_aes_result *r = req->data;
	if (err == -EINPROGRESS)
		return;
	r->err = err;
	complete(&r->completion);
}

/*--------------------------------------------------------------------------

   \brief cds_encrypt_aes() - Generate AES Encrypted byte stream

   The cds_encrypt_aes() function generates the encrypted byte stream for given text.

   Buffer should be allocated before calling cds_rand_get_bytes().

   Attempting to initialize an already initialized lock results in
   a failure.

   \param lock - pointer to the opaque lock object to initialize

   \return CDF_STATUS_SUCCESS - Successfully generated random memory.

          CDF_STATUS_E_FAULT  - pbBuf is an invalid pointer.

          CDF_STATUS_E_FAILURE - default return value if it fails due to
          unknown reasons

  ***CDF_STATUS_E_RESOURCES - System resources (other than memory)
          are unavailable
   \sa

    ( *** return value not considered yet )
   --------------------------------------------------------------------------*/

CDF_STATUS cds_encrypt_aes(uint32_t cryptHandle,        /* Handle */
			   uint8_t *pPlainText,         /* pointer to data stream */
			   uint8_t *pCiphertext, uint8_t *pKey)
{                               /* pointer to authentication key */
	struct ecb_aes_result result;
	struct ablkcipher_request *req;
	struct crypto_ablkcipher *tfm;
	int ret = 0;
	char iv[IV_SIZE_AES_128];
	struct scatterlist sg_in;
	struct scatterlist sg_out;

	init_completion(&result.completion);

	tfm = cds_crypto_alloc_ablkcipher("cbc(aes)", 0, 0);
	if (IS_ERR(tfm)) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "crypto_alloc_ablkcipher failed");
		ret = PTR_ERR(tfm);
		goto err_tfm;
	}

	req = ablkcipher_request_alloc(tfm, GFP_KERNEL);
	if (!req) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "Failed to allocate request for cbc(aes)");
		ret = -ENOMEM;
		goto err_req;
	}

	ablkcipher_request_set_callback(req, CRYPTO_TFM_REQ_MAY_BACKLOG,
					ecb_aes_complete, &result);

	crypto_ablkcipher_clear_flags(tfm, ~0);

	ret = crypto_ablkcipher_setkey(tfm, pKey, AES_KEYSIZE_128);
	if (ret) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "crypto_cipher_setkey failed");
		goto err_setkey;
	}

	memset(iv, 0, IV_SIZE_AES_128);

	sg_init_one(&sg_in, pPlainText, AES_BLOCK_SIZE);

	sg_init_one(&sg_out, pCiphertext, AES_BLOCK_SIZE);

	ablkcipher_request_set_crypt(req, &sg_in, &sg_out, AES_BLOCK_SIZE, iv);

	crypto_ablkcipher_encrypt(req);

/* ------------------------------------- */
err_setkey:
	cds_ablkcipher_request_free(req);
err_req:
	cds_crypto_free_ablkcipher(tfm);
err_tfm:
	/* return ret; */
	if (ret != 0) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "%s() call failed", __func__);
		return CDF_STATUS_E_FAULT;
	}

	return CDF_STATUS_SUCCESS;
}

/*--------------------------------------------------------------------------

   \brief cds_decrypt_aes() - Decrypts an AES Encrypted byte stream

   The cds_decrypt_aes() function decrypts the encrypted byte stream.

   Buffer should be allocated before calling cds_rand_get_bytes().

   Attempting to initialize an already initialized lock results in
   a failure.

   \param lock - pointer to the opaque lock object to initialize

   \return CDF_STATUS_SUCCESS - Successfully generated random memory.

          CDF_STATUS_E_FAULT  - pbBuf is an invalid pointer.

          CDF_STATUS_E_FAILURE - default return value if it fails due to
          unknown reasons

  ***CDF_STATUS_E_RESOURCES - System resources (other than memory)
          are unavailable
   \sa

    ( *** return value not considered yet )
   --------------------------------------------------------------------------*/

CDF_STATUS cds_decrypt_aes(uint32_t cryptHandle,        /* Handle */
			   uint8_t *pText,      /* pointer to data stream */
			   uint8_t *pDecrypted, uint8_t *pKey)
{                               /* pointer to authentication key */
/*    CDF_STATUS uResult = CDF_STATUS_E_FAILURE; */
	struct ecb_aes_result result;
	struct ablkcipher_request *req;
	struct crypto_ablkcipher *tfm;
	int ret = 0;
	char iv[IV_SIZE_AES_128];
	struct scatterlist sg_in;
	struct scatterlist sg_out;

	init_completion(&result.completion);

	tfm = cds_crypto_alloc_ablkcipher("cbc(aes)", 0, 0);
	if (IS_ERR(tfm)) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "crypto_alloc_ablkcipher failed");
		ret = PTR_ERR(tfm);
		goto err_tfm;
	}

	req = ablkcipher_request_alloc(tfm, GFP_KERNEL);
	if (!req) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "Failed to allocate request for cbc(aes)");
		ret = -ENOMEM;
		goto err_req;
	}

	ablkcipher_request_set_callback(req, CRYPTO_TFM_REQ_MAY_BACKLOG,
					ecb_aes_complete, &result);

	crypto_ablkcipher_clear_flags(tfm, ~0);

	ret = crypto_ablkcipher_setkey(tfm, pKey, AES_KEYSIZE_128);
	if (ret) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "crypto_cipher_setkey failed");
		goto err_setkey;
	}

	memset(iv, 0, IV_SIZE_AES_128);

	sg_init_one(&sg_in, pText, AES_BLOCK_SIZE);

	sg_init_one(&sg_out, pDecrypted, AES_BLOCK_SIZE);

	ablkcipher_request_set_crypt(req, &sg_in, &sg_out, AES_BLOCK_SIZE, iv);

	crypto_ablkcipher_decrypt(req);

/* ------------------------------------- */
err_setkey:
	cds_ablkcipher_request_free(req);
err_req:
	cds_crypto_free_ablkcipher(tfm);
err_tfm:
	/* return ret; */
	if (ret != 0) {
		CDF_TRACE(CDF_MODULE_ID_CDF, CDF_TRACE_LEVEL_ERROR,
			  "%s() call failed", __func__);
		return CDF_STATUS_E_FAULT;
	}

	return CDF_STATUS_SUCCESS;
}

uint32_t cds_chan_to_freq(uint8_t chan)
{
	if (chan < CDS_24_GHZ_CHANNEL_14)       /* ch 0 - ch 13 */
		return CDS_24_GHZ_BASE_FREQ + chan * CDS_CHAN_SPACING_5MHZ;
	else if (chan == CDS_24_GHZ_CHANNEL_14) /* ch 14 */
		return CDS_CHAN_14_FREQ;
	else if (chan < CDS_24_GHZ_CHANNEL_27)  /* ch 15 - ch 26 */
		return CDS_CHAN_15_FREQ +
		       (chan - CDS_24_GHZ_CHANNEL_15) * CDS_CHAN_SPACING_20MHZ;
	else if (chan == CDS_5_GHZ_CHANNEL_170)
		return CDS_CHAN_170_FREQ;
	else
		return CDS_5_GHZ_BASE_FREQ + chan * CDS_CHAN_SPACING_5MHZ;
}

uint8_t cds_freq_to_chan(uint32_t freq)
{
	uint8_t chan;

	if (freq > CDS_24_GHZ_BASE_FREQ && freq < CDS_CHAN_14_FREQ)
		chan = ((freq - CDS_24_GHZ_BASE_FREQ) / CDS_CHAN_SPACING_5MHZ);
	else if (freq == CDS_CHAN_14_FREQ)
		chan = CDS_24_GHZ_CHANNEL_14;
	else if ((freq > CDS_24_GHZ_BASE_FREQ) && (freq < CDS_5_GHZ_BASE_FREQ))
		chan = (((freq - CDS_CHAN_15_FREQ) / CDS_CHAN_SPACING_20MHZ) +
			CDS_24_GHZ_CHANNEL_15);
	else
		chan = (freq - CDS_5_GHZ_BASE_FREQ) / CDS_CHAN_SPACING_5MHZ;
	return chan;
}

uint8_t cds_chan_to_band(uint32_t chan)
{
	if (chan <= CDS_24_GHZ_CHANNEL_14)
		return CDS_BAND_2GHZ;

	return CDS_BAND_5GHZ;
}
