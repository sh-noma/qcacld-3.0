/*
 * Copyright (c) 2012, 2014 The Linux Foundation. All rights reserved.
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

/*
 * This file lim_scan_result_utils.h contains the utility definitions
 * LIM uses for maintaining and accessing scan results on STA.
 * Author:        Chandra Modumudi
 * Date:          02/13/02
 * History:-
 * Date           Modified by    Modification Information
 * --------------------------------------------------------------------
 */
#ifndef __LIM_SCAN_UTILS_H
#define __LIM_SCAN_UTILS_H

#include "parser_api.h"
#include "lim_types.h"

/* Scan result hash related functions */
uint8_t lim_scan_hash_function(tSirMacAddr);
void lim_restore_pre_scan_state(tpAniSirGlobal);
void lim_copy_scan_result(tpAniSirGlobal, uint8_t *);
void lim_check_and_add_bss_description(tpAniSirGlobal, tpSirProbeRespBeacon,
				       uint8_t *, bool, uint8_t);
#if defined WLAN_FEATURE_VOWIFI
CDF_STATUS lim_collect_bss_description(tpAniSirGlobal,
				       tSirBssDescription *,
				       tpSirProbeRespBeacon, uint8_t *, uint8_t);
#else
CDF_STATUS lim_collect_bss_description(tpAniSirGlobal,
				       tSirBssDescription *,
				       tpSirProbeRespBeacon, uint8_t *);
#endif

#endif /* __LIM_SCAN_UTILS_H */
