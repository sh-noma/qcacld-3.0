/*
 * Copyright (c) 2013-2015 The Linux Foundation. All rights reserved.
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

/**
 *  DOC:    wma_scan_roam.c
 *  This file contains functions related to scan and
 *  roaming functionality.
 */

/* Header files */

#include "wma.h"
#include "wma_api.h"
#include "cds_api.h"
#include "wmi_unified_api.h"
#include "wlan_qct_sys.h"
#include "wni_api.h"
#include "ani_global.h"
#include "wmi_unified.h"
#include "wni_cfg.h"
#include "cfg_api.h"
#include "ol_txrx_ctrl_api.h"
#include "wlan_tgt_def_config.h"

#include "cdf_nbuf.h"
#include "cdf_types.h"
#include "ol_txrx_api.h"
#include "cdf_memory.h"
#include "ol_txrx_types.h"
#include "ol_txrx_peer_find.h"

#include "wma_types.h"
#include "lim_api.h"
#include "lim_session_utils.h"

#include "cds_utils.h"

#if !defined(REMOVE_PKT_LOG)
#include "pktlog_ac.h"
#endif /* REMOVE_PKT_LOG */

#include "dbglog_host.h"
/* FIXME: Inclusion of .c looks odd but this is how it is in internal codebase */
#include "csr_api.h"
#include "ol_fw.h"

#include "dfs.h"
#include "wma_internal.h"

#define WMA_MCC_MIRACAST_REST_TIME 400
#define WMA_SCAN_ID_MASK 0x0fff

#ifdef FEATURE_WLAN_EXTSCAN
/**
 * enum extscan_report_events_type - extscan report events type
 * @EXTSCAN_REPORT_EVENTS_BUFFER_FULL: report only when scan history is % full
 * @EXTSCAN_REPORT_EVENTS_EACH_SCAN: report a scan completion event after scan
 * @EXTSCAN_REPORT_EVENTS_FULL_RESULTS: forward scan results
 *		(beacons/probe responses + IEs)
 *		in real time to HAL, in addition to completion events.
 *		Note: To keep backward compatibility,
 *		fire completion events regardless of REPORT_EVENTS_EACH_SCAN.
 * @EXTSCAN_REPORT_EVENTS_NO_BATCH: controls batching,
 *		0 => batching, 1 => no batching
 */
enum extscan_report_events_type {
	EXTSCAN_REPORT_EVENTS_BUFFER_FULL   = 0x00,
	EXTSCAN_REPORT_EVENTS_EACH_SCAN     = 0x01,
	EXTSCAN_REPORT_EVENTS_FULL_RESULTS  = 0x02,
	EXTSCAN_REPORT_EVENTS_NO_BATCH      = 0x04,
};

#define WMA_EXTSCAN_CYCLE_WAKE_LOCK_DURATION (5 * 1000) /* in msec */
#endif

/**
 * wma_set_p2p_scan_info() - set p2p scan info in wma handle
 * @wma_handle: wma handle
 * @scan_id: scan id
 * @vdev_id: vdev id
 * @p2p_scan_type: p2p scan type
 *
 * Return: none
 */
static inline void wma_set_p2p_scan_info(tp_wma_handle wma_handle,
				     uint32_t scan_id,
				     uint32_t vdev_id,
				     tSirP2pScanType p2p_scan_type)
{
	wma_handle->interfaces[vdev_id].p2p_scan_info.scan_id = scan_id;
	wma_handle->interfaces[vdev_id].p2p_scan_info.p2p_scan_type =
		p2p_scan_type;
}

/**
 * wma_reset_p2p_scan_info() - reset scan info from wma handle
 * @wma_handle: wma handle
 * @vdev_id: vdev id
 *
 * Return: none
 */
static inline void wma_reset_p2p_scan_info(tp_wma_handle wma_handle,
				       uint8_t vdev_id)
{
	cdf_mem_zero((void *)&(wma_handle->interfaces[vdev_id].p2p_scan_info),
		     sizeof(struct p2p_scan_param));
}

/**
 * wma_is_mcc_24G() - check that if device is in 2.4GHz MCC
 * @handle: wma handle
 *
 * Return: true/false
 */
static bool wma_is_mcc_24G(WMA_HANDLE handle)
{
	tp_wma_handle wma_handle = (tp_wma_handle) handle;
	int32_t prev_chan = 0;
	int32_t i;

	if (NULL == wma_handle) {
		WMA_LOGE("%s: wma_handle is NULL", __func__);
		return false;
	}
	for (i = 0; i < wma_handle->max_bssid; i++) {
		if (wma_handle->interfaces[i].handle &&
				wma_handle->interfaces[i].vdev_up) {
			if ((prev_chan != 0 &&
				prev_chan != wma_handle->interfaces[i].mhz) &&
				(wma_handle->interfaces[i].mhz <=
				 CDS_CHAN_14_FREQ))
				return true;
			else
				prev_chan = wma_handle->interfaces[i].mhz;
		}
	}
	return false;
}

/**
 * wma_get_buf_start_scan_cmd() - Fill start scan command
 * @wma_handle: wma handle
 * @scan_req: scan request
 * @buf: wmi buffer to be filled in
 * @buf_len: buf length
 *
 * Fill individual elements of wmi_start_scan_req and TLV for
 * channel list, bssid, ssid etc.
 *
 * Return: CDF status
 */
CDF_STATUS wma_get_buf_start_scan_cmd(tp_wma_handle wma_handle,
				      tSirScanOffloadReq *scan_req,
				      wmi_buf_t *buf, int *buf_len)
{
	wmi_start_scan_cmd_fixed_param *cmd;
	wmi_chan_list *chan_list = NULL;
	wmi_mac_addr *bssid;
	wmi_ssid *ssid = NULL;
	uint32_t *tmp_ptr, ie_len_with_pad;
	CDF_STATUS cdf_status = CDF_STATUS_E_FAILURE;
	uint8_t *buf_ptr;
	uint32_t dwell_time;
	uint8_t SSID_num;
	int i;
	int len = sizeof(*cmd);
	tpAniSirGlobal pMac = cds_get_context(CDF_MODULE_ID_PE);

	if (!pMac) {
		WMA_LOGP("%s: pMac is NULL!", __func__);
		return CDF_STATUS_E_FAILURE;
	}

	len += WMI_TLV_HDR_SIZE;        /* Length TLV placeholder for array of uint32_t */
	/* calculate the length of buffer required */
	if (scan_req->channelList.numChannels)
		len += scan_req->channelList.numChannels * sizeof(uint32_t);

	len += WMI_TLV_HDR_SIZE;        /* Length TLV placeholder for array of wmi_ssid structures */
	if (scan_req->numSsid)
		len += scan_req->numSsid * sizeof(wmi_ssid);

	len += WMI_TLV_HDR_SIZE;        /* Length TLV placeholder for array of wmi_mac_addr structures */
	len += sizeof(wmi_mac_addr);

	len += WMI_TLV_HDR_SIZE;        /* Length TLV placeholder for array of bytes */
	if (scan_req->uIEFieldLen)
		len += roundup(scan_req->uIEFieldLen, sizeof(uint32_t));

	/* Allocate the memory */
	*buf = wmi_buf_alloc(wma_handle->wmi_handle, len);
	if (!*buf) {
		WMA_LOGP("%s: failed to allocate memory for start scan cmd",
			 __func__);
		return CDF_STATUS_E_FAILURE;
	}

	buf_ptr = (uint8_t *) wmi_buf_data(*buf);
	cmd = (wmi_start_scan_cmd_fixed_param *) buf_ptr;
	WMITLV_SET_HDR(&cmd->tlv_header,
		       WMITLV_TAG_STRUC_wmi_start_scan_cmd_fixed_param,
		       WMITLV_GET_STRUCT_TLVLEN
			       (wmi_start_scan_cmd_fixed_param));

	cmd->vdev_id = scan_req->sessionId;
	/*
	 * host cycles through the lower 12 bits for scan id generation
	 * and prefix 0xA000 to scan id
	 */
	if (scan_req->scan_id < WMA_HOST_SCAN_REQID_PREFIX) {
		WMA_LOGE("Received scan_id 0x%x is wrong",
			 cmd->scan_id);
		scan_req->scan_id = scan_req->scan_id & WMA_SCAN_ID_MASK;
		/* Appending the 0xA000 to scan Id*/
		cmd->scan_id = scan_req->scan_id | WMA_HOST_SCAN_REQID_PREFIX;
	} else {
		cmd->scan_id = scan_req->scan_id;
	}
	cmd->scan_priority = WMI_SCAN_PRIORITY_LOW;
	cmd->scan_req_id = WMA_HOST_SCAN_REQUESTOR_ID_PREFIX |
			   WMA_DEFAULT_SCAN_REQUESTER_ID;

	/* Set the scan events which the driver is intereseted to receive */
	/* TODO: handle all the other flags also */
	cmd->notify_scan_events = WMI_SCAN_EVENT_STARTED |
				  WMI_SCAN_EVENT_START_FAILED |
				  WMI_SCAN_EVENT_FOREIGN_CHANNEL |
				  WMI_SCAN_EVENT_COMPLETED |
				  WMI_SCAN_EVENT_DEQUEUED |
				  WMI_SCAN_EVENT_PREEMPTED | WMI_SCAN_EVENT_RESTARTED;

	cmd->dwell_time_active = scan_req->maxChannelTime;

	if (scan_req->scanType == eSIR_ACTIVE_SCAN) {
		/* In Active scan case, the firmware has to do passive scan on DFS channels
		 * So the passive scan duration should be updated properly so that the duration
		 * will be sufficient enough to receive the beacon from AP */

		if (wlan_cfg_get_int(pMac, WNI_CFG_PASSIVE_MAXIMUM_CHANNEL_TIME,
				     &dwell_time) != eSIR_SUCCESS) {
			WMA_LOGE("Failed to get passive max channel value"
				 "using default value");
			dwell_time = WMA_DWELL_TIME_PASSIVE_DEFAULT;
		}
		cmd->dwell_time_passive = dwell_time;
	} else
		cmd->dwell_time_passive = scan_req->maxChannelTime;

	WMA_LOGI("Scan Type %x, Active dwell time %u, Passive dwell time %u",
		 scan_req->scanType, cmd->dwell_time_active,
		 cmd->dwell_time_passive);

	/* Ensure correct number of probes are sent on active channel */
	cmd->repeat_probe_time =
		cmd->dwell_time_active / WMA_SCAN_NPROBES_DEFAULT;

	/* CSR sends only one value restTime for staying on home channel
	 * to continue data traffic. Rome fw has facility to monitor the traffic
	 * and move to next channel. Stay on the channel for at least half
	 * of the requested time and then leave if there is no traffic.
	 */
	cmd->min_rest_time = scan_req->restTime / 2;
	cmd->max_rest_time = scan_req->restTime;

	/* Check for traffic at idle_time interval after min_rest_time.
	 * Default value is 25 ms to allow full use of max_rest_time
	 * when voice packets are running at 20 ms interval.
	 */
	cmd->idle_time = WMA_SCAN_IDLE_TIME_DEFAULT;

	/* Large timeout value for full scan cycle, 30 seconds */
	cmd->max_scan_time = WMA_HW_DEF_SCAN_MAX_DURATION;

	/* add DS param IE in probe req frame */
	cmd->scan_ctrl_flags |= WMI_SCAN_ADD_DS_IE_IN_PROBE_REQ;

	/* do not add OFDM rates in 11B mode */
	if (scan_req->dot11mode != WNI_CFG_DOT11_MODE_11B)
		cmd->scan_ctrl_flags |= WMI_SCAN_ADD_OFDM_RATES;
	else
		WMA_LOGD("OFDM_RATES not included in 11B mode");

	/* Do not combine multiple channels in a single burst. Come back
	 * to home channel for data traffic after every foreign channel.
	 * By default, prefer throughput performance over scan cycle time.
	 */
	cmd->burst_duration = 0;

	if (!scan_req->p2pScanType) {
		WMA_LOGD("Normal Scan request");
		cmd->scan_ctrl_flags |= WMI_SCAN_ADD_CCK_RATES;
		if (!scan_req->numSsid)
			cmd->scan_ctrl_flags |= WMI_SCAN_ADD_BCAST_PROBE_REQ;
		if (scan_req->scanType == eSIR_PASSIVE_SCAN)
			cmd->scan_ctrl_flags |= WMI_SCAN_FLAG_PASSIVE;
		cmd->scan_ctrl_flags |= WMI_SCAN_FILTER_PROBE_REQ;

		/*
		 * Decide burst_duration and dwell_time_active based on
		 * what type of devices are active.
		 */
		do {
			if (wma_is_sap_active(wma_handle) &&
					wma_is_p2p_go_active(wma_handle) &&
					wma_is_sta_active(wma_handle)) {
				if (scan_req->maxChannelTime <=
					WMA_3PORT_CONC_SCAN_MAX_BURST_DURATION)
					cmd->burst_duration =
						scan_req->maxChannelTime;
				else
					cmd->burst_duration =
					WMA_3PORT_CONC_SCAN_MAX_BURST_DURATION;
				break;
			}
			if (wma_is_sap_active(wma_handle)) {
				/* Background scan while SoftAP is sending beacons.
				 * Max duration of CTS2self is 32 ms, which limits
				 * the dwell time.
				 */
				cmd->dwell_time_active =
				   CDF_MIN(scan_req->maxChannelTime,
					   (WMA_CTS_DURATION_MS_MAX -
					    WMA_ROAM_SCAN_CHANNEL_SWITCH_TIME));
				cmd->dwell_time_passive =
					cmd->dwell_time_active;
				cmd->burst_duration = 0;
				break;
			}
			if (wma_handle->miracast_value &&
					wma_is_mcc_24G(wma_handle)) {
				cmd->max_rest_time =
					pMac->f_sta_miracast_mcc_rest_time_val;
			}
			if (wma_is_p2p_go_active(wma_handle)) {
				/* Background scan while GO is sending beacons.
				 * Every off-channel transition has overhead of 2 beacon
				 * intervals for NOA. Maximize number of channels in
				 * every transition by using burst scan.
				 */
				if (wma_handle->miracast_value) {
					/* When miracast is running, burst duration
					 * needs to be minimum to avoid any stutter
					 * or glitch in miracast during station scan
					 */
					if (scan_req->maxChannelTime <=
					    WMA_GO_MIN_ACTIVE_SCAN_BURST_DURATION)
						cmd->burst_duration =
							scan_req->maxChannelTime;
					else
						cmd->burst_duration =
							WMA_GO_MIN_ACTIVE_SCAN_BURST_DURATION;
				} else {
					/* If miracast is not running, accomodate max
					 * stations to make the scans faster
					 */
					cmd->burst_duration =
						WMA_BURST_SCAN_MAX_NUM_OFFCHANNELS *
						scan_req->maxChannelTime;
					if (cmd->burst_duration >
					    WMA_GO_MAX_ACTIVE_SCAN_BURST_DURATION) {
						uint8_t channels =
							WMA_P2P_SCAN_MAX_BURST_DURATION
							/ scan_req->maxChannelTime;
						if (channels)
							cmd->burst_duration =
								channels *
								scan_req->maxChannelTime;
						else
							cmd->burst_duration =
								WMA_GO_MAX_ACTIVE_SCAN_BURST_DURATION;
					}
				}
				break;
			}
			if (wma_is_sta_active(wma_handle) ||
			    wma_is_p2p_cli_active(wma_handle)) {
				/* Typical background scan. Disable burst scan for now. */
				cmd->burst_duration = 0;
				break;
			}
		} while (0);

	} else {
		WMA_LOGD("P2P Scan");
		switch (scan_req->p2pScanType) {
		case P2P_SCAN_TYPE_LISTEN:
			WMA_LOGD("P2P_SCAN_TYPE_LISTEN");
			cmd->scan_ctrl_flags |= WMI_SCAN_FLAG_PASSIVE;
			cmd->notify_scan_events |=
				WMI_SCAN_EVENT_FOREIGN_CHANNEL;
			cmd->repeat_probe_time = 0;
			break;
		case P2P_SCAN_TYPE_SEARCH:
			WMA_LOGD("P2P_SCAN_TYPE_SEARCH");
			cmd->scan_ctrl_flags |= WMI_SCAN_FILTER_PROBE_REQ;
			/* Default P2P burst duration of 120 ms will cover
			 * 3 channels with default max dwell time 40 ms.
			 * Cap limit will be set by
			 * WMA_P2P_SCAN_MAX_BURST_DURATION. Burst duration
			 * should be such that no channel is scanned less
			 * than the dwell time in normal scenarios.
			 */
			if (scan_req->channelList.numChannels ==
			    P2P_SOCIAL_CHANNELS
			    && (!(wma_handle->miracast_value)))
				cmd->repeat_probe_time =
					scan_req->maxChannelTime / 5;
			else
				cmd->repeat_probe_time =
					scan_req->maxChannelTime / 3;

			cmd->burst_duration =
				WMA_BURST_SCAN_MAX_NUM_OFFCHANNELS *
				scan_req->maxChannelTime;
			if (cmd->burst_duration >
			    WMA_P2P_SCAN_MAX_BURST_DURATION) {
				uint8_t channels =
					WMA_P2P_SCAN_MAX_BURST_DURATION /
					scan_req->maxChannelTime;
				if (channels)
					cmd->burst_duration =
						channels * scan_req->maxChannelTime;
				else
					cmd->burst_duration =
						WMA_P2P_SCAN_MAX_BURST_DURATION;
			}
			break;
		default:
			WMA_LOGE("Invalid scan type");
			goto error;
		}
	}

	cmd->n_probes = (cmd->repeat_probe_time > 0) ?
			cmd->dwell_time_active / cmd->repeat_probe_time : 0;

	buf_ptr += sizeof(*cmd);
	tmp_ptr = (uint32_t *) (buf_ptr + WMI_TLV_HDR_SIZE);

	if (scan_req->channelList.numChannels) {
		chan_list = (wmi_chan_list *) tmp_ptr;
		cmd->num_chan = scan_req->channelList.numChannels;
		for (i = 0; i < scan_req->channelList.numChannels; ++i) {
			tmp_ptr[i] =
				cds_chan_to_freq(scan_req->channelList.
						 channelNumber[i]);
		}
	}
	WMITLV_SET_HDR(buf_ptr,
		       WMITLV_TAG_ARRAY_UINT32,
		       (cmd->num_chan * sizeof(uint32_t)));
	buf_ptr += WMI_TLV_HDR_SIZE + (cmd->num_chan * sizeof(uint32_t));
	if (scan_req->numSsid > SIR_SCAN_MAX_NUM_SSID) {
		WMA_LOGE("Invalid value for numSsid");
		goto error;
	}
	cmd->num_ssids = scan_req->numSsid;
	WMITLV_SET_HDR(buf_ptr, WMITLV_TAG_ARRAY_FIXED_STRUC,
		       (cmd->num_ssids * sizeof(wmi_ssid)));
	if (scan_req->numSsid) {
		ssid = (wmi_ssid *) (buf_ptr + WMI_TLV_HDR_SIZE);
		for (i = 0; i < scan_req->numSsid; ++i) {
			ssid->ssid_len = scan_req->ssId[i].length;
			cdf_mem_copy(ssid->ssid, scan_req->ssId[i].ssId,
				     scan_req->ssId[i].length);
			ssid++;
		}
	}
	buf_ptr += WMI_TLV_HDR_SIZE + (cmd->num_ssids * sizeof(wmi_ssid));

	cmd->num_bssid = 1;

	if (!scan_req->p2pScanType) {
		if (wma_is_sap_active(wma_handle)) {
			SSID_num = cmd->num_ssids * cmd->num_bssid;
			cmd->repeat_probe_time = probe_time_dwell_time_map[
				CDF_MIN(SSID_num,
					WMA_DWELL_TIME_PROBE_TIME_MAP_SIZE
					- 1)].probe_time;
		}
	}

	WMITLV_SET_HDR(buf_ptr, WMITLV_TAG_ARRAY_FIXED_STRUC,
		       (cmd->num_bssid * sizeof(wmi_mac_addr)));
	bssid = (wmi_mac_addr *) (buf_ptr + WMI_TLV_HDR_SIZE);
	WMI_CHAR_ARRAY_TO_MAC_ADDR(scan_req->bssId.bytes, bssid);
	buf_ptr += WMI_TLV_HDR_SIZE + (cmd->num_bssid * sizeof(wmi_mac_addr));

	cmd->ie_len = scan_req->uIEFieldLen;
	ie_len_with_pad = roundup(scan_req->uIEFieldLen, sizeof(uint32_t));
	WMITLV_SET_HDR(buf_ptr, WMITLV_TAG_ARRAY_BYTE, ie_len_with_pad);
	if (scan_req->uIEFieldLen) {
		cdf_mem_copy(buf_ptr + WMI_TLV_HDR_SIZE,
			     (uint8_t *) scan_req +
			     (scan_req->uIEFieldOffset), scan_req->uIEFieldLen);
	}
	buf_ptr += WMI_TLV_HDR_SIZE + ie_len_with_pad;

	*buf_len = len;
	return CDF_STATUS_SUCCESS;
error:
	cdf_mem_free(*buf);
	*buf = NULL;
	return cdf_status;
}

/**
 * wma_get_buf_stop_scan_cmd() - Fill stop scan command
 * @wma_handle: wma handle
 * @buf: wmi buffer to be filled in
 * @buf_len: buf length
 * @abort_scan_req: abort scan request
 *
 * Fill wmi_stop_scan_cmd buffer.
 *
 * Return: CDF status
 */
CDF_STATUS wma_get_buf_stop_scan_cmd(tp_wma_handle wma_handle,
				     wmi_buf_t *buf,
				     int *buf_len,
				     tAbortScanParams *abort_scan_req)
{
	wmi_stop_scan_cmd_fixed_param *cmd;
	CDF_STATUS cdf_status;
	int len = sizeof(*cmd);

	/* Allocate the memory */
	*buf = wmi_buf_alloc(wma_handle->wmi_handle, len);
	if (!*buf) {
		WMA_LOGP("%s: failed to allocate memory for stop scan cmd",
			 __func__);
		cdf_status = CDF_STATUS_E_FAILURE;
		goto error;
	}

	cmd = (wmi_stop_scan_cmd_fixed_param *) wmi_buf_data(*buf);
	WMITLV_SET_HDR(&cmd->tlv_header,
		       WMITLV_TAG_STRUC_wmi_stop_scan_cmd_fixed_param,
		       WMITLV_GET_STRUCT_TLVLEN(wmi_stop_scan_cmd_fixed_param));
	cmd->vdev_id = abort_scan_req->SessionId;
	cmd->requestor = WMA_HOST_SCAN_REQUESTOR_ID_PREFIX |
			WMA_DEFAULT_SCAN_REQUESTER_ID;
	cmd->scan_id = abort_scan_req->scan_id;
	/* stop the scan with the corresponding scan_id */
	cmd->req_type = WMI_SCAN_STOP_ONE;

	*buf_len = len;
	cdf_status = CDF_STATUS_SUCCESS;
error:
	return cdf_status;

}


/**
 * wma_start_scan() - start scan command
 * @wma_handle: wma handle
 * @scan_req: scan request params
 * @msg_type: message time
 *
 * Send start scan command to fw.
 *
 * Return: CDF status
 */
CDF_STATUS wma_start_scan(tp_wma_handle wma_handle,
			  tSirScanOffloadReq *scan_req, uint16_t msg_type)
{
	uint32_t vdev_id, scan_id;
	CDF_STATUS cdf_status = CDF_STATUS_SUCCESS;
	wmi_buf_t buf = NULL;
	wmi_start_scan_cmd_fixed_param *cmd;
	int status = 0;
	int len;
	tSirScanOffloadEvent *scan_event;

	if (scan_req->sessionId > wma_handle->max_bssid) {
		WMA_LOGE("%s: Invalid vdev_id %d, msg_type : 0x%x", __func__,
			 scan_req->sessionId, msg_type);
		goto error;
	}

	/* Sanity check to find whether vdev id active or not */
	if (msg_type != WMA_START_SCAN_OFFLOAD_REQ &&
		!wma_handle->interfaces[scan_req->sessionId].handle) {
		WMA_LOGA("vdev id [%d] is not active", scan_req->sessionId);
		goto error;
	}

	/* Fill individual elements of wmi_start_scan_req and
	 * TLV for channel list, bssid, ssid etc ... */
	cdf_status = wma_get_buf_start_scan_cmd(wma_handle, scan_req,
						&buf, &len);
	if (cdf_status != CDF_STATUS_SUCCESS) {
		WMA_LOGE("Failed to get buffer for start scan cmd");
		goto error;
	}

	if (NULL == buf) {
		WMA_LOGE("Failed to get buffer for saving current scan info");
		goto error;
	}

	/* Save current scan info */
	cmd = (wmi_start_scan_cmd_fixed_param *) wmi_buf_data(buf);
	if (msg_type == WMA_CHNL_SWITCH_REQ) {
		/* Adjust parameters for channel switch scan */
		cmd->min_rest_time = WMA_ROAM_PREAUTH_REST_TIME;
		cmd->max_rest_time = WMA_ROAM_PREAUTH_REST_TIME;
		cmd->max_scan_time = WMA_ROAM_PREAUTH_MAX_SCAN_TIME;
		cmd->scan_priority = WMI_SCAN_PRIORITY_HIGH;
		cmd->scan_id = scan_req->scan_id;
		wma_handle->roam_preauth_scan_id = cmd->scan_id;
	}
	if (scan_req->p2pScanType == P2P_SCAN_TYPE_LISTEN)
		wma_set_p2p_scan_info(wma_handle, cmd->scan_id,
			 cmd->vdev_id, P2P_SCAN_TYPE_LISTEN);
	WMA_LOGE("scan_id 0x%x, vdev_id %d, p2pScanType %d, msg_type 0x%x",
		 cmd->scan_id, cmd->vdev_id, scan_req->p2pScanType, msg_type);
	/*
	 * Cache vdev_id and scan_id because cmd is freed after calling
	 * wmi_unified_cmd_send cmd. WMI internally frees cmd buffer after
	 * getting TX complete from CE
	 */
	vdev_id = cmd->vdev_id;
	scan_id = cmd->scan_id;
	WMA_LOGI("ActiveDwell %d, PassiveDwell %d, ScanFlags 0x%x NumChan %d",
		 cmd->dwell_time_active, cmd->dwell_time_passive,
		 cmd->scan_ctrl_flags, cmd->num_chan);

	status = wmi_unified_cmd_send(wma_handle->wmi_handle, buf,
				      len, WMI_START_SCAN_CMDID);
	/* Call the wmi api to request the scan */
	if (status != EOK) {
		WMA_LOGE("wmi_unified_cmd_send returned Error %d", status);
		cdf_status = CDF_STATUS_E_FAILURE;
		goto error;
	}

	WMA_LOGI("WMA --> WMI_START_SCAN_CMDID");

	return CDF_STATUS_SUCCESS;
error:
	/* Send completion event for only for start scan request */
	if (msg_type == WMA_START_SCAN_OFFLOAD_REQ) {
		scan_event =
			(tSirScanOffloadEvent *)
			cdf_mem_malloc(sizeof(tSirScanOffloadEvent));
		if (!scan_event) {
			WMA_LOGP("%s: Failed to allocate memory for scan rsp",
				 __func__);
			return CDF_STATUS_E_NOMEM;
		}
		memset(scan_event, 0x00, sizeof(*scan_event));
		scan_event->event = WMI_SCAN_EVENT_COMPLETED;
		scan_event->reasonCode = eSIR_SME_SCAN_FAILED;
		scan_event->sessionId = scan_req->sessionId;
		scan_event->p2pScanType = scan_req->p2pScanType;
		scan_event->scanId = scan_req->scan_id;
		wma_send_msg(wma_handle, WMA_RX_SCAN_EVENT, (void *)scan_event,
			     0);
	}
	return cdf_status;
}

/**
 * wma_stop_scan() - stop scan command
 * @wma_handle: wma handle
 * @abort_scan_req: abort scan params
 *
 * Send stop scan command to fw.
 *
 * Return: CDF status
 */
CDF_STATUS wma_stop_scan(tp_wma_handle wma_handle,
			 tAbortScanParams *abort_scan_req)
{
	CDF_STATUS cdf_status;
	wmi_buf_t buf;
	int status = 0;
	int len;

	cdf_status = wma_get_buf_stop_scan_cmd(wma_handle, &buf, &len,
					       abort_scan_req);
	if (cdf_status != CDF_STATUS_SUCCESS) {
		WMA_LOGE("Failed to get buffer for stop scan cmd");
		goto error1;
	}

	if (NULL == buf) {
		WMA_LOGE("Failed to get buffer for stop scan cmd");
		cdf_status = CDF_STATUS_E_FAULT;
		goto error1;
	}

	status = wmi_unified_cmd_send(wma_handle->wmi_handle, buf,
				      len, WMI_STOP_SCAN_CMDID);
	/* Call the wmi api to request the scan */
	if (status != EOK) {
		WMA_LOGE("wmi_unified_cmd_send WMI_STOP_SCAN_CMDID returned Error %d",
			status);
		cdf_status = CDF_STATUS_E_FAILURE;
		goto error;
	}
	WMA_LOGE("scan_id %x, vdev_id %x",
		 abort_scan_req->scan_id, abort_scan_req->SessionId);
	WMA_LOGI("WMA --> WMI_STOP_SCAN_CMDID");

	return CDF_STATUS_SUCCESS;
error:
	if (buf)
		cdf_nbuf_free(buf);
error1:
	return cdf_status;
}

/**
 * wma_update_channel_list() - update channel list
 * @handle: wma handle
 * @chan_list: channel list
 *
 * Function is used to update the support channel list in fw.
 *
 * Return: CDF status
 */
CDF_STATUS wma_update_channel_list(WMA_HANDLE handle,
				   tSirUpdateChanList *chan_list)
{
	tp_wma_handle wma_handle = (tp_wma_handle) handle;
	wmi_buf_t buf;
	CDF_STATUS cdf_status = CDF_STATUS_SUCCESS;
	wmi_scan_chan_list_cmd_fixed_param *cmd;
	int status, i;
	uint8_t *buf_ptr;
	wmi_channel *chan_info;
	uint16_t len = sizeof(*cmd) + WMI_TLV_HDR_SIZE;

	len += sizeof(wmi_channel) * chan_list->numChan;
	buf = wmi_buf_alloc(wma_handle->wmi_handle, len);
	if (!buf) {
		WMA_LOGE("Failed to allocate memory");
		cdf_status = CDF_STATUS_E_NOMEM;
		goto end;
	}

	buf_ptr = (uint8_t *) wmi_buf_data(buf);
	cmd = (wmi_scan_chan_list_cmd_fixed_param *) buf_ptr;
	WMITLV_SET_HDR(&cmd->tlv_header,
		       WMITLV_TAG_STRUC_wmi_scan_chan_list_cmd_fixed_param,
		       WMITLV_GET_STRUCT_TLVLEN
			       (wmi_scan_chan_list_cmd_fixed_param));

	WMA_LOGD("no of channels = %d, len = %d", chan_list->numChan, len);

	cmd->num_scan_chans = chan_list->numChan;
	WMITLV_SET_HDR((buf_ptr + sizeof(wmi_scan_chan_list_cmd_fixed_param)),
		       WMITLV_TAG_ARRAY_STRUC,
		       sizeof(wmi_channel) * chan_list->numChan);
	chan_info = (wmi_channel *) (buf_ptr + sizeof(*cmd) + WMI_TLV_HDR_SIZE);

	for (i = 0; i < chan_list->numChan; ++i) {
		WMITLV_SET_HDR(&chan_info->tlv_header,
			       WMITLV_TAG_STRUC_wmi_channel,
			       WMITLV_GET_STRUCT_TLVLEN(wmi_channel));
		chan_info->mhz =
			cds_chan_to_freq(chan_list->chanParam[i].chanId);
		chan_info->band_center_freq1 = chan_info->mhz;
		chan_info->band_center_freq2 = 0;

		WMA_LOGD("chan[%d] = %u", i, chan_info->mhz);
		if (chan_list->chanParam[i].dfsSet) {
			WMI_SET_CHANNEL_FLAG(chan_info, WMI_CHAN_FLAG_PASSIVE);
			WMA_LOGI("chan[%d] DFS[%d]\n",
				 chan_list->chanParam[i].chanId,
				 chan_list->chanParam[i].dfsSet);
		}

		if (chan_info->mhz < WMA_2_4_GHZ_MAX_FREQ)
			WMI_SET_CHANNEL_MODE(chan_info, MODE_11G);
		else
			WMI_SET_CHANNEL_MODE(chan_info, MODE_11A);

		if (chan_list->chanParam[i].half_rate)
			WMI_SET_CHANNEL_FLAG(chan_info,
				WMI_CHAN_FLAG_HALF_RATE);
		else if (chan_list->chanParam[i].quarter_rate)
			WMI_SET_CHANNEL_FLAG(chan_info,
				WMI_CHAN_FLAG_QUARTER_RATE);

		WMI_SET_CHANNEL_MAX_TX_POWER(chan_info,
					     chan_list->chanParam[i].pwr);

		WMI_SET_CHANNEL_REG_POWER(chan_info,
					  chan_list->chanParam[i].pwr);
		WMA_LOGD("Channel TX power[%d] = %u: %d", i, chan_info->mhz,
			 chan_list->chanParam[i].pwr);
		/*TODO: Set WMI_SET_CHANNEL_MIN_POWER */
		/*TODO: Set WMI_SET_CHANNEL_ANTENNA_MAX */
		/*TODO: WMI_SET_CHANNEL_REG_CLASSID */
		chan_info++;
	}

	status = wmi_unified_cmd_send(wma_handle->wmi_handle, buf, len,
				      WMI_SCAN_CHAN_LIST_CMDID);

	if (status != EOK) {
		cdf_status = CDF_STATUS_E_FAILURE;
		WMA_LOGE("Failed to send WMI_SCAN_CHAN_LIST_CMDID");
		wmi_buf_free(buf);
	}
end:
	return cdf_status;
}


/**
 * wma_roam_scan_offload_mode() - send roam scan mode request to fw
 * @wma_handle: wma handle
 * @scan_cmd_fp: start scan command ptr
 * @roam_req: roam request param
 * @mode: mode
 * @vdev_id: vdev id
 *
 * send WMI_ROAM_SCAN_MODE TLV to firmware. It has a piggyback
 * of WMI_ROAM_SCAN_MODE.
 *
 * Return: CDF status
 */
CDF_STATUS wma_roam_scan_offload_mode(tp_wma_handle wma_handle,
				      wmi_start_scan_cmd_fixed_param *
				      scan_cmd_fp,
				      tSirRoamOffloadScanReq *roam_req,
				      uint32_t mode, uint32_t vdev_id)
{
	CDF_STATUS cdf_status = CDF_STATUS_SUCCESS;
	wmi_buf_t buf = NULL;
	int status = 0;
	int len;
	uint8_t *buf_ptr;
	wmi_roam_scan_mode_fixed_param *roam_scan_mode_fp;
#ifdef WLAN_FEATURE_ROAM_OFFLOAD
	int auth_mode = WMI_AUTH_NONE;
	wmi_roam_offload_tlv_param *roam_offload_params;
	wmi_roam_11i_offload_tlv_param *roam_offload_11i;
	wmi_roam_11r_offload_tlv_param *roam_offload_11r;
	wmi_roam_ese_offload_tlv_param *roam_offload_ese;
	if (roam_req)
		auth_mode = e_csr_auth_type_to_rsn_authmode
				    (roam_req->ConnectedNetwork.authentication,
				    roam_req->ConnectedNetwork.encryption);
	WMA_LOGD("%s : auth mode = %d", __func__, auth_mode);
#endif /* WLAN_FEATURE_ROAM_OFFLOAD */
	/* Need to create a buf with roam_scan command at
	 * front and piggyback with scan command */
	len = sizeof(wmi_roam_scan_mode_fixed_param) +
#ifdef WLAN_FEATURE_ROAM_OFFLOAD
	      (2 * WMI_TLV_HDR_SIZE) +
#endif /* WLAN_FEATURE_ROAM_OFFLOAD */
	      sizeof(wmi_start_scan_cmd_fixed_param);
#ifdef WLAN_FEATURE_ROAM_OFFLOAD
	if (roam_req && roam_req->RoamOffloadEnabled) {
		len += sizeof(wmi_roam_offload_tlv_param);
		len += WMI_TLV_HDR_SIZE;
		if ((auth_mode != WMI_AUTH_NONE) &&
		    ((auth_mode != WMI_AUTH_OPEN) ||
		     (auth_mode == WMI_AUTH_OPEN &&
		      roam_req->MDID.mdiePresent) || roam_req->IsESEAssoc)) {
			len += WMI_TLV_HDR_SIZE;
			if (roam_req->IsESEAssoc)
				len += sizeof(wmi_roam_ese_offload_tlv_param);
			else if (auth_mode == WMI_AUTH_FT_RSNA ||
				 auth_mode == WMI_AUTH_FT_RSNA_PSK ||
				 (auth_mode == WMI_AUTH_OPEN &&
				  roam_req->MDID.mdiePresent))
				len += sizeof(wmi_roam_11r_offload_tlv_param);
			else
				len += sizeof(wmi_roam_11i_offload_tlv_param);
		} else {
			len += WMI_TLV_HDR_SIZE;
		}
	} else {
		if (roam_req)
			WMA_LOGD("%s : roam offload = %d",
				 __func__, roam_req->RoamOffloadEnabled);
		else
			WMA_LOGD("%s : roam_req is NULL", __func__);
		len += (2 * WMI_TLV_HDR_SIZE);
	}
	if (roam_req && roam_req->RoamOffloadEnabled) {
		mode = mode | WMI_ROAM_SCAN_MODE_ROAMOFFLOAD;
	}
#endif /* WLAN_FEATURE_ROAM_OFFLOAD */
	buf = wmi_buf_alloc(wma_handle->wmi_handle, len);
	if (!buf) {
		WMA_LOGE("%s : wmi_buf_alloc failed", __func__);
		return CDF_STATUS_E_NOMEM;
	}

	buf_ptr = (uint8_t *) wmi_buf_data(buf);
	roam_scan_mode_fp = (wmi_roam_scan_mode_fixed_param *) buf_ptr;
	WMITLV_SET_HDR(&roam_scan_mode_fp->tlv_header,
		       WMITLV_TAG_STRUC_wmi_roam_scan_mode_fixed_param,
		       WMITLV_GET_STRUCT_TLVLEN
			       (wmi_roam_scan_mode_fixed_param));

	roam_scan_mode_fp->roam_scan_mode = mode;
	roam_scan_mode_fp->vdev_id = vdev_id;
	/* Fill in scan parameters suitable for roaming scan */
	buf_ptr += sizeof(wmi_roam_scan_mode_fixed_param);
	cdf_mem_copy(buf_ptr, scan_cmd_fp,
		     sizeof(wmi_start_scan_cmd_fixed_param));
	/* Ensure there is no additional IEs */
	scan_cmd_fp->ie_len = 0;
	WMITLV_SET_HDR(buf_ptr,
		       WMITLV_TAG_STRUC_wmi_start_scan_cmd_fixed_param,
		       WMITLV_GET_STRUCT_TLVLEN
			       (wmi_start_scan_cmd_fixed_param));
#ifdef WLAN_FEATURE_ROAM_OFFLOAD
	buf_ptr += sizeof(wmi_start_scan_cmd_fixed_param);
	if (roam_req && roam_req->RoamOffloadEnabled) {
		WMITLV_SET_HDR(buf_ptr, WMITLV_TAG_ARRAY_STRUC,
			       sizeof(wmi_roam_offload_tlv_param));
		buf_ptr += WMI_TLV_HDR_SIZE;
		roam_offload_params = (wmi_roam_offload_tlv_param *) buf_ptr;
		WMITLV_SET_HDR(buf_ptr,
			       WMITLV_TAG_STRUC_wmi_roam_offload_tlv_param,
			       WMITLV_GET_STRUCT_TLVLEN
				       (wmi_roam_offload_tlv_param));
		roam_offload_params->prefer_5g = roam_req->Prefer5GHz;
		roam_offload_params->rssi_cat_gap = roam_req->RoamRssiCatGap;
		roam_offload_params->select_5g_margin =
			roam_req->Select5GHzMargin;
		roam_offload_params->reassoc_failure_timeout =
			roam_req->ReassocFailureTimeout;
		/* Fill the capabilities */
		wma_roam_scan_fill_self_caps(wma_handle, roam_offload_params,
					     roam_req);
		buf_ptr += sizeof(wmi_roam_offload_tlv_param);
		/* The TLV's are in the order of 11i, 11R, ESE. Hence,
		 * they are filled in the same order.Depending on the
		 * authentication type, the other mode TLV's are nullified
		 * and only headers are filled.*/
		if ((auth_mode != WMI_AUTH_NONE) &&
		    ((auth_mode != WMI_AUTH_OPEN) ||
		     (auth_mode == WMI_AUTH_OPEN
		      && roam_req->MDID.mdiePresent) || roam_req->IsESEAssoc)) {
			if (roam_req->IsESEAssoc) {
				WMITLV_SET_HDR(buf_ptr, WMITLV_TAG_ARRAY_STRUC,
					       WMITLV_GET_STRUCT_TLVLEN(0));
				buf_ptr += WMI_TLV_HDR_SIZE;
				WMITLV_SET_HDR(buf_ptr, WMITLV_TAG_ARRAY_STRUC,
					       WMITLV_GET_STRUCT_TLVLEN(0));
				buf_ptr += WMI_TLV_HDR_SIZE;
				WMITLV_SET_HDR(buf_ptr, WMITLV_TAG_ARRAY_STRUC,
					sizeof(wmi_roam_ese_offload_tlv_param));
				buf_ptr += WMI_TLV_HDR_SIZE;
				roam_offload_ese =
					(wmi_roam_ese_offload_tlv_param *) buf_ptr;
				cdf_mem_copy(roam_offload_ese->krk,
					     roam_req->KRK,
					     sizeof(roam_req->KRK));
				cdf_mem_copy(roam_offload_ese->btk,
					     roam_req->BTK,
					     sizeof(roam_req->BTK));
				WMITLV_SET_HDR(&roam_offload_ese->tlv_header,
					WMITLV_TAG_STRUC_wmi_roam_ese_offload_tlv_param,
					WMITLV_GET_STRUCT_TLVLEN
					(wmi_roam_ese_offload_tlv_param));
				buf_ptr +=
					sizeof(wmi_roam_ese_offload_tlv_param);
			} else if (auth_mode == WMI_AUTH_FT_RSNA
				   || auth_mode == WMI_AUTH_FT_RSNA_PSK
				   || (auth_mode == WMI_AUTH_OPEN
				       && roam_req->MDID.mdiePresent)) {
				WMITLV_SET_HDR(buf_ptr, WMITLV_TAG_ARRAY_STRUC,
					       0);
				buf_ptr += WMI_TLV_HDR_SIZE;
				WMITLV_SET_HDR(buf_ptr, WMITLV_TAG_ARRAY_STRUC,
					sizeof(wmi_roam_11r_offload_tlv_param));
				buf_ptr += WMI_TLV_HDR_SIZE;
				roam_offload_11r =
					(wmi_roam_11r_offload_tlv_param *) buf_ptr;
				roam_offload_11r->r0kh_id_len =
					roam_req->R0KH_ID_Length;
				cdf_mem_copy(roam_offload_11r->r0kh_id,
					     roam_req->R0KH_ID,
					     roam_offload_11r->r0kh_id_len);
				cdf_mem_copy(roam_offload_11r->psk_msk,
					     roam_req->PSK_PMK,
					     sizeof(roam_req->PSK_PMK));
				roam_offload_11r->psk_msk_len =
					roam_req->pmk_len;
				roam_offload_11r->mdie_present =
					roam_req->MDID.mdiePresent;
				roam_offload_11r->mdid =
					roam_req->MDID.mobilityDomain;
				if (auth_mode == WMI_AUTH_OPEN) {
					/* If FT-Open ensure pmk length
					   and r0khid len are zero */
					roam_offload_11r->r0kh_id_len = 0;
					roam_offload_11r->psk_msk_len = 0;
				}
				WMITLV_SET_HDR(&roam_offload_11r->tlv_header,
					WMITLV_TAG_STRUC_wmi_roam_11r_offload_tlv_param,
					WMITLV_GET_STRUCT_TLVLEN
					(wmi_roam_11r_offload_tlv_param));
				buf_ptr +=
					sizeof(wmi_roam_11r_offload_tlv_param);
				WMITLV_SET_HDR(buf_ptr, WMITLV_TAG_ARRAY_STRUC,
					       WMITLV_GET_STRUCT_TLVLEN(0));
				buf_ptr += WMI_TLV_HDR_SIZE;
			} else {
				WMITLV_SET_HDR(buf_ptr, WMITLV_TAG_ARRAY_STRUC,
					sizeof(wmi_roam_11i_offload_tlv_param));
				buf_ptr += WMI_TLV_HDR_SIZE;
				roam_offload_11i =
					(wmi_roam_11i_offload_tlv_param *) buf_ptr;
				if (roam_req->RoamKeyMgmtOffloadEnabled) {
					WMI_SET_ROAM_OFFLOAD_OKC_ENABLED
						(roam_offload_11i->flags);
					WMA_LOGE("LFR3:OKC Enabled");
				} else {
					WMI_SET_ROAM_OFFLOAD_OKC_DISABLED
						(roam_offload_11i->flags);
					WMA_LOGE("LFR3:OKC Disabled");
				}

				cdf_mem_copy(roam_offload_11i->pmk,
					     roam_req->PSK_PMK,
					     sizeof(roam_req->PSK_PMK));
				roam_offload_11i->pmk_len = roam_req->pmk_len;
				WMITLV_SET_HDR(&roam_offload_11i->tlv_header,
					WMITLV_TAG_STRUC_wmi_roam_11i_offload_tlv_param,
					WMITLV_GET_STRUCT_TLVLEN
					(wmi_roam_11i_offload_tlv_param));
				buf_ptr +=
					sizeof(wmi_roam_11i_offload_tlv_param);
				WMITLV_SET_HDR(buf_ptr, WMITLV_TAG_ARRAY_STRUC,
					       0);
				buf_ptr += WMI_TLV_HDR_SIZE;
				WMITLV_SET_HDR(buf_ptr, WMITLV_TAG_ARRAY_STRUC,
					       0);
				buf_ptr += WMI_TLV_HDR_SIZE;
			}
		} else {
			WMITLV_SET_HDR(buf_ptr, WMITLV_TAG_ARRAY_STRUC,
				       WMITLV_GET_STRUCT_TLVLEN(0));
			buf_ptr += WMI_TLV_HDR_SIZE;
			WMITLV_SET_HDR(buf_ptr, WMITLV_TAG_ARRAY_STRUC,
				       WMITLV_GET_STRUCT_TLVLEN(0));
			buf_ptr += WMI_TLV_HDR_SIZE;
			WMITLV_SET_HDR(buf_ptr, WMITLV_TAG_ARRAY_STRUC,
				       WMITLV_GET_STRUCT_TLVLEN(0));
		}
	} else {
		WMITLV_SET_HDR(buf_ptr, WMITLV_TAG_ARRAY_STRUC,
			       WMITLV_GET_STRUCT_TLVLEN(0));
		buf_ptr += WMI_TLV_HDR_SIZE;
		WMITLV_SET_HDR(buf_ptr, WMITLV_TAG_ARRAY_STRUC,
			       WMITLV_GET_STRUCT_TLVLEN(0));
		buf_ptr += WMI_TLV_HDR_SIZE;
		WMITLV_SET_HDR(buf_ptr, WMITLV_TAG_ARRAY_STRUC,
			       WMITLV_GET_STRUCT_TLVLEN(0));
		buf_ptr += WMI_TLV_HDR_SIZE;
		WMITLV_SET_HDR(buf_ptr, WMITLV_TAG_ARRAY_STRUC,
			       WMITLV_GET_STRUCT_TLVLEN(0));
	}
#endif /* WLAN_FEATURE_ROAM_OFFLOAD */
	status = wmi_unified_cmd_send(wma_handle->wmi_handle, buf,
				      len, WMI_ROAM_SCAN_MODE);
	if (status != EOK) {
		WMA_LOGE("wmi_unified_cmd_send WMI_ROAM_SCAN_MODE returned Error %d",
			status);
		cdf_status = CDF_STATUS_E_FAILURE;
		goto error;
	}

	WMA_LOGI("%s: WMA --> WMI_ROAM_SCAN_MODE", __func__);
	return CDF_STATUS_SUCCESS;
error:
	wmi_buf_free(buf);

	return cdf_status;
}

/**
 * wma_roam_scan_offload_rssi_threshold() - set scan offload rssi threashold
 * @wma_handle: wma handle
 * @roam_req:   Roaming request buffer
 *
 * Send WMI_ROAM_SCAN_RSSI_THRESHOLD TLV to firmware
 *
 * Return: CDF status
 */
CDF_STATUS wma_roam_scan_offload_rssi_thresh(tp_wma_handle wma_handle,
	tSirRoamOffloadScanReq *roam_req)
{
	CDF_STATUS cdf_status = CDF_STATUS_SUCCESS;
	wmi_buf_t buf = NULL;
	int status = 0;
	int len, rssi_thresh, rssi_thresh_diff;
	uint8_t *buf_ptr;
	wmi_roam_scan_rssi_threshold_fixed_param *rssi_threshold_fp;
	wmi_roam_scan_extended_threshold_param *ext_thresholds = NULL;
	wmi_roam_earlystop_rssi_thres_param *early_stop_thresholds = NULL;
	struct roam_ext_params *roam_params;
	int32_t good_rssi_threshold;
	uint32_t hirssi_scan_max_count;
	uint32_t hirssi_scan_delta;
	int32_t hirssi_upper_bound;

	/* Send rssi threshold */
	roam_params = &roam_req->roam_params;
	rssi_thresh = roam_req->LookupThreshold - WMA_NOISE_FLOOR_DBM_DEFAULT;
	rssi_thresh_diff = roam_req->OpportunisticScanThresholdDiff;
	hirssi_scan_max_count = roam_req->hi_rssi_scan_max_count;
	hirssi_scan_delta = roam_req->hi_rssi_scan_rssi_delta;
	hirssi_upper_bound = roam_req->hi_rssi_scan_rssi_ub -
				     WMA_NOISE_FLOOR_DBM_DEFAULT;
	len = sizeof(wmi_roam_scan_rssi_threshold_fixed_param);
	len += WMI_TLV_HDR_SIZE; /* TLV for ext_thresholds*/
	len += sizeof(wmi_roam_scan_extended_threshold_param);
	len += WMI_TLV_HDR_SIZE;
	len += sizeof(wmi_roam_earlystop_rssi_thres_param);
	buf = wmi_buf_alloc(wma_handle->wmi_handle, len);
	if (!buf) {
		WMA_LOGE("%s : wmi_buf_alloc failed", __func__);
		return CDF_STATUS_E_NOMEM;
	}

	buf_ptr = (uint8_t *) wmi_buf_data(buf);
	rssi_threshold_fp =
		(wmi_roam_scan_rssi_threshold_fixed_param *) buf_ptr;
	WMITLV_SET_HDR(&rssi_threshold_fp->tlv_header,
		       WMITLV_TAG_STRUC_wmi_roam_scan_rssi_threshold_fixed_param,
		       WMITLV_GET_STRUCT_TLVLEN
			       (wmi_roam_scan_rssi_threshold_fixed_param));
	/* fill in threshold values */
	rssi_threshold_fp->vdev_id = roam_req->sessionId;
	rssi_threshold_fp->roam_scan_rssi_thresh = rssi_thresh & 0x000000ff;
	rssi_threshold_fp->roam_rssi_thresh_diff =
		rssi_thresh_diff & 0x000000ff;
	rssi_threshold_fp->hirssi_scan_max_count = hirssi_scan_max_count;
	rssi_threshold_fp->hirssi_scan_delta = hirssi_scan_delta;
	rssi_threshold_fp->hirssi_upper_bound = hirssi_upper_bound & 0x00000ff;

	buf_ptr += sizeof(wmi_roam_scan_rssi_threshold_fixed_param);
	WMITLV_SET_HDR(buf_ptr,
			WMITLV_TAG_ARRAY_STRUC,
			sizeof(wmi_roam_scan_extended_threshold_param));
	buf_ptr += WMI_TLV_HDR_SIZE;
	ext_thresholds = (wmi_roam_scan_extended_threshold_param *) buf_ptr;
	/*
	 * The current Noise floor in firmware is -96dBm. Penalty/Boost
	 * threshold is applied on a weaker signal to make it even more weaker.
	 * So, there is a chance that the user may configure a very low
	 * Penalty/Boost threshold beyond the noise floor. If that is the case,
	 * then suppress the penalty/boost threshold to the noise floor.
	 */
	if (roam_params->raise_rssi_thresh_5g < WMA_NOISE_FLOOR_DBM_DEFAULT)
		ext_thresholds->penalty_threshold_5g = 0;
	else
		ext_thresholds->boost_threshold_5g =
			(roam_params->raise_rssi_thresh_5g -
			 WMA_NOISE_FLOOR_DBM_DEFAULT) & 0x000000ff;
	if (roam_params->drop_rssi_thresh_5g < WMA_NOISE_FLOOR_DBM_DEFAULT)
		ext_thresholds->penalty_threshold_5g = 0;
	else
		ext_thresholds->penalty_threshold_5g =
			(roam_params->drop_rssi_thresh_5g -
			 WMA_NOISE_FLOOR_DBM_DEFAULT) & 0x000000ff;
	ext_thresholds->boost_algorithm_5g =
		WMI_ROAM_5G_BOOST_PENALIZE_ALGO_LINEAR;
	ext_thresholds->boost_factor_5g = roam_params->raise_factor_5g;
	ext_thresholds->penalty_algorithm_5g =
		WMI_ROAM_5G_BOOST_PENALIZE_ALGO_LINEAR;
	ext_thresholds->penalty_factor_5g = roam_params->drop_factor_5g;
	ext_thresholds->max_boost_5g = roam_params->max_raise_rssi_5g;
	ext_thresholds->max_penalty_5g = roam_params->max_drop_rssi_5g;

	if (roam_params->good_rssi_roam)
		good_rssi_threshold = WMA_NOISE_FLOOR_DBM_DEFAULT;
	else
		good_rssi_threshold = 0;
	ext_thresholds->good_rssi_threshold =
	    (good_rssi_threshold - WMA_NOISE_FLOOR_DBM_DEFAULT) & 0x000000ff;

	WMA_LOGD("WMA --> good_rssi_threshold=%d",
		 ext_thresholds->good_rssi_threshold);

	WMITLV_SET_HDR(&ext_thresholds->tlv_header,
		WMITLV_TAG_STRUC_wmi_roam_scan_extended_threshold_param,
		WMITLV_GET_STRUCT_TLVLEN
		(wmi_roam_scan_extended_threshold_param));
	buf_ptr += sizeof(wmi_roam_scan_extended_threshold_param);
	WMITLV_SET_HDR(buf_ptr,
			WMITLV_TAG_ARRAY_STRUC,
			sizeof(wmi_roam_earlystop_rssi_thres_param));
	buf_ptr += WMI_TLV_HDR_SIZE;
	early_stop_thresholds = (wmi_roam_earlystop_rssi_thres_param *) buf_ptr;
	early_stop_thresholds->roam_earlystop_thres_min =
		roam_req->early_stop_scan_min_threshold -
		WMA_NOISE_FLOOR_DBM_DEFAULT;
	early_stop_thresholds->roam_earlystop_thres_max =
		roam_req->early_stop_scan_max_threshold -
		WMA_NOISE_FLOOR_DBM_DEFAULT;
	WMITLV_SET_HDR(&early_stop_thresholds->tlv_header,
		WMITLV_TAG_STRUC_wmi_roam_earlystop_rssi_thres_param,
		WMITLV_GET_STRUCT_TLVLEN
		(wmi_roam_earlystop_rssi_thres_param));
	WMA_LOGD("early_stop_thresholds en=%d, min=%d, max=%d",
		roam_req->early_stop_scan_enable,
		early_stop_thresholds->roam_earlystop_thres_min,
		early_stop_thresholds->roam_earlystop_thres_max);

	status = wmi_unified_cmd_send(wma_handle->wmi_handle, buf,
				      len, WMI_ROAM_SCAN_RSSI_THRESHOLD);
	if (status != EOK) {
		WMA_LOGE("cmd WMI_ROAM_SCAN_RSSI_THRESHOLD returned Error %d",
			status);
		cdf_status = CDF_STATUS_E_FAILURE;
		goto error;
	}

	WMA_LOGI(FL("roam_scan_rssi_thresh=%d, roam_rssi_thresh_diff=%d"),
		rssi_thresh, rssi_thresh_diff);
	WMA_LOGI(
		FL("hirssi_scan max_count=%d, delta=%d, hirssi_upper_bound=%d"),
		hirssi_scan_max_count, hirssi_scan_delta, hirssi_upper_bound);
	return CDF_STATUS_SUCCESS;
error:
	wmi_buf_free(buf);

	return cdf_status;
}

/**
 * wma_roam_scan_offload_scan_period() - set roam offload scan period
 * @wma_handle: wma handle
 * @scan_period: scan period
 * @scan_age: scan age
 * @vdev_id: vdev id
 *
 * Send WMI_ROAM_SCAN_PERIOD parameters to fw.
 *
 * Return: CDF status
 */
CDF_STATUS wma_roam_scan_offload_scan_period(tp_wma_handle wma_handle,
					     uint32_t scan_period,
					     uint32_t scan_age,
					     uint32_t vdev_id)
{
	CDF_STATUS cdf_status = CDF_STATUS_SUCCESS;
	wmi_buf_t buf = NULL;
	int status = 0;
	int len;
	uint8_t *buf_ptr;
	wmi_roam_scan_period_fixed_param *scan_period_fp;

	/* Send scan period values */
	len = sizeof(wmi_roam_scan_period_fixed_param);
	buf = wmi_buf_alloc(wma_handle->wmi_handle, len);
	if (!buf) {
		WMA_LOGE("%s : wmi_buf_alloc failed", __func__);
		return CDF_STATUS_E_NOMEM;
	}

	buf_ptr = (uint8_t *) wmi_buf_data(buf);
	scan_period_fp = (wmi_roam_scan_period_fixed_param *) buf_ptr;
	WMITLV_SET_HDR(&scan_period_fp->tlv_header,
		       WMITLV_TAG_STRUC_wmi_roam_scan_period_fixed_param,
		       WMITLV_GET_STRUCT_TLVLEN
			       (wmi_roam_scan_period_fixed_param));
	/* fill in scan period values */
	scan_period_fp->vdev_id = vdev_id;
	scan_period_fp->roam_scan_period = scan_period; /* 20 seconds */
	scan_period_fp->roam_scan_age = scan_age;

	status = wmi_unified_cmd_send(wma_handle->wmi_handle, buf,
				      len, WMI_ROAM_SCAN_PERIOD);
	if (status != EOK) {
		WMA_LOGE("wmi_unified_cmd_send WMI_ROAM_SCAN_PERIOD returned Error %d",
			status);
		cdf_status = CDF_STATUS_E_FAILURE;
		goto error;
	}

	WMA_LOGI("%s: WMA --> WMI_ROAM_SCAN_PERIOD roam_scan_period=%d, roam_scan_age=%d",
		__func__, scan_period, scan_age);
	return CDF_STATUS_SUCCESS;
error:
	wmi_buf_free(buf);

	return cdf_status;
}

/**
 * wma_roam_scan_offload_rssi_change() - set roam offload RSSI change threshold
 * @wma_handle: wma handle
 * @rssi_change_thresh: RSSI Change threshold
 * @bcn_rssi_weight: beacon RSSI weight
 * @vdev_id: vdev id
 *
 * Send WMI_ROAM_SCAN_RSSI_CHANGE_THRESHOLD parameters to fw.
 *
 * Return: CDF status
 */
CDF_STATUS wma_roam_scan_offload_rssi_change(tp_wma_handle wma_handle,
	uint32_t vdev_id,
	int32_t rssi_change_thresh,
	uint32_t bcn_rssi_weight,
	uint32_t hirssi_delay_btw_scans)
{
	CDF_STATUS cdf_status = CDF_STATUS_SUCCESS;
	wmi_buf_t buf = NULL;
	int status = 0;
	int len;
	uint8_t *buf_ptr;
	wmi_roam_scan_rssi_change_threshold_fixed_param *rssi_change_fp;

	/* Send rssi change parameters */
	len = sizeof(wmi_roam_scan_rssi_change_threshold_fixed_param);
	buf = wmi_buf_alloc(wma_handle->wmi_handle, len);
	if (!buf) {
		WMA_LOGE("%s : wmi_buf_alloc failed", __func__);
		return CDF_STATUS_E_NOMEM;
	}

	buf_ptr = (uint8_t *) wmi_buf_data(buf);
	rssi_change_fp =
		(wmi_roam_scan_rssi_change_threshold_fixed_param *) buf_ptr;
	WMITLV_SET_HDR(&rssi_change_fp->tlv_header,
		       WMITLV_TAG_STRUC_wmi_roam_scan_rssi_change_threshold_fixed_param,
		       WMITLV_GET_STRUCT_TLVLEN
			       (wmi_roam_scan_rssi_change_threshold_fixed_param));
	/* fill in rssi change threshold (hysteresis) values */
	rssi_change_fp->vdev_id = vdev_id;
	rssi_change_fp->roam_scan_rssi_change_thresh = rssi_change_thresh;
	rssi_change_fp->bcn_rssi_weight = bcn_rssi_weight;
	rssi_change_fp->hirssi_delay_btw_scans = hirssi_delay_btw_scans;

	status = wmi_unified_cmd_send(wma_handle->wmi_handle, buf,
				      len, WMI_ROAM_SCAN_RSSI_CHANGE_THRESHOLD);
	if (status != EOK) {
		WMA_LOGE("wmi_unified_cmd_send WMI_ROAM_SCAN_RSSI_CHANGE_THRESHOLD returned Error %d",
			status);
		cdf_status = CDF_STATUS_E_FAILURE;
		goto error;
	}

	WMA_LOGI(FL("roam_scan_rssi_change_thresh=%d, bcn_rssi_weight=%d"),
		rssi_change_thresh, bcn_rssi_weight);
	WMA_LOGI(FL("hirssi_delay_btw_scans=%d"), hirssi_delay_btw_scans);
	return CDF_STATUS_SUCCESS;
error:
	wmi_buf_free(buf);

	return cdf_status;
}

/**
 * wma_roam_scan_offload_chan_list() - set roam offload channel list
 * @wma_handle: wma handle
 * @chan_count: channel count
 * @chan_list: channel list
 * @list_type: list type
 * @vdev_id: vdev id
 *
 * Set roam offload channel list.
 *
 * Return: CDF status
 */
CDF_STATUS wma_roam_scan_offload_chan_list(tp_wma_handle wma_handle,
					   uint8_t chan_count,
					   uint8_t *chan_list,
					   uint8_t list_type, uint32_t vdev_id)
{
	CDF_STATUS cdf_status = CDF_STATUS_SUCCESS;
	wmi_buf_t buf = NULL;
	int status = 0;
	int len, list_tlv_len;
	int i;
	uint8_t *buf_ptr;
	wmi_roam_chan_list_fixed_param *chan_list_fp;
	A_UINT32 *roam_chan_list_array;

	if (chan_count == 0) {
		WMA_LOGD("%s : invalid number of channels %d", __func__,
			 chan_count);
		return CDF_STATUS_E_EMPTY;
	}
	/* Channel list is a table of 2 TLV's */
	list_tlv_len = WMI_TLV_HDR_SIZE + chan_count * sizeof(A_UINT32);
	len = sizeof(wmi_roam_chan_list_fixed_param) + list_tlv_len;
	buf = wmi_buf_alloc(wma_handle->wmi_handle, len);
	if (!buf) {
		WMA_LOGE("%s : wmi_buf_alloc failed", __func__);
		return CDF_STATUS_E_NOMEM;
	}

	buf_ptr = (uint8_t *) wmi_buf_data(buf);
	chan_list_fp = (wmi_roam_chan_list_fixed_param *) buf_ptr;
	WMITLV_SET_HDR(&chan_list_fp->tlv_header,
		       WMITLV_TAG_STRUC_wmi_roam_chan_list_fixed_param,
		       WMITLV_GET_STRUCT_TLVLEN
			       (wmi_roam_chan_list_fixed_param));
	chan_list_fp->vdev_id = vdev_id;
	chan_list_fp->num_chan = chan_count;
	if (chan_count > 0 && list_type == CHANNEL_LIST_STATIC) {
		/* external app is controlling channel list */
		chan_list_fp->chan_list_type =
			WMI_ROAM_SCAN_CHAN_LIST_TYPE_STATIC;
	} else {
		/* umac supplied occupied channel list in LFR */
		chan_list_fp->chan_list_type =
			WMI_ROAM_SCAN_CHAN_LIST_TYPE_DYNAMIC;
	}

	buf_ptr += sizeof(wmi_roam_chan_list_fixed_param);
	WMITLV_SET_HDR(buf_ptr, WMITLV_TAG_ARRAY_UINT32,
		       (chan_list_fp->num_chan * sizeof(uint32_t)));
	roam_chan_list_array = (A_UINT32 *) (buf_ptr + WMI_TLV_HDR_SIZE);
	WMA_LOGI("%s: %d channels = ", __func__, chan_list_fp->num_chan);
	for (i = 0; ((i < chan_list_fp->num_chan) &&
		     (i < SIR_ROAM_MAX_CHANNELS)); i++) {
		roam_chan_list_array[i] = cds_chan_to_freq(chan_list[i]);
		WMA_LOGI("%d,", roam_chan_list_array[i]);
	}

	status = wmi_unified_cmd_send(wma_handle->wmi_handle, buf,
				      len, WMI_ROAM_CHAN_LIST);
	if (status != EOK) {
		WMA_LOGE("wmi_unified_cmd_send WMI_ROAM_CHAN_LIST returned Error %d",
			status);
		cdf_status = CDF_STATUS_E_FAILURE;
		goto error;
	}

	WMA_LOGI("%s: WMA --> WMI_ROAM_SCAN_CHAN_LIST", __func__);
	return CDF_STATUS_SUCCESS;
error:
	wmi_buf_free(buf);

	return cdf_status;
}

/**
 * e_csr_auth_type_to_rsn_authmode() - map csr auth type to rsn authmode
 * @authtype: CSR authtype
 * @encr: CSR Encryption
 *
 * Map CSR's authentication type into RSN auth mode used by firmware
 *
 * Return: WMI RSN auth mode
 */
A_UINT32 e_csr_auth_type_to_rsn_authmode(eCsrAuthType authtype,
					 eCsrEncryptionType encr)
{
	switch (authtype) {
	case eCSR_AUTH_TYPE_OPEN_SYSTEM:
		return WMI_AUTH_OPEN;
	case eCSR_AUTH_TYPE_WPA:
		return WMI_AUTH_WPA;
	case eCSR_AUTH_TYPE_WPA_PSK:
		return WMI_AUTH_WPA_PSK;
	case eCSR_AUTH_TYPE_RSN:
		return WMI_AUTH_RSNA;
	case eCSR_AUTH_TYPE_RSN_PSK:
		return WMI_AUTH_RSNA_PSK;
#if defined WLAN_FEATURE_VOWIFI_11R
	case eCSR_AUTH_TYPE_FT_RSN:
		return WMI_AUTH_FT_RSNA;
	case eCSR_AUTH_TYPE_FT_RSN_PSK:
		return WMI_AUTH_FT_RSNA_PSK;
#endif /* WLAN_FEATURE_VOWIFI_11R */
#ifdef FEATURE_WLAN_WAPI
	case eCSR_AUTH_TYPE_WAPI_WAI_CERTIFICATE:
		return WMI_AUTH_WAPI;
	case eCSR_AUTH_TYPE_WAPI_WAI_PSK:
		return WMI_AUTH_WAPI_PSK;
#endif /* FEATURE_WLAN_WAPI */
#ifdef FEATURE_WLAN_ESE
	case eCSR_AUTH_TYPE_CCKM_WPA:
	case eCSR_AUTH_TYPE_CCKM_RSN:
		return WMI_AUTH_CCKM;
#endif /* FEATURE_WLAN_ESE */
#ifdef WLAN_FEATURE_11W
	case eCSR_AUTH_TYPE_RSN_PSK_SHA256:
		return WMI_AUTH_RSNA_PSK_SHA256;
	case eCSR_AUTH_TYPE_RSN_8021X_SHA256:
		return WMI_AUTH_RSNA_8021X_SHA256;
#endif /* WLAN_FEATURE_11W */
	case eCSR_AUTH_TYPE_NONE:
	case eCSR_AUTH_TYPE_AUTOSWITCH:
		/* In case of WEP and other keys, NONE means OPEN auth */
		if (encr == eCSR_ENCRYPT_TYPE_WEP40_STATICKEY ||
		    encr == eCSR_ENCRYPT_TYPE_WEP104_STATICKEY ||
		    encr == eCSR_ENCRYPT_TYPE_WEP40 ||
		    encr == eCSR_ENCRYPT_TYPE_WEP104 ||
		    encr == eCSR_ENCRYPT_TYPE_TKIP ||
		    encr == eCSR_ENCRYPT_TYPE_AES) {
			return WMI_AUTH_OPEN;
		}
		return WMI_AUTH_NONE;
	default:
		return WMI_AUTH_NONE;
	}
}

/**
 * e_csr_encryption_type_to_rsn_cipherset() - map csr enc type to ESN cipher
 * @encr: CSR Encryption
 *
 * Map CSR's encryption type into RSN cipher types used by firmware
 *
 * Return: WMI RSN cipher
 */
A_UINT32 e_csr_encryption_type_to_rsn_cipherset(eCsrEncryptionType encr)
{

	switch (encr) {
	case eCSR_ENCRYPT_TYPE_WEP40_STATICKEY:
	case eCSR_ENCRYPT_TYPE_WEP104_STATICKEY:
	case eCSR_ENCRYPT_TYPE_WEP40:
	case eCSR_ENCRYPT_TYPE_WEP104:
		return WMI_CIPHER_WEP;
	case eCSR_ENCRYPT_TYPE_TKIP:
		return WMI_CIPHER_TKIP;
	case eCSR_ENCRYPT_TYPE_AES:
		return WMI_CIPHER_AES_CCM;
#ifdef FEATURE_WLAN_WAPI
	case eCSR_ENCRYPT_TYPE_WPI:
		return WMI_CIPHER_WAPI;
#endif /* FEATURE_WLAN_WAPI */
	case eCSR_ENCRYPT_TYPE_ANY:
		return WMI_CIPHER_ANY;
	case eCSR_ENCRYPT_TYPE_NONE:
	default:
		return WMI_CIPHER_NONE;
	}
}

/**
 * wma_roam_scan_fill_ap_profile() - fill ap_profile
 * @wma_handle: wma handle
 * @pMac: Mac ptr
 * @roam_req: roam offload scan request
 * @ap_profile_p: ap profile
 *
 * Fill ap_profile structure from configured parameters
 *
 * Return: none
 */
void wma_roam_scan_fill_ap_profile(tp_wma_handle wma_handle,
				   tpAniSirGlobal pMac,
				   tSirRoamOffloadScanReq *roam_req,
				   wmi_ap_profile *ap_profile_p)
{
	cdf_mem_zero(ap_profile_p, sizeof(wmi_ap_profile));
	if (roam_req == NULL) {
		ap_profile_p->ssid.ssid_len = 0;
		ap_profile_p->ssid.ssid[0] = 0;
		ap_profile_p->rsn_authmode = WMI_AUTH_NONE;
		ap_profile_p->rsn_ucastcipherset = WMI_CIPHER_NONE;
		ap_profile_p->rsn_mcastcipherset = WMI_CIPHER_NONE;
		ap_profile_p->rsn_mcastmgmtcipherset = WMI_CIPHER_NONE;
		ap_profile_p->rssi_threshold = WMA_ROAM_RSSI_DIFF_DEFAULT;
	} else {
		ap_profile_p->ssid.ssid_len =
			roam_req->ConnectedNetwork.ssId.length;
		cdf_mem_copy(ap_profile_p->ssid.ssid,
			     roam_req->ConnectedNetwork.ssId.ssId,
			     ap_profile_p->ssid.ssid_len);
		ap_profile_p->rsn_authmode =
			e_csr_auth_type_to_rsn_authmode(roam_req->ConnectedNetwork.authentication,
							roam_req->ConnectedNetwork.encryption);
		ap_profile_p->rsn_ucastcipherset =
			e_csr_encryption_type_to_rsn_cipherset(roam_req->ConnectedNetwork.encryption);
		ap_profile_p->rsn_mcastcipherset =
			e_csr_encryption_type_to_rsn_cipherset(roam_req->ConnectedNetwork.mcencryption);
		ap_profile_p->rsn_mcastmgmtcipherset =
			ap_profile_p->rsn_mcastcipherset;
		ap_profile_p->rssi_threshold = roam_req->RoamRssiDiff;
#ifdef WLAN_FEATURE_11W
		if (roam_req->ConnectedNetwork.mfp_enabled)
			ap_profile_p->flags |= WMI_AP_PROFILE_FLAG_PMF;
#endif
	}
}

/**
 * wma_roam_scan_scan_params() - fill roam scan params
 * @wma_handle: wma handle
 * @pMac: Mac ptr
 * @scan_params: scan parameters
 * @roam_req: NULL if this routine is called before connect
 *            It will be non-NULL if called after assoc.
 *
 * Fill scan_params structure from configured parameters
 *
 * Return: none
 */
void wma_roam_scan_fill_scan_params(tp_wma_handle wma_handle,
				    tpAniSirGlobal pMac,
				    tSirRoamOffloadScanReq *roam_req,
				    wmi_start_scan_cmd_fixed_param *
				    scan_params)
{
	uint8_t channels_per_burst = 0;
	uint32_t val = 0;

	if (NULL == pMac) {
		WMA_LOGE("%s: pMac is NULL", __func__);
		return;
	}

	cdf_mem_zero(scan_params, sizeof(wmi_start_scan_cmd_fixed_param));
	scan_params->scan_ctrl_flags = WMI_SCAN_ADD_CCK_RATES |
				       WMI_SCAN_ADD_OFDM_RATES |
				       WMI_SCAN_ADD_DS_IE_IN_PROBE_REQ;
	if (roam_req != NULL) {
		/* Parameters updated after association is complete */
		WMA_LOGI("%s: Input parameters: NeighborScanChannelMinTime"
			 " = %d, NeighborScanChannelMaxTime = %d",
			 __func__,
			 roam_req->NeighborScanChannelMinTime,
			 roam_req->NeighborScanChannelMaxTime);
		WMA_LOGI("%s: Input parameters: NeighborScanTimerPeriod ="
			 " %d, HomeAwayTime = %d, nProbes = %d",
			 __func__,
			 roam_req->NeighborScanTimerPeriod,
			 roam_req->HomeAwayTime, roam_req->nProbes);

		/*
		 * roam_req->NeighborScanChannelMaxTime = SCAN_CHANNEL_TIME
		 * roam_req->HomeAwayTime               = SCAN_HOME_AWAY_TIME
		 * roam_req->NeighborScanTimerPeriod    = SCAN_HOME_TIME
		 *
		 * scan_params->dwell_time_active  = time station stays on channel
		 *                                   and sends probes;
		 * scan_params->dwell_time_passive = time station stays on channel
		 *                                   and listens probes;
		 * scan_params->burst_duration     = time station goes off channel
		 *                                   to scan;
		 */

		if (wlan_cfg_get_int
			    (pMac, WNI_CFG_PASSIVE_MAXIMUM_CHANNEL_TIME,
			    &val) != eSIR_SUCCESS) {
			/*
			 * Could not get max channel value from CFG. Log error.
			 */
			WMA_LOGE
				("could not retrieve passive max channel value");

			/* use a default value of 110ms */
			val = WMA_ROAM_DWELL_TIME_PASSIVE_DEFAULT;
		}

		scan_params->dwell_time_passive = val;
		/*
		 * Here is the formula,
		 * T(HomeAway) = N * T(dwell) + (N+1) * T(cs)
		 * where N is number of channels scanned in single burst
		 */
		scan_params->dwell_time_active =
			roam_req->NeighborScanChannelMaxTime;
		if (roam_req->HomeAwayTime <
		    2 * WMA_ROAM_SCAN_CHANNEL_SWITCH_TIME) {
			/* clearly we can't follow home away time.
			 * Make it a split scan.
			 */
			scan_params->burst_duration = 0;
		} else {
			channels_per_burst =
				(roam_req->HomeAwayTime -
				 WMA_ROAM_SCAN_CHANNEL_SWITCH_TIME)
				/ (scan_params->dwell_time_active +
				   WMA_ROAM_SCAN_CHANNEL_SWITCH_TIME);

			if (channels_per_burst < 1) {
				/* dwell time and home away time conflicts */
				/* we will override dwell time */
				scan_params->dwell_time_active =
					roam_req->HomeAwayTime -
					2 * WMA_ROAM_SCAN_CHANNEL_SWITCH_TIME;
				scan_params->burst_duration =
					scan_params->dwell_time_active;
			} else {
				scan_params->burst_duration =
					channels_per_burst *
					scan_params->dwell_time_active;
			}
		}
		if (roam_req->allowDFSChannelRoam ==
		    SIR_ROAMING_DFS_CHANNEL_ENABLED_NORMAL
		    && roam_req->HomeAwayTime > 0
		    && roam_req->ChannelCacheType != CHANNEL_LIST_STATIC) {
			/* Roaming on DFS channels is supported and it is not
			 * app channel list. It is ok to override homeAwayTime
			 * to accomodate DFS dwell time in burst
			 * duration.
			 */
			scan_params->burst_duration =
				CDF_MAX(scan_params->burst_duration,
					scan_params->dwell_time_passive);
		}
		scan_params->min_rest_time = roam_req->NeighborScanTimerPeriod;
		scan_params->max_rest_time = roam_req->NeighborScanTimerPeriod;
		scan_params->repeat_probe_time = (roam_req->nProbes > 0) ?
						 CDF_MAX(scan_params->dwell_time_active / roam_req->nProbes,
							 1) : 0;
		scan_params->probe_spacing_time = 0;
		scan_params->probe_delay = 0;
		/* 30 seconds for full scan cycle */
		scan_params->max_scan_time = WMA_HW_DEF_SCAN_MAX_DURATION;
		scan_params->idle_time = scan_params->min_rest_time;
		scan_params->n_probes = roam_req->nProbes;
		if (roam_req->allowDFSChannelRoam ==
		    SIR_ROAMING_DFS_CHANNEL_DISABLED) {
			scan_params->scan_ctrl_flags |= WMI_SCAN_BYPASS_DFS_CHN;
		} else {
			/* Roaming scan on DFS channel is allowed.
			 * No need to change any flags for default
			 * allowDFSChannelRoam = 1.
			 * Special case where static channel list is given by\
			 * application that contains DFS channels.
			 * Assume that the application has knowledge of matching
			 * APs being active and that probe request transmission
			 * is permitted on those channel.
			 * Force active scans on those channels.
			 */

			if (roam_req->allowDFSChannelRoam ==
			    SIR_ROAMING_DFS_CHANNEL_ENABLED_ACTIVE &&
			    roam_req->ChannelCacheType == CHANNEL_LIST_STATIC &&
			    roam_req->ConnectedNetwork.ChannelCount > 0) {
				scan_params->scan_ctrl_flags |=
					WMI_SCAN_FLAG_FORCE_ACTIVE_ON_DFS;
			}
		}
	} else {
		/* roam_req = NULL during initial or pre-assoc invocation */
		scan_params->dwell_time_active =
			WMA_ROAM_DWELL_TIME_ACTIVE_DEFAULT;
		scan_params->dwell_time_passive =
			WMA_ROAM_DWELL_TIME_PASSIVE_DEFAULT;
		scan_params->min_rest_time = WMA_ROAM_MIN_REST_TIME_DEFAULT;
		scan_params->max_rest_time = WMA_ROAM_MAX_REST_TIME_DEFAULT;
		scan_params->repeat_probe_time = 0;
		scan_params->probe_spacing_time = 0;
		scan_params->probe_delay = 0;
		scan_params->max_scan_time = WMA_HW_DEF_SCAN_MAX_DURATION;
		scan_params->idle_time = scan_params->min_rest_time;
		scan_params->burst_duration = 0;
		scan_params->n_probes = 0;
	}

	WMA_LOGI("%s: Rome roam scan parameters:"
		 " dwell_time_active = %d, dwell_time_passive = %d",
		 __func__,
		 scan_params->dwell_time_active,
		 scan_params->dwell_time_passive);
	WMA_LOGI("%s: min_rest_time = %d, max_rest_time = %d,"
		 " repeat_probe_time = %d n_probes = %d",
		 __func__,
		 scan_params->min_rest_time,
		 scan_params->max_rest_time,
		 scan_params->repeat_probe_time, scan_params->n_probes);
	WMA_LOGI("%s: max_scan_time = %d, idle_time = %d,"
		 " burst_duration = %d, scan_ctrl_flags = 0x%x",
		 __func__,
		 scan_params->max_scan_time,
		 scan_params->idle_time,
		 scan_params->burst_duration, scan_params->scan_ctrl_flags);
}

/**
 * wma_roam_scan_offload_ap_profile() - set roam ap profile in fw
 * @wma_handle: wma handle
 * @ap_profile_p: ap profile
 * @vdev_id: vdev id
 *
 * Send WMI_ROAM_AP_PROFILE to firmware
 *
 * Return: CDF status
 */
CDF_STATUS wma_roam_scan_offload_ap_profile(tp_wma_handle wma_handle,
					    wmi_ap_profile *ap_profile_p,
					    uint32_t vdev_id)
{
	CDF_STATUS cdf_status = CDF_STATUS_SUCCESS;
	wmi_buf_t buf = NULL;
	int status = 0;
	int len;
	uint8_t *buf_ptr;
	wmi_roam_ap_profile_fixed_param *roam_ap_profile_fp;

	len = sizeof(wmi_roam_ap_profile_fixed_param) + sizeof(wmi_ap_profile);

	buf = wmi_buf_alloc(wma_handle->wmi_handle, len);
	if (!buf) {
		WMA_LOGE("%s : wmi_buf_alloc failed", __func__);
		return CDF_STATUS_E_NOMEM;
	}

	buf_ptr = (uint8_t *) wmi_buf_data(buf);
	roam_ap_profile_fp = (wmi_roam_ap_profile_fixed_param *) buf_ptr;
	WMITLV_SET_HDR(&roam_ap_profile_fp->tlv_header,
		       WMITLV_TAG_STRUC_wmi_roam_ap_profile_fixed_param,
		       WMITLV_GET_STRUCT_TLVLEN
			       (wmi_roam_ap_profile_fixed_param));
	/* fill in threshold values */
	roam_ap_profile_fp->vdev_id = vdev_id;
	roam_ap_profile_fp->id = 0;
	buf_ptr += sizeof(wmi_roam_ap_profile_fixed_param);

	cdf_mem_copy(buf_ptr, ap_profile_p, sizeof(wmi_ap_profile));
	WMITLV_SET_HDR(buf_ptr,
		       WMITLV_TAG_STRUC_wmi_ap_profile,
		       WMITLV_GET_STRUCT_TLVLEN(wmi_ap_profile));
	status = wmi_unified_cmd_send(wma_handle->wmi_handle, buf,
				      len, WMI_ROAM_AP_PROFILE);
	if (status != EOK) {
		WMA_LOGE("wmi_unified_cmd_send WMI_ROAM_AP_PROFILE returned Error %d",
			status);
		cdf_status = CDF_STATUS_E_FAILURE;
		goto error;
	}

	WMA_LOGI("WMA --> WMI_ROAM_AP_PROFILE and other parameters");
	return CDF_STATUS_SUCCESS;
error:
	wmi_buf_free(buf);

	return cdf_status;
}

/**
 * wma_roam_scan_filter() - Filter to be applied while roaming
 * @wma_handle:     Global WMA Handle
 * @roam_req:       Request which contains the filters
 *
 * There are filters such as whitelist, blacklist and preferred
 * list that need to be applied to the scan results to form the
 * probable candidates for roaming.
 *
 * Return: Return success upon succesfully passing the
 *         parameters to the firmware, otherwise failure.
 */
CDF_STATUS wma_roam_scan_filter(tp_wma_handle wma_handle,
	tSirRoamOffloadScanReq *roam_req)
{
	wmi_buf_t buf = NULL;
	int status = 0, i;
	uint32_t len, num_bssid_black_list = 0, num_ssid_white_list = 0,
	   num_bssid_preferred_list = 0;
	uint32_t op_bitmap = 0;
	uint8_t *buf_ptr;
	wmi_roam_filter_fixed_param *roam_filter;
	uint8_t *bssid_src_ptr = NULL;
	wmi_mac_addr *bssid_dst_ptr = NULL;
	wmi_ssid *ssid_ptr = NULL;
	uint32_t *bssid_preferred_factor_ptr = NULL;
	struct roam_ext_params *roam_params;

	roam_params = &roam_req->roam_params;
	len = sizeof(wmi_roam_filter_fixed_param);
	len += WMI_TLV_HDR_SIZE;
	if (roam_req->Command != ROAM_SCAN_OFFLOAD_STOP) {
		switch (roam_req->reason) {
		case REASON_ROAM_SET_BLACKLIST_BSSID:
			op_bitmap |= 0x1;
			num_bssid_black_list =
				roam_params->num_bssid_avoid_list;
			len += num_bssid_black_list * sizeof(wmi_mac_addr);
			len += WMI_TLV_HDR_SIZE;
			break;
		case REASON_ROAM_SET_SSID_ALLOWED:
			op_bitmap |= 0x2;
			num_ssid_white_list =
				roam_params->num_ssid_allowed_list;
			len += num_ssid_white_list * sizeof(wmi_ssid);
			len += WMI_TLV_HDR_SIZE;
			break;
		case REASON_ROAM_SET_FAVORED_BSSID:
			op_bitmap |= 0x4;
			num_bssid_preferred_list =
				roam_params->num_bssid_favored;
			len += num_bssid_preferred_list * sizeof(wmi_mac_addr);
			len += WMI_TLV_HDR_SIZE;
			len += num_bssid_preferred_list * sizeof(A_UINT32);
			break;
		default:
			WMA_LOGD("%s : Roam Filter need not be sent", __func__);
			return CDF_STATUS_SUCCESS;
			break;
		}
	} else {
		/* In case of STOP command, reset all the variables
		 * except for blacklist BSSID which should be retained
		 * across connections.*/
		op_bitmap = 0x2 | 0x4;
		num_ssid_white_list = roam_params->num_ssid_allowed_list;
		len += num_ssid_white_list * sizeof(wmi_ssid);
		num_bssid_preferred_list = roam_params->num_bssid_favored;
		len += num_bssid_preferred_list * sizeof(wmi_mac_addr);
		len += num_bssid_preferred_list * sizeof(A_UINT32);
		len += (2 * WMI_TLV_HDR_SIZE);
	}
	buf = wmi_buf_alloc(wma_handle->wmi_handle, len);
	if (!buf) {
		WMA_LOGE("%s : wmi_buf_alloc failed", __func__);
		return CDF_STATUS_E_NOMEM;
	}

	buf_ptr = (u_int8_t *) wmi_buf_data(buf);
	roam_filter = (wmi_roam_filter_fixed_param *) buf_ptr;
	WMITLV_SET_HDR(&roam_filter->tlv_header,
		WMITLV_TAG_STRUC_wmi_roam_filter_fixed_param,
		WMITLV_GET_STRUCT_TLVLEN(wmi_roam_filter_fixed_param));
	/* fill in fixed values */
	roam_filter->vdev_id = roam_req->sessionId;
	roam_filter->flags = 0;
	roam_filter->op_bitmap = op_bitmap;
	roam_filter->num_bssid_black_list = num_bssid_black_list;
	roam_filter->num_ssid_white_list = num_ssid_white_list;
	roam_filter->num_bssid_preferred_list = num_bssid_preferred_list;
	buf_ptr += sizeof(wmi_roam_filter_fixed_param);

	WMITLV_SET_HDR((buf_ptr),
		WMITLV_TAG_ARRAY_FIXED_STRUC,
		(num_bssid_black_list * sizeof(wmi_mac_addr)));
	bssid_src_ptr = (uint8_t *)&roam_params->bssid_avoid_list;
	bssid_dst_ptr = (wmi_mac_addr *)(buf_ptr + WMI_TLV_HDR_SIZE);
	for (i = 0; i < num_bssid_black_list; i++) {
		WMI_CHAR_ARRAY_TO_MAC_ADDR(bssid_src_ptr, bssid_dst_ptr);
		bssid_src_ptr += ATH_MAC_LEN;
		bssid_dst_ptr++;
	}
	buf_ptr += WMI_TLV_HDR_SIZE +
		(num_bssid_black_list * sizeof(wmi_mac_addr));
	WMITLV_SET_HDR((buf_ptr),
		WMITLV_TAG_ARRAY_FIXED_STRUC,
		(num_ssid_white_list * sizeof(wmi_ssid)));
	ssid_ptr = (wmi_ssid *)(buf_ptr + WMI_TLV_HDR_SIZE);
	for (i = 0; i < num_ssid_white_list; i++) {
		memcpy(&ssid_ptr->ssid, &roam_params->ssid_allowed_list[i].ssId,
			roam_params->ssid_allowed_list[i].length);
		ssid_ptr->ssid_len = roam_params->ssid_allowed_list[i].length;
		WMA_LOGD("%s: SSID length=%d", __func__, ssid_ptr->ssid_len);
		CDF_TRACE_HEX_DUMP(CDF_MODULE_ID_WMA, CDF_TRACE_LEVEL_DEBUG,
				(uint8_t *)ssid_ptr->ssid,
				ssid_ptr->ssid_len);
		ssid_ptr++;
	}
	buf_ptr += WMI_TLV_HDR_SIZE + (num_ssid_white_list * sizeof(wmi_ssid));
	WMITLV_SET_HDR((buf_ptr),
		WMITLV_TAG_ARRAY_FIXED_STRUC,
		(num_bssid_preferred_list * sizeof(wmi_mac_addr)));
	bssid_src_ptr = (uint8_t *)&roam_params->bssid_favored;
	bssid_dst_ptr = (wmi_mac_addr *)(buf_ptr + WMI_TLV_HDR_SIZE);
	for (i = 0; i < num_bssid_preferred_list; i++) {
		WMI_CHAR_ARRAY_TO_MAC_ADDR(bssid_src_ptr,
				(wmi_mac_addr *)bssid_dst_ptr);
		bssid_src_ptr += ATH_MAC_LEN;
		bssid_dst_ptr++;
	}
	buf_ptr += WMI_TLV_HDR_SIZE +
		(num_bssid_preferred_list * sizeof(wmi_mac_addr));
	WMITLV_SET_HDR(buf_ptr, WMITLV_TAG_ARRAY_UINT32,
			(num_bssid_preferred_list * sizeof(uint32_t)));
	bssid_preferred_factor_ptr = (uint32_t *)(buf_ptr + WMI_TLV_HDR_SIZE);
	for (i = 0; i < num_bssid_preferred_list; i++) {
		*bssid_preferred_factor_ptr =
			roam_params->bssid_favored_factor[i];
		bssid_preferred_factor_ptr++;
	}
	buf_ptr += WMI_TLV_HDR_SIZE +
		(num_bssid_preferred_list * sizeof(uint32_t));
	status = wmi_unified_cmd_send(wma_handle->wmi_handle, buf,
		len, WMI_ROAM_FILTER_CMDID);
	if (status != EOK) {
		WMA_LOGE("cmd WMI_ROAM_FILTER_CMDID returned Error %d",
				status);
		goto error;
	}
	return CDF_STATUS_SUCCESS;
error:
	wmi_buf_free(buf);
	return CDF_STATUS_E_FAILURE;
}

/**
 * wma_roam_scan_bmiss_cnt() - set bmiss count to fw
 * @wma_handle: wma handle
 * @first_bcnt: first bmiss count
 * @final_bcnt: final bmiss count
 * @vdev_id: vdev id
 *
 * set first & final biss count to fw.
 *
 * Return: CDF status
 */
CDF_STATUS wma_roam_scan_bmiss_cnt(tp_wma_handle wma_handle,
				   A_INT32 first_bcnt,
				   A_UINT32 final_bcnt, uint32_t vdev_id)
{
	int status = 0;

	WMA_LOGI("%s: first_bcnt=%d, final_bcnt=%d", __func__, first_bcnt,
		 final_bcnt);

	status = wmi_unified_vdev_set_param_send(wma_handle->wmi_handle,
						 vdev_id,
						 WMI_VDEV_PARAM_BMISS_FIRST_BCNT,
						 first_bcnt);
	if (status != EOK) {
		WMA_LOGE("wmi_unified_vdev_set_param_send WMI_VDEV_PARAM_BMISS_FIRST_BCNT returned Error %d",
			status);
		return CDF_STATUS_E_FAILURE;
	}

	status = wmi_unified_vdev_set_param_send(wma_handle->wmi_handle,
						 vdev_id,
						 WMI_VDEV_PARAM_BMISS_FINAL_BCNT,
						 final_bcnt);
	if (status != EOK) {
		WMA_LOGE("wmi_unified_vdev_set_param_send WMI_VDEV_PARAM_BMISS_FINAL_BCNT returned Error %d",
			status);
		return CDF_STATUS_E_FAILURE;
	}

	return CDF_STATUS_SUCCESS;
}

/**
 * wma_roam_scan_offload_command() - set roam offload command
 * @wma_handle: wma handle
 * @command: command
 * @vdev_id: vdev id
 *
 * This function set roam offload command to fw.
 *
 * Return: CDF status
 */
CDF_STATUS wma_roam_scan_offload_command(tp_wma_handle wma_handle,
					 uint32_t command, uint32_t vdev_id)
{
	CDF_STATUS cdf_status;
	wmi_roam_scan_cmd_fixed_param *cmd_fp;
	wmi_buf_t buf = NULL;
	int status = 0;
	int len;
	uint8_t *buf_ptr;

	len = sizeof(wmi_roam_scan_cmd_fixed_param);
	buf = wmi_buf_alloc(wma_handle->wmi_handle, len);
	if (!buf) {
		WMA_LOGE("%s : wmi_buf_alloc failed", __func__);
		return CDF_STATUS_E_NOMEM;
	}

	buf_ptr = (uint8_t *) wmi_buf_data(buf);

	cmd_fp = (wmi_roam_scan_cmd_fixed_param *) buf_ptr;
	WMITLV_SET_HDR(&cmd_fp->tlv_header,
		       WMITLV_TAG_STRUC_wmi_roam_scan_cmd_fixed_param,
		       WMITLV_GET_STRUCT_TLVLEN(wmi_roam_scan_cmd_fixed_param));
	cmd_fp->vdev_id = vdev_id;
	cmd_fp->command_arg = command;

	status = wmi_unified_cmd_send(wma_handle->wmi_handle, buf,
				      len, WMI_ROAM_SCAN_CMD);
	if (status != EOK) {
		WMA_LOGE("wmi_unified_cmd_send WMI_ROAM_SCAN_CMD returned Error %d",
			status);
		cdf_status = CDF_STATUS_E_FAILURE;
		goto error;
	}

	WMA_LOGI("%s: WMA --> WMI_ROAM_SCAN_CMD", __func__);
	return CDF_STATUS_SUCCESS;

error:
	wmi_buf_free(buf);

	return cdf_status;
}

/**
 * wma_process_roam_scan_req() - process roam request
 * @wma_handle: wma handle
 * @roam_req: roam request parameters
 *
 * Main routine to handle ROAM commands coming from CSR module.
 *
 * Return: CDF status
 */
CDF_STATUS wma_process_roam_scan_req(tp_wma_handle wma_handle,
				     tSirRoamOffloadScanReq *roam_req)
{
	CDF_STATUS cdf_status = CDF_STATUS_SUCCESS;
	wmi_start_scan_cmd_fixed_param scan_params;
	wmi_ap_profile ap_profile;
	tpAniSirGlobal pMac = cds_get_context(CDF_MODULE_ID_PE);
	uint32_t mode = 0;
	struct wma_txrx_node *intr = NULL;

	WMA_LOGI("%s: command 0x%x, reason %d", __func__, roam_req->Command,
		 roam_req->reason);

	if (NULL == pMac) {
		WMA_LOGE("%s: pMac is NULL", __func__);
		return CDF_STATUS_E_FAILURE;
	}

	if (!wma_handle->roam_offload_enabled) {
		/* roam scan offload is not enabled in firmware.
		 * Cannot initialize it in the middle of connection.
		 */
		cdf_mem_free(roam_req);
		return CDF_STATUS_E_PERM;
	}
	switch (roam_req->Command) {
	case ROAM_SCAN_OFFLOAD_START:
		intr = &wma_handle->interfaces[roam_req->sessionId];
		intr->delay_before_vdev_stop = roam_req->delay_before_vdev_stop;
		/*
		 * Scan/Roam threshold parameters are translated from fields of
		 * tSirRoamOffloadScanReq to WMITLV values sent to Rome firmware.
		 * some of these parameters are configurable in qcom_cfg.ini file.
		 */

		/* First parameter is positive rssi value to trigger rssi based scan.
		 * Opportunistic scan is started at 30 dB higher that trigger rssi.
		 */
		wma_handle->suitable_ap_hb_failure = false;

		cdf_status = wma_roam_scan_offload_rssi_thresh(wma_handle,
								roam_req);
		if (cdf_status != CDF_STATUS_SUCCESS)
			break;
		cdf_status = wma_roam_scan_bmiss_cnt(wma_handle,
						     roam_req->RoamBmissFirstBcnt,
						     roam_req->RoamBmissFinalBcnt,
						     roam_req->sessionId);
		if (cdf_status != CDF_STATUS_SUCCESS)
			break;

		/* Opportunistic scan runs on a timer, value set by
		 * EmptyRefreshScanPeriod. Age out the entries after 3 such
		 * cycles.
		 */
		if (roam_req->EmptyRefreshScanPeriod > 0) {
			cdf_status =
				wma_roam_scan_offload_scan_period(wma_handle,
								  roam_req->EmptyRefreshScanPeriod,
								  roam_req->EmptyRefreshScanPeriod * 3,
								  roam_req->sessionId);
			if (cdf_status != CDF_STATUS_SUCCESS)
				break;

			mode = WMI_ROAM_SCAN_MODE_PERIODIC;
			/* Don't use rssi triggered roam scans if external app
			 * is in control of channel list.
			 */
			if (roam_req->ChannelCacheType != CHANNEL_LIST_STATIC)
				mode |= WMI_ROAM_SCAN_MODE_RSSI_CHANGE;

		} else {
			mode = WMI_ROAM_SCAN_MODE_RSSI_CHANGE;
		}

		/* Start new rssi triggered scan only if it changes by RoamRssiDiff value.
		 * Beacon weight of 14 means average rssi is taken over 14 previous samples +
		 * 2 times the current beacon's rssi.
		 */
		cdf_status = wma_roam_scan_offload_rssi_change(wma_handle,
					roam_req->sessionId,
					roam_req->RoamRescanRssiDiff,
					roam_req->RoamBeaconRssiWeight,
					roam_req->hi_rssi_scan_delay);

		if (cdf_status != CDF_STATUS_SUCCESS)
			break;

		wma_roam_scan_fill_ap_profile(wma_handle, pMac, roam_req,
					      &ap_profile);

		cdf_status = wma_roam_scan_offload_ap_profile(wma_handle,
							      &ap_profile,
							      roam_req->sessionId);
		if (cdf_status != CDF_STATUS_SUCCESS)
			break;

		cdf_status = wma_roam_scan_offload_chan_list(wma_handle,
							     roam_req->ConnectedNetwork.ChannelCount,
							     &roam_req->ConnectedNetwork.ChannelCache[0],
							     roam_req->ChannelCacheType,
							     roam_req->sessionId);
		if ((cdf_status != CDF_STATUS_SUCCESS) &&
			(cdf_status != CDF_STATUS_E_EMPTY))
			break;


		wma_roam_scan_fill_scan_params(wma_handle, pMac, roam_req,
					       &scan_params);
		cdf_status =
			wma_roam_scan_offload_mode(wma_handle, &scan_params,
						   roam_req, mode,
						   roam_req->sessionId);
		if (cdf_status != CDF_STATUS_SUCCESS)
			break;
		cdf_status = wma_roam_scan_filter(wma_handle, roam_req);
		if (cdf_status != CDF_STATUS_SUCCESS) {
			WMA_LOGE("Sending start for roam scan filter failed");
			break;
		}
		break;

	case ROAM_SCAN_OFFLOAD_STOP:
		wma_handle->suitable_ap_hb_failure = false;
		if (wma_handle->roam_offload_enabled) {

			wma_roam_scan_fill_scan_params(wma_handle, pMac,
						       NULL, &scan_params);
			cdf_status = wma_roam_scan_offload_mode(wma_handle,
								&scan_params,
								NULL,
								WMI_ROAM_SCAN_MODE_NONE,
								roam_req->sessionId);
		}
		/*
		 * If the STOP command is due to a disconnect, then
		 * send the filter command to clear all the filter
		 * entries. If it is roaming scenario, then do not
		 * send the cleared entries.
		 */
		if (!roam_req->middle_of_roaming) {
			cdf_status = wma_roam_scan_filter(wma_handle, roam_req);
			if (cdf_status != CDF_STATUS_SUCCESS) {
				WMA_LOGE("clear for roam scan filter failed");
				break;
			}
		}

		if (roam_req->reason ==
		    REASON_OS_REQUESTED_ROAMING_NOW) {
			cds_msg_t cds_msg;
			tSirRoamOffloadScanRsp *scan_offload_rsp;
			scan_offload_rsp =
				cdf_mem_malloc(sizeof(*scan_offload_rsp));
			if (!scan_offload_rsp) {
				WMA_LOGE("%s: Alloc failed for scan_offload_rsp",
					__func__);
				cdf_mem_free(roam_req);
				return CDF_STATUS_E_NOMEM;
			}
			cds_msg.type = eWNI_SME_ROAM_SCAN_OFFLOAD_RSP;
			scan_offload_rsp->sessionId = roam_req->sessionId;
			scan_offload_rsp->reason = roam_req->reason;
			cds_msg.bodyptr = scan_offload_rsp;
			/*
			 * Since REASSOC request is processed in
			 * Roam_Scan_Offload_Rsp post a dummy rsp msg back to
			 * SME with proper reason code.
			 */
			if (CDF_STATUS_SUCCESS !=
			    cds_mq_post_message(CDS_MQ_ID_SME,
						(cds_msg_t *) &cds_msg)) {
				cdf_mem_free(scan_offload_rsp);
				CDF_TRACE(CDF_MODULE_ID_WMA,
					  CDF_TRACE_LEVEL_INFO,
					  "%s: Failed to post Scan Offload Rsp to UMAC",
					  __func__);
			}
		}
		break;

	case ROAM_SCAN_OFFLOAD_ABORT_SCAN:
		/* If roam scan is running, stop that cycle.
		 * It will continue automatically on next trigger.
		 */
		cdf_status = wma_roam_scan_offload_command(wma_handle,
							   WMI_ROAM_SCAN_STOP_CMD,
							   roam_req->sessionId);
		break;

	case ROAM_SCAN_OFFLOAD_RESTART:
		/* Rome offload engine does not stop after any scan.
		 * If this command is sent because all preauth attempts failed
		 * and WMI_ROAM_REASON_SUITABLE_AP event was received earlier,
		 * now it is time to call it heartbeat failure.
		 */
		if ((roam_req->reason == REASON_PREAUTH_FAILED_FOR_ALL)
		    && wma_handle->suitable_ap_hb_failure) {
			WMA_LOGE("%s: Sending heartbeat failure after preauth failures",
				__func__);
			wma_beacon_miss_handler(wma_handle,
						roam_req->sessionId);
			wma_handle->suitable_ap_hb_failure = false;
		}
		break;

	case ROAM_SCAN_OFFLOAD_UPDATE_CFG:
		wma_handle->suitable_ap_hb_failure = false;
		wma_roam_scan_fill_scan_params(wma_handle, pMac, roam_req,
					       &scan_params);
		cdf_status =
			wma_roam_scan_offload_mode(wma_handle, &scan_params,
						   roam_req,
						   WMI_ROAM_SCAN_MODE_NONE,
						   roam_req->sessionId);
		if (cdf_status != CDF_STATUS_SUCCESS)
			break;

		if (roam_req->RoamScanOffloadEnabled == false)
			break;

		cdf_status = wma_roam_scan_bmiss_cnt(wma_handle,
						     roam_req->RoamBmissFirstBcnt,
						     roam_req->RoamBmissFinalBcnt,
						     roam_req->sessionId);
		if (cdf_status != CDF_STATUS_SUCCESS)
			break;
		cdf_status = wma_roam_scan_filter(wma_handle, roam_req);
		if (cdf_status != CDF_STATUS_SUCCESS) {
			WMA_LOGE("Sending update for roam scan filter failed");
			break;
		}


		/*
		 * Runtime (after association) changes to rssi thresholds and
		 * other parameters.
		 */
		cdf_status = wma_roam_scan_offload_chan_list(wma_handle,
							     roam_req->ConnectedNetwork.ChannelCount,
							     &roam_req->ConnectedNetwork.ChannelCache[0],
							     roam_req->ChannelCacheType,
							     roam_req->sessionId);
		/*
		 * Even though the channel list is empty, we can
		 * still go ahead and start Roaming.
		 */
		if ((cdf_status != CDF_STATUS_SUCCESS) &&
			(cdf_status != CDF_STATUS_E_EMPTY))
			break;


		cdf_status = wma_roam_scan_offload_rssi_thresh(wma_handle,
							       roam_req);
		if (cdf_status != CDF_STATUS_SUCCESS)
			break;

		if (roam_req->EmptyRefreshScanPeriod > 0) {
			cdf_status =
				wma_roam_scan_offload_scan_period(wma_handle,
								  roam_req->EmptyRefreshScanPeriod,
								  roam_req->EmptyRefreshScanPeriod * 3,
								  roam_req->sessionId);
			if (cdf_status != CDF_STATUS_SUCCESS)
				break;

			mode = WMI_ROAM_SCAN_MODE_PERIODIC;
			/* Don't use rssi triggered roam scans if external app
			 * is in control of channel list.
			 */
			if (roam_req->ChannelCacheType != CHANNEL_LIST_STATIC)
				mode |= WMI_ROAM_SCAN_MODE_RSSI_CHANGE;

		} else {
			mode = WMI_ROAM_SCAN_MODE_RSSI_CHANGE;
		}

		cdf_status = wma_roam_scan_offload_rssi_change(wma_handle,
				    roam_req->sessionId,
				    roam_req->RoamRescanRssiDiff,
				    roam_req->RoamBeaconRssiWeight,
				    roam_req->hi_rssi_scan_delay);
		if (cdf_status != CDF_STATUS_SUCCESS)
			break;

		wma_roam_scan_fill_ap_profile(wma_handle, pMac, roam_req,
					      &ap_profile);
		cdf_status =
			wma_roam_scan_offload_ap_profile(wma_handle, &ap_profile,
							 roam_req->sessionId);
		if (cdf_status != CDF_STATUS_SUCCESS)
			break;

		wma_roam_scan_fill_scan_params(wma_handle, pMac, roam_req,
					       &scan_params);
		cdf_status =
			wma_roam_scan_offload_mode(wma_handle, &scan_params,
						   roam_req, mode,
						   roam_req->sessionId);

		break;

	default:
		break;
	}
	cdf_mem_free(roam_req);
	return cdf_status;
}

/**
 * wma_roam_preauth_chan_set() - set preauth channel
 * @wma_handle: wma handle
 * @params: switch channel params
 * @vdev_id: vdev id
 *
 * Send a single channel passive scan request
 * to handle set_channel operation for preauth
 *
 * Return: CDF atatus
 */
CDF_STATUS wma_roam_preauth_chan_set(tp_wma_handle wma_handle,
				     tpSwitchChannelParams params,
				     uint8_t vdev_id)
{
	CDF_STATUS cdf_status = CDF_STATUS_SUCCESS;
	tSirScanOffloadReq scan_req;
	uint8_t bssid[IEEE80211_ADDR_LEN] = {
					0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

	WMA_LOGI("%s: channel %d", __func__, params->channelNumber);

	/* Check for prior operation in progress */
	if (wma_handle->roam_preauth_chan_context != NULL) {
		cdf_status = CDF_STATUS_E_FAILURE;
		WMA_LOGE("%s: Rejected request. Previous operation in progress",
			 __func__);
		goto send_resp;
	}
	wma_handle->roam_preauth_chan_context = params;

	/* Prepare a dummy scan request and get the
	 * wmi_start_scan_cmd_fixed_param structure filled properly
	 */
	cdf_mem_zero(&scan_req, sizeof(scan_req));
	cdf_copy_macaddr((struct cdf_mac_addr *) &scan_req.bssId,
			 (struct cdf_mac_addr *) bssid);
	cdf_copy_macaddr((struct cdf_mac_addr *) &scan_req.selfMacAddr,
			 (struct cdf_mac_addr *) &params->selfStaMacAddr);
	scan_req.channelList.numChannels = 1;
	scan_req.channelList.channelNumber[0] = params->channelNumber;
	scan_req.numSsid = 0;
	scan_req.minChannelTime = WMA_ROAM_PREAUTH_SCAN_TIME;
	scan_req.maxChannelTime = WMA_ROAM_PREAUTH_SCAN_TIME;
	scan_req.scanType = eSIR_PASSIVE_SCAN;
	scan_req.p2pScanType = P2P_SCAN_TYPE_LISTEN;
	scan_req.sessionId = vdev_id;
	wma_get_scan_id(&scan_req.scan_id);
	wma_handle->roam_preauth_chanfreq =
		cds_chan_to_freq(params->channelNumber);

	/* set the state in advance before calling wma_start_scan and be ready
	 * to handle scan events from firmware. Otherwise print statments
	 * in wma_start_can create a race condition.
	 */
	wma_handle->roam_preauth_scan_state = WMA_ROAM_PREAUTH_CHAN_REQUESTED;
	cdf_status = wma_start_scan(wma_handle, &scan_req, WMA_CHNL_SWITCH_REQ);

	if (cdf_status == CDF_STATUS_SUCCESS)
		return cdf_status;
	wma_handle->roam_preauth_scan_state = WMA_ROAM_PREAUTH_CHAN_NONE;
	/* Failed operation. Safely clear context */
	wma_handle->roam_preauth_chan_context = NULL;

send_resp:
	WMA_LOGI("%s: sending WMA_SWITCH_CHANNEL_RSP, status = 0x%x",
		 __func__, cdf_status);
	params->chainMask = wma_handle->pdevconfig.txchainmask;
	params->smpsMode = SMPS_MODE_DISABLED;
	params->status = cdf_status;
	wma_send_msg(wma_handle, WMA_SWITCH_CHANNEL_RSP, (void *)params, 0);
	return cdf_status;
}

/**
 * wma_roam_preauth_chan_cancel() - cancel preauth scan
 * @wma_handle: wma handle
 * @params: switch channel parameters
 * @vdev_id: vdev id
 *
 * Return: CDF status
 */
CDF_STATUS wma_roam_preauth_chan_cancel(tp_wma_handle wma_handle,
					tpSwitchChannelParams params,
					uint8_t vdev_id)
{
	tAbortScanParams abort_scan_req;
	CDF_STATUS cdf_status = CDF_STATUS_SUCCESS;

	WMA_LOGI("%s: channel %d", __func__, params->channelNumber);
	/* Check for prior operation in progress */
	if (wma_handle->roam_preauth_chan_context != NULL) {
		cdf_status = CDF_STATUS_E_FAILURE;
		WMA_LOGE("%s: Rejected request. Previous operation in progress",
			 __func__);
		goto send_resp;
	}
	wma_handle->roam_preauth_chan_context = params;

	abort_scan_req.SessionId = vdev_id;
	wma_handle->roam_preauth_scan_state =
		WMA_ROAM_PREAUTH_CHAN_CANCEL_REQUESTED;
	cdf_status = wma_stop_scan(wma_handle, &abort_scan_req);
	if (cdf_status == CDF_STATUS_SUCCESS)
		return cdf_status;
	/* Failed operation. Safely clear context */
	wma_handle->roam_preauth_chan_context = NULL;

send_resp:
	WMA_LOGI("%s: sending WMA_SWITCH_CHANNEL_RSP, status = 0x%x",
		 __func__, cdf_status);
	params->chainMask = wma_handle->pdevconfig.txchainmask;
	params->smpsMode = SMPS_MODE_DISABLED;
	params->status = cdf_status;
	wma_send_msg(wma_handle, WMA_SWITCH_CHANNEL_RSP, (void *)params, 0);
	return cdf_status;
}

/**
 * wma_roam_preauth_scan_event_handler() - preauth scan event handler
 * @wma_handle: wma handle
 * @vdev_id: vdev id
 * @wmi_event: event data
 *
 * This function handles preauth scan event and send appropriate
 * message to upper layers.
 *
 * Return: none
 */
void wma_roam_preauth_scan_event_handler(tp_wma_handle wma_handle,
					 uint8_t vdev_id,
					 wmi_scan_event_fixed_param *
					 wmi_event)
{
	CDF_STATUS cdf_status = CDF_STATUS_SUCCESS;
	tSwitchChannelParams *params;

	WMA_LOGI("%s: preauth_scan_state %d, event 0x%x, reason 0x%x",
		 __func__, wma_handle->roam_preauth_scan_state,
		 wmi_event->event, wmi_event->reason);
	switch (wma_handle->roam_preauth_scan_state) {
	case WMA_ROAM_PREAUTH_CHAN_REQUESTED:
		if (wmi_event->event & WMI_SCAN_EVENT_FOREIGN_CHANNEL) {
			/* complete set_chan request */
			wma_handle->roam_preauth_scan_state =
				WMA_ROAM_PREAUTH_ON_CHAN;
			cdf_status = CDF_STATUS_SUCCESS;
		} else if (wmi_event->event & WMI_SCAN_FINISH_EVENTS) {
			/* Failed to get preauth channel or finished (unlikely) */
			wma_handle->roam_preauth_scan_state =
				WMA_ROAM_PREAUTH_CHAN_NONE;
			cdf_status = CDF_STATUS_E_FAILURE;
		} else
			return;
		break;
	case WMA_ROAM_PREAUTH_CHAN_CANCEL_REQUESTED:
		/* Completed or cancelled, complete set_chan cancel request */
		wma_handle->roam_preauth_scan_state =
			WMA_ROAM_PREAUTH_CHAN_NONE;
		break;

	case WMA_ROAM_PREAUTH_ON_CHAN:
		if ((wmi_event->event & WMI_SCAN_EVENT_BSS_CHANNEL) ||
		    (wmi_event->event & WMI_SCAN_FINISH_EVENTS))
			wma_handle->roam_preauth_scan_state =
				WMA_ROAM_PREAUTH_CHAN_COMPLETED;

		/* There is no WMA request to complete. Next set channel request will
		 * look at this state and complete it.
		 */
		break;
	default:
		WMA_LOGE("%s: unhandled event 0x%x, reason 0x%x",
			 __func__, wmi_event->event, wmi_event->reason);
		return;
	}

	params = (tpSwitchChannelParams) wma_handle->roam_preauth_chan_context;
	if (params) {
		WMA_LOGI("%s: sending WMA_SWITCH_CHANNEL_RSP, status = 0x%x",
			 __func__, cdf_status);
		params->chainMask = wma_handle->pdevconfig.txchainmask;
		params->smpsMode = SMPS_MODE_DISABLED;
		params->status = cdf_status;
		wma_send_msg(wma_handle, WMA_SWITCH_CHANNEL_RSP, (void *)params,
			     0);
		wma_handle->roam_preauth_chan_context = NULL;
	}

}

#ifdef WLAN_FEATURE_ROAM_OFFLOAD

/**
 * wma_process_roam_invoke() - send roam invoke command to fw.
 * @handle: wma handle
 * @roaminvoke: roam invoke command
 *
 * Send roam invoke command to fw for fastreassoc.
 *
 * Return: none
 */
void wma_process_roam_invoke(WMA_HANDLE handle,
		struct wma_roam_invoke_cmd *roaminvoke)
{
	tp_wma_handle wma_handle = (tp_wma_handle) handle;
	wmi_roam_invoke_cmd_fixed_param *cmd;
	wmi_buf_t wmi_buf;
	u_int8_t *buf_ptr;
	u_int16_t len, args_tlv_len;
	A_UINT32 *channel_list;
	wmi_mac_addr *bssid_list;

	if (!wma_handle || !wma_handle->wmi_handle) {
		WMA_LOGE("%s: WMA is closed, can not send roam invoke",
				__func__);
		return;
	}
	/* Host sends only one channel and one bssid */
	args_tlv_len = 2 * WMI_TLV_HDR_SIZE + sizeof(A_UINT32) +
			sizeof(wmi_mac_addr);
	len = sizeof(wmi_roam_invoke_cmd_fixed_param) + args_tlv_len;
	wmi_buf = wmi_buf_alloc(wma_handle->wmi_handle, len);
	if (!wmi_buf) {
		WMA_LOGE("%s: wmai_buf_alloc failed", __func__);
		return;
	}

	cmd = (wmi_roam_invoke_cmd_fixed_param *)wmi_buf_data(wmi_buf);
	buf_ptr = (u_int8_t *) cmd;
	WMITLV_SET_HDR(&cmd->tlv_header,
	WMITLV_TAG_STRUC_wmi_roam_invoke_cmd_fixed_param,
	WMITLV_GET_STRUCT_TLVLEN(wmi_roam_invoke_cmd_fixed_param));
	cmd->vdev_id = roaminvoke->vdev_id;
	cmd->flags = 0;
	cmd->roam_scan_mode = 0;
	cmd->roam_ap_sel_mode = 0;
	cmd->roam_delay = 0;
	cmd->num_chan = 1;
	cmd->num_bssid = 1;
	buf_ptr += sizeof(wmi_roam_invoke_cmd_fixed_param);
	WMITLV_SET_HDR(buf_ptr, WMITLV_TAG_ARRAY_UINT32,
				(sizeof(u_int32_t)));
	channel_list = (A_UINT32 *)(buf_ptr + WMI_TLV_HDR_SIZE);
	*channel_list = (A_UINT32)cds_chan_to_freq(roaminvoke->channel);
	buf_ptr += sizeof(A_UINT32) + WMI_TLV_HDR_SIZE;
	WMITLV_SET_HDR(buf_ptr, WMITLV_TAG_ARRAY_FIXED_STRUC,
				(sizeof(wmi_mac_addr)));
	bssid_list = (wmi_mac_addr *)(buf_ptr + WMI_TLV_HDR_SIZE);
	WMI_CHAR_ARRAY_TO_MAC_ADDR(roaminvoke->bssid, bssid_list);
	if (wmi_unified_cmd_send(wma_handle->wmi_handle, wmi_buf, len,
					WMI_ROAM_INVOKE_CMDID)) {
		WMA_LOGP("%s: failed to send roam invoke command", __func__);
		wmi_buf_free(wmi_buf);
		return;
	}
	return;
}

/**
 * wma_process_roam_synch_fail() -roam synch failure handle
 * @handle: wma handle
 * @synch_fail: roam synch fail parameters
 *
 * Return: none
 */
void wma_process_roam_synch_fail(WMA_HANDLE handle,
				 struct roam_offload_synch_fail *synch_fail)
{
	tp_wma_handle wma_handle = (tp_wma_handle) handle;
	if (!wma_handle || !wma_handle->wmi_handle) {
		WMA_LOGE("%s: WMA is closed, can not clean-up roam synch",
			__func__);
		return;
	}
	/* Hand Off Failure could happen as an exception, when a roam synch
	 * indication is posted to Host, but a roam synch complete is not
	 * posted to the firmware.So, clear the roam synch in progress
	 * flag before disconnecting the session through this event.*/
	wma_handle->interfaces[synch_fail->session_id].roam_synch_in_progress =
		false;
}

/**
 * wma_roam_synch_event_handler() - roam synch event handler
 * @handle: wma handle
 * @event: event data
 * @len: length of data
 *
 * This function is roam synch event handler. It sends roam
 * indication for upper layer.
 */
int wma_roam_synch_event_handler(void *handle, uint8_t *event,
					uint32_t len)
{
	WMI_ROAM_SYNCH_EVENTID_param_tlvs *param_buf = NULL;
	wmi_roam_synch_event_fixed_param *synch_event = NULL;
	uint8_t *bcn_probersp_ptr = NULL;
	uint8_t *reassoc_rsp_ptr = NULL;
	tp_wma_handle wma = (tp_wma_handle) handle;
	wmi_channel *chan = NULL;
	wmi_key_material *key = NULL;
	int size = 0;
	roam_offload_synch_ind *roam_synch_ind_ptr;

	WMA_LOGD("LFR3:%s", __func__);
	if (!event) {
		WMA_LOGE("%s: event param null", __func__);
		return -EINVAL;
	}

	param_buf = (WMI_ROAM_SYNCH_EVENTID_param_tlvs *) event;
	if (!param_buf) {
		WMA_LOGE("%s: received null buf from target", __func__);
		return -EINVAL;
	}

	synch_event = param_buf->fixed_param;
	if (!synch_event) {
		WMA_LOGE("%s: received null event data from target", __func__);
		return -EINVAL;
	}

	if (wma->interfaces[synch_event->vdev_id].roam_synch_in_progress) {
		WMA_LOGE("%s: Ignoring RSI since one is already in progress",
				__func__);
		return -EINVAL;
	}
	wma->interfaces[synch_event->vdev_id].roam_synch_in_progress = true;
	len = sizeof(roam_offload_synch_ind) +
	      synch_event->bcn_probe_rsp_len + synch_event->reassoc_rsp_len;
	roam_synch_ind_ptr =
		(roam_offload_synch_ind *) cdf_mem_malloc(len);
	if (!roam_synch_ind_ptr) {
		WMA_LOGE("%s: failed to allocate memory for roam_synch_event",
			 __func__);
		return -ENOMEM;
	}

	/* abort existing scan if any */
	if (wma->interfaces[synch_event->vdev_id].scan_info.scan_id != 0) {
		tAbortScanParams abort_scan;
		WMA_LOGD("LFR3: Aborting Scan with scan_id=%d\n",
		wma->interfaces[synch_event->vdev_id].scan_info.scan_id);
		abort_scan.SessionId = synch_event->vdev_id;
		wma_stop_scan(wma, &abort_scan);
	}

	roam_synch_ind_ptr->messageType = eWNI_SME_ROAM_OFFLOAD_SYNCH_IND;
	roam_synch_ind_ptr->length = size;
	roam_synch_ind_ptr->roamedVdevId = synch_event->vdev_id;
	roam_synch_ind_ptr->authStatus = synch_event->auth_status;
	roam_synch_ind_ptr->roamReason = synch_event->roam_reason;
	roam_synch_ind_ptr->rssi = synch_event->rssi;
	roam_synch_ind_ptr->isBeacon = synch_event->is_beacon;
	WMI_MAC_ADDR_TO_CHAR_ARRAY(&synch_event->bssid,
				   roam_synch_ind_ptr->bssId);
	roam_synch_ind_ptr->beaconProbeRespOffset =
		sizeof(roam_offload_synch_ind);
	bcn_probersp_ptr =
		(uint8_t *) roam_synch_ind_ptr +
		roam_synch_ind_ptr->beaconProbeRespOffset;
	roam_synch_ind_ptr->beaconProbeRespLength =
		synch_event->bcn_probe_rsp_len;
	cdf_mem_copy(bcn_probersp_ptr, param_buf->bcn_probe_rsp_frame,
		     roam_synch_ind_ptr->beaconProbeRespLength);
	roam_synch_ind_ptr->reassocRespOffset =
		sizeof(roam_offload_synch_ind) +
		roam_synch_ind_ptr->beaconProbeRespLength;
	roam_synch_ind_ptr->reassocRespLength = synch_event->reassoc_rsp_len;
	reassoc_rsp_ptr = (uint8_t *) roam_synch_ind_ptr +
			  roam_synch_ind_ptr->reassocRespOffset;
	cdf_mem_copy(reassoc_rsp_ptr,
		     param_buf->reassoc_rsp_frame,
		     roam_synch_ind_ptr->reassocRespLength);
	chan = (wmi_channel *) param_buf->chan;
	roam_synch_ind_ptr->chan_freq = chan->mhz;
	key = (wmi_key_material *) param_buf->key;
	if (key != NULL) {
		CDF_TRACE_HEX_DUMP(CDF_MODULE_ID_WMA, CDF_TRACE_LEVEL_DEBUG,
				   key->replay_counter, SIR_REPLAY_CTR_LEN);
		cdf_mem_copy(roam_synch_ind_ptr->kck, key->kck,
			     SIR_KCK_KEY_LEN);
		cdf_mem_copy(roam_synch_ind_ptr->kek, key->kek,
			     SIR_KEK_KEY_LEN);
		cdf_mem_copy(roam_synch_ind_ptr->replay_ctr,
			     key->replay_counter, SIR_REPLAY_CTR_LEN);
	}
	wma_send_msg(wma, WMA_ROAM_OFFLOAD_SYNCH_IND,
			(void *) roam_synch_ind_ptr, 0);
	return 0;
}

/**
 * wma_rssi_breached_event_handler() - rssi breached event handler
 * @handle: wma handle
 * @cmd_param_info: event handler data
 * @len: length of @cmd_param_info
 *
 * Return: 0 on success; error number otherwise
 */
int wma_rssi_breached_event_handler(void *handle,
				u_int8_t  *cmd_param_info, u_int32_t len)
{
	WMI_RSSI_BREACH_EVENTID_param_tlvs *param_buf;
	wmi_rssi_breach_event_fixed_param  *event;
	struct rssi_breach_event  rssi;
	tpAniSirGlobal mac = cds_get_context(CDF_MODULE_ID_PE);

	if (!mac) {
		WMA_LOGE("%s: Invalid mac context", __func__);
		return -EINVAL;
	}
	if (!mac->sme.rssi_threshold_breached_cb) {
		WMA_LOGE("%s: Callback not registered", __func__);
		return -EINVAL;
	}
	param_buf = (WMI_RSSI_BREACH_EVENTID_param_tlvs *)cmd_param_info;
	if (!param_buf) {
		WMA_LOGE("%s: Invalid rssi breached event", __func__);
		return -EINVAL;
	}
	event = param_buf->fixed_param;

	rssi.request_id = event->request_id;
	rssi.session_id = event->vdev_id;
	rssi.curr_rssi = event->rssi + WMA_TGT_NOISE_FLOOR_DBM;
	WMI_MAC_ADDR_TO_CHAR_ARRAY(&event->bssid, rssi.curr_bssid.bytes);

	WMA_LOGD("%s: req_id: %u vdev_id: %d curr_rssi: %d", __func__,
		rssi.request_id, rssi.session_id, rssi.curr_rssi);
	WMA_LOGI("%s: curr_bssid: %pM", __func__, rssi.curr_bssid.bytes);

	mac->sme.rssi_threshold_breached_cb(mac->hHdd, &rssi);
	WMA_LOGD("%s: Invoke HDD rssi breached callback", __func__);
	return 0;
}

/**
 * wma_roam_scan_fill_self_caps() - fill capabilities
 * @wma_handle: wma handle
 * @roam_offload_params: offload parameters
 * @roam_req: roam request
 *
 * This function fills roam self capablities.
 *
 * Return: CDF status
 */
CDF_STATUS wma_roam_scan_fill_self_caps(tp_wma_handle wma_handle,
					wmi_roam_offload_tlv_param *
					roam_offload_params,
					tSirRoamOffloadScanReq *roam_req)
{
	struct sAniSirGlobal *pMac = NULL;
	tSirMacCapabilityInfo selfCaps;
	uint32_t val = 0;
	uint32_t nCfgValue;
	uint16_t *pCfgValue16;
	uint8_t nCfgValue8, *pCfgValue8;
	tSirMacQosInfoStation macQosInfoSta;
	union {
		uint16_t nCfgValue16;
		tSirMacHTCapabilityInfo htCapInfo;
		tSirMacExtendedHTCapabilityInfo extHtCapInfo;
	} uHTCapabilityInfo;

	cdf_mem_set(&macQosInfoSta, sizeof(tSirMacQosInfoStation), 0);
	/* Roaming is done only for INFRA STA type.
	 * So, ess will be one and ibss will be Zero */
	pMac = cds_get_context(CDF_MODULE_ID_PE);
	if (!pMac) {
		WMA_LOGE("%s:NULL pMac ptr. Exiting", __func__);
		CDF_ASSERT(0);
		return CDF_STATUS_E_FAILURE;
	}

	if (wlan_cfg_get_int(pMac, WNI_CFG_PRIVACY_ENABLED, &val) != eSIR_SUCCESS) {
		CDF_TRACE(CDF_MODULE_ID_WMA, CDF_TRACE_LEVEL_ERROR,
			  "Failed to get WNI_CFG_PRIVACY_ENABLED");
		return CDF_STATUS_E_FAILURE;
	}
	selfCaps.ess = 1;
	selfCaps.ibss = 0;
	if (val)
		selfCaps.privacy = 1;
	if (wlan_cfg_get_int(pMac, WNI_CFG_SHORT_PREAMBLE, &val) != eSIR_SUCCESS) {
		CDF_TRACE(CDF_MODULE_ID_WMA, CDF_TRACE_LEVEL_ERROR,
			  "Failed to get WNI_CFG_SHORT_PREAMBLE");
		return CDF_STATUS_E_FAILURE;
	}
	if (val)
		selfCaps.shortPreamble = 1;

	selfCaps.pbcc = 0;
	selfCaps.channelAgility = 0;
	if (wlan_cfg_get_int(pMac, WNI_CFG_11G_SHORT_SLOT_TIME_ENABLED,
			     &val) != eSIR_SUCCESS) {
		CDF_TRACE(CDF_MODULE_ID_WMA, CDF_TRACE_LEVEL_ERROR,
			  "Failed to get WNI_CFG_11G_SHORT_SLOT_TIME_ENABLED");
		return CDF_STATUS_E_FAILURE;
	}
	if (val)
		selfCaps.shortSlotTime = 1;
	if (wlan_cfg_get_int(pMac, WNI_CFG_11H_ENABLED, &val) != eSIR_SUCCESS) {
		CDF_TRACE(CDF_MODULE_ID_WMA, CDF_TRACE_LEVEL_ERROR,
			  "Failed to get WNI_CFG_11H_ENABLED");
		return CDF_STATUS_E_FAILURE;
	}
	if (val)
		selfCaps.spectrumMgt = 1;
	if (wlan_cfg_get_int(pMac, WNI_CFG_QOS_ENABLED, &val) != eSIR_SUCCESS) {
		CDF_TRACE(CDF_MODULE_ID_WMA, CDF_TRACE_LEVEL_ERROR,
			  "Failed to get WNI_CFG_QOS_ENABLED");
		return CDF_STATUS_E_FAILURE;
	}
	if (val)
		selfCaps.qos = 1;
	if (wlan_cfg_get_int(pMac, WNI_CFG_APSD_ENABLED, &val) != eSIR_SUCCESS) {
		CDF_TRACE(CDF_MODULE_ID_WMA, CDF_TRACE_LEVEL_ERROR,
			  "Failed to get WNI_CFG_APSD_ENABLED");
		return CDF_STATUS_E_FAILURE;
	}
	if (val)
		selfCaps.apsd = 1;

	selfCaps.rrm = pMac->rrm.rrmSmeContext.rrmConfig.rrm_enabled;

	if (wlan_cfg_get_int(pMac, WNI_CFG_BLOCK_ACK_ENABLED, &val) !=
	    eSIR_SUCCESS) {
		CDF_TRACE(CDF_MODULE_ID_WMA, CDF_TRACE_LEVEL_ERROR,
			  "Failed to get WNI_CFG_BLOCK_ACK_ENABLED");
		return CDF_STATUS_E_FAILURE;
	}
	selfCaps.delayedBA =
		(uint16_t) ((val >> WNI_CFG_BLOCK_ACK_ENABLED_DELAYED) & 1);
	selfCaps.immediateBA =
		(uint16_t) ((val >> WNI_CFG_BLOCK_ACK_ENABLED_IMMEDIATE) & 1);
	pCfgValue16 = (uint16_t *) &selfCaps;
	roam_offload_params->capability = (*pCfgValue16) & 0xFFFF;

	if (wlan_cfg_get_int(pMac, WNI_CFG_HT_CAP_INFO, &nCfgValue) !=
	    eSIR_SUCCESS) {
		CDF_TRACE(CDF_MODULE_ID_WMA, CDF_TRACE_LEVEL_ERROR,
			  "Failed to get WNI_CFG_HT_CAP_INFO");
		return CDF_STATUS_E_FAILURE;
	}
	uHTCapabilityInfo.nCfgValue16 = nCfgValue & 0xFFFF;
	roam_offload_params->ht_caps_info =
		uHTCapabilityInfo.nCfgValue16 & 0xFFFF;
	if (wlan_cfg_get_int(pMac, WNI_CFG_HT_AMPDU_PARAMS, &nCfgValue) !=
	    eSIR_SUCCESS) {
		CDF_TRACE(CDF_MODULE_ID_WMA, CDF_TRACE_LEVEL_ERROR,
			  "Failed to get WNI_CFG_HT_AMPDU_PARAMS");
		return CDF_STATUS_E_FAILURE;
	}
	/* tSirMacHTParametersInfo */
	nCfgValue8 = (uint8_t) nCfgValue;
	roam_offload_params->ampdu_param = (nCfgValue8) & 0xFF;

	val = ROAM_OFFLOAD_NUM_MCS_SET;
	if (wlan_cfg_get_str(pMac, WNI_CFG_SUPPORTED_MCS_SET,
			     (uint8_t *) roam_offload_params->mcsset,
			     &val) != eSIR_SUCCESS) {
		CDF_TRACE(CDF_MODULE_ID_WMA, CDF_TRACE_LEVEL_ERROR,
			  "Failed to get WNI_CFG_SUPPORTED_MCS_SET");
		return CDF_STATUS_E_FAILURE;
	}
	if (wlan_cfg_get_int(pMac, WNI_CFG_EXT_HT_CAP_INFO, &nCfgValue) !=
	    eSIR_SUCCESS) {
		CDF_TRACE(CDF_MODULE_ID_WMA, CDF_TRACE_LEVEL_ERROR,
			  "Failed to get WNI_CFG_EXT_HT_CAP_INFO");
		return CDF_STATUS_E_FAILURE;
	}
	/* uHTCapabilityInfo.extHtCapInfo */
	uHTCapabilityInfo.nCfgValue16 = nCfgValue & 0xFFFF;
	roam_offload_params->ht_ext_cap =
		uHTCapabilityInfo.nCfgValue16 & 0xFFFF;

	if (wlan_cfg_get_int(pMac, WNI_CFG_TX_BF_CAP, &nCfgValue) != eSIR_SUCCESS) {
		CDF_TRACE(CDF_MODULE_ID_WMA, CDF_TRACE_LEVEL_ERROR,
			  "Failed to get WNI_CFG_TX_BF_CAP");
		return CDF_STATUS_E_FAILURE;
	}
	/* tSirMacTxBFCapabilityInfo */
	nCfgValue8 = (uint8_t) nCfgValue;
	roam_offload_params->ht_txbf = nCfgValue8 & 0xFF;
	if (wlan_cfg_get_int(pMac, WNI_CFG_AS_CAP, &nCfgValue) != eSIR_SUCCESS) {
		CDF_TRACE(CDF_MODULE_ID_WMA, CDF_TRACE_LEVEL_ERROR,
			  "Failed to get WNI_CFG_AS_CAP");
		return CDF_STATUS_E_FAILURE;
	}
	/* tSirMacASCapabilityInfo */
	nCfgValue8 = (uint8_t) nCfgValue;
	roam_offload_params->asel_cap = nCfgValue8 & 0xFF;

	/* QOS Info */
	if (wlan_cfg_get_int(pMac, WNI_CFG_MAX_SP_LENGTH, &nCfgValue) !=
	    eSIR_SUCCESS) {
		CDF_TRACE(CDF_MODULE_ID_WMA, CDF_TRACE_LEVEL_ERROR,
			  "Failed to get WNI_CFG_MAX_SP_LENGTH");
		return CDF_STATUS_E_FAILURE;
	}
	nCfgValue8 = (uint8_t) nCfgValue;
	macQosInfoSta.maxSpLen = nCfgValue8;
	macQosInfoSta.moreDataAck = 0;
	macQosInfoSta.qack = 0;
	macQosInfoSta.acbe_uapsd = roam_req->AcUapsd.acbe_uapsd;
	macQosInfoSta.acbk_uapsd = roam_req->AcUapsd.acbk_uapsd;
	macQosInfoSta.acvi_uapsd = roam_req->AcUapsd.acvi_uapsd;
	macQosInfoSta.acvo_uapsd = roam_req->AcUapsd.acvo_uapsd;
	pCfgValue8 = (uint8_t *) &macQosInfoSta;
	/* macQosInfoSta Only queue_request is set.Refer to
	 * populate_dot11f_wmm_caps for more details
	 */
	roam_offload_params->qos_caps = (*pCfgValue8) & 0xFF;
	roam_offload_params->wmm_caps = 0x4 & 0xFF;
	return CDF_STATUS_SUCCESS;
}

/**
 * wma_set_ric_req() - set ric request element
 * @wma: wma handle
 * @msg: message
 * @is_add_ts: is addts required
 *
 * This function sets ric request element for 11r roaming.
 *
 * Return: none
 */
void wma_set_ric_req(tp_wma_handle wma, void *msg, uint8_t is_add_ts)
{
	wmi_ric_request_fixed_param *cmd;
	wmi_ric_tspec *tspec_param;
	wmi_buf_t buf;
	uint8_t *buf_ptr;
	tSirMacTspecIE *ptspecIE;
	int32_t len = sizeof(wmi_ric_request_fixed_param) +
		      WMI_TLV_HDR_SIZE + sizeof(wmi_ric_tspec);

	buf = wmi_buf_alloc(wma->wmi_handle, len);
	if (!buf) {
		WMA_LOGP("%s: wmi_buf_alloc failed", __func__);
		return;
	}

	buf_ptr = (uint8_t *) wmi_buf_data(buf);

	cmd = (wmi_ric_request_fixed_param *) buf_ptr;
	WMITLV_SET_HDR(&cmd->tlv_header,
		       WMITLV_TAG_STRUC_wmi_ric_request_fixed_param,
		       WMITLV_GET_STRUCT_TLVLEN(wmi_ric_request_fixed_param));
	if (is_add_ts)
		cmd->vdev_id = ((tAddTsParams *) msg)->sessionId;
	else
		cmd->vdev_id = ((tDelTsParams *) msg)->sessionId;
	cmd->num_ric_request = 1;       /* Today we are sending only 1 ric at once */
	cmd->is_add_ric = is_add_ts;

	buf_ptr += sizeof(wmi_ric_request_fixed_param);
	WMITLV_SET_HDR(buf_ptr, WMITLV_TAG_ARRAY_STRUC, sizeof(wmi_ric_tspec));

	buf_ptr += WMI_TLV_HDR_SIZE;
	tspec_param = (wmi_ric_tspec *) buf_ptr;
	WMITLV_SET_HDR(&tspec_param->tlv_header,
		       WMITLV_TAG_STRUC_wmi_ric_tspec,
		       WMITLV_GET_STRUCT_TLVLEN(wmi_ric_tspec));

	if (is_add_ts)
		ptspecIE = &(((tAddTsParams *) msg)->tspec);
	else
		ptspecIE = &(((tDelTsParams *) msg)->delTsInfo.tspec);

	/* Fill the tsinfo in the format expected by firmware */
#ifndef ANI_LITTLE_BIT_ENDIAN
	cdf_mem_copy(((uint8_t *) &tspec_param->ts_info) + 1,
		     ((uint8_t *) &ptspecIE->tsinfo) + 1, 2);
#else
	cdf_mem_copy(((uint8_t *) &tspec_param->ts_info),
		     ((uint8_t *) &ptspecIE->tsinfo) + 1, 2);
#endif /* ANI_LITTLE_BIT_ENDIAN */

	tspec_param->nominal_msdu_size = ptspecIE->nomMsduSz;
	tspec_param->maximum_msdu_size = ptspecIE->maxMsduSz;
	tspec_param->min_service_interval = ptspecIE->minSvcInterval;
	tspec_param->max_service_interval = ptspecIE->maxSvcInterval;
	tspec_param->inactivity_interval = ptspecIE->inactInterval;
	tspec_param->suspension_interval = ptspecIE->suspendInterval;
	tspec_param->svc_start_time = ptspecIE->svcStartTime;
	tspec_param->min_data_rate = ptspecIE->minDataRate;
	tspec_param->mean_data_rate = ptspecIE->meanDataRate;
	tspec_param->peak_data_rate = ptspecIE->peakDataRate;
	tspec_param->max_burst_size = ptspecIE->maxBurstSz;
	tspec_param->delay_bound = ptspecIE->delayBound;
	tspec_param->min_phy_rate = ptspecIE->minPhyRate;
	tspec_param->surplus_bw_allowance = ptspecIE->surplusBw;
	tspec_param->medium_time = 0;

	WMA_LOGI("%s: Set RIC Req is_add_ts:%d", __func__, is_add_ts);

	if (wmi_unified_cmd_send(wma->wmi_handle, buf, len,
				 WMI_ROAM_SET_RIC_REQUEST_CMDID)) {
		WMA_LOGP("%s: Failed to send vdev Set RIC Req command",
			 __func__);
		if (is_add_ts)
			((tAddTsParams *) msg)->status = CDF_STATUS_E_FAILURE;
		cdf_nbuf_free(buf);
	}
}
#endif /* WLAN_FEATURE_ROAM_OFFLOAD */

/**
 * wma_process_unit_test_cmd() - send unit test command to fw.
 * @handle: wma handle
 * @wma_utest: unit test command
 *
 * This function send unit test command to fw.
 *
 * Return: none
 */
void wma_process_unit_test_cmd(WMA_HANDLE handle,
			       t_wma_unit_test_cmd *wma_utest)
{
	tp_wma_handle wma_handle = (tp_wma_handle) handle;
	wmi_unit_test_cmd_fixed_param *cmd;
	wmi_buf_t wmi_buf;
	uint8_t *buf_ptr;
	int i;
	uint16_t len, args_tlv_len;
	A_UINT32 *unit_test_cmd_args;

	args_tlv_len =
		WMI_TLV_HDR_SIZE + wma_utest->num_args * sizeof(A_UINT32);
	len = sizeof(wmi_unit_test_cmd_fixed_param) + args_tlv_len;
	if (!wma_handle || !wma_handle->wmi_handle) {
		WMA_LOGE("%s: WMA is closed, can not issue fw unit test cmd",
			 __func__);
		return;
	}
	wmi_buf = wmi_buf_alloc(wma_handle->wmi_handle, len);
	if (!wmi_buf) {
		WMA_LOGE("%s: wmai_buf_alloc failed", __func__);
		return;
	}

	cmd = (wmi_unit_test_cmd_fixed_param *) wmi_buf_data(wmi_buf);
	buf_ptr = (uint8_t *) cmd;
	WMITLV_SET_HDR(&cmd->tlv_header,
		       WMITLV_TAG_STRUC_wmi_unit_test_cmd_fixed_param,
		       WMITLV_GET_STRUCT_TLVLEN(wmi_unit_test_cmd_fixed_param));
	cmd->vdev_id = wma_utest->vdev_id;
	cmd->module_id = wma_utest->module_id;
	cmd->num_args = wma_utest->num_args;
	buf_ptr += sizeof(wmi_unit_test_cmd_fixed_param);
	WMITLV_SET_HDR(buf_ptr, WMITLV_TAG_ARRAY_UINT32,
		       (wma_utest->num_args * sizeof(uint32_t)));
	unit_test_cmd_args = (A_UINT32 *) (buf_ptr + WMI_TLV_HDR_SIZE);
	WMA_LOGI("%s: %d num of args = ", __func__, wma_utest->num_args);
	for (i = 0; (i < wma_utest->num_args && i < WMA_MAX_NUM_ARGS); i++) {
		unit_test_cmd_args[i] = wma_utest->args[i];
		WMA_LOGI("%d,", wma_utest->args[i]);
	}
	if (wmi_unified_cmd_send(wma_handle->wmi_handle, wmi_buf, len,
				 WMI_UNIT_TEST_CMDID)) {
		WMA_LOGP("%s: failed to send unit test command", __func__);
		cdf_nbuf_free(wmi_buf);
		return;
	}
	return;
}

#ifdef WLAN_FEATURE_ROAM_OFFLOAD

/**
 * wma_roam_ho_fail_handler() - LFR3.0 roam hand off failed handler
 * @wma: wma handle
 * @vdev_id: vdev id
 *
 * Return: none
 */
static void wma_roam_ho_fail_handler(tp_wma_handle wma, uint32_t vdev_id)
{
	tSirSmeHOFailureInd *ho_failure_ind;
	cds_msg_t sme_msg = { 0 };
	CDF_STATUS cdf_status;

	ho_failure_ind = cdf_mem_malloc(sizeof(tSirSmeHOFailureInd));

	if (NULL == ho_failure_ind) {
		WMA_LOGE("%s: Memory allocation failure", __func__);
		return;
	}
	ho_failure_ind->sessionId = vdev_id;
	sme_msg.type = eWNI_SME_HO_FAIL_IND;
	sme_msg.bodyptr = ho_failure_ind;
	sme_msg.bodyval = 0;

	cdf_status = cds_mq_post_message(CDF_MODULE_ID_SME, &sme_msg);
	if (!CDF_IS_STATUS_SUCCESS(cdf_status)) {
		WMA_LOGE("Fail to post eWNI_SME_HO_FAIL_IND msg to SME");
		cdf_mem_free(ho_failure_ind);
		return;
	}
	return;
}

/**
 * wma_process_roam_synch_complete() - roam synch complete command to fw.
 * @handle: wma handle
 * @synchcnf: offload synch confirmation params
 *
 * This function sends roam synch complete event to fw.
 *
 * Return: none
 */
void wma_process_roam_synch_complete(WMA_HANDLE handle,
				     tSirSmeRoamOffloadSynchCnf *synchcnf)
{
	tp_wma_handle wma_handle = (tp_wma_handle) handle;
	wmi_roam_synch_complete_fixed_param *cmd;
	wmi_buf_t wmi_buf;
	uint8_t *buf_ptr;
	uint16_t len;
	bool roam_synch_in_progress;
	len = sizeof(wmi_roam_synch_complete_fixed_param);

	if (!wma_handle || !wma_handle->wmi_handle) {
		WMA_LOGE("%s: WMA is closed, can not issue roam synch cnf",
			 __func__);
		return;
	}
	roam_synch_in_progress =
		wma_handle->interfaces[synchcnf->sessionId].roam_synch_in_progress;
	if (roam_synch_in_progress == false) {
		WMA_LOGE("%s: Dont send roam synch complete", __func__);
		return;
	} else {
		wma_handle->interfaces[synchcnf->sessionId].roam_synch_in_progress =
			false;
	}
	wmi_buf = wmi_buf_alloc(wma_handle->wmi_handle, len);
	if (!wmi_buf) {
		WMA_LOGE("%s: wmi_buf_alloc failed", __func__);
		return;
	}
	cmd = (wmi_roam_synch_complete_fixed_param *) wmi_buf_data(wmi_buf);
	buf_ptr = (uint8_t *) cmd;
	WMITLV_SET_HDR(&cmd->tlv_header,
		       WMITLV_TAG_STRUC_wmi_roam_synch_complete_fixed_param,
		       WMITLV_GET_STRUCT_TLVLEN
			       (wmi_roam_synch_complete_fixed_param));
	cmd->vdev_id = synchcnf->sessionId;
	if (wmi_unified_cmd_send(wma_handle->wmi_handle, wmi_buf, len,
				 WMI_ROAM_SYNCH_COMPLETE)) {
		WMA_LOGP("%s: failed to send roam synch confirmation",
			 __func__);
		cdf_nbuf_free(wmi_buf);
		return;
	}
	return;
}
#endif /* WLAN_FEATURE_ROAM_OFFLOAD */

/**
 * wma_set_channel() - set channel
 * @wma: wma handle
 * @params: switch channel parameters
 *
 * Return: none
 */
void wma_set_channel(tp_wma_handle wma, tpSwitchChannelParams params)
{
	struct wma_vdev_start_req req;
	struct wma_target_req *msg;
	CDF_STATUS status = CDF_STATUS_SUCCESS;
	uint8_t vdev_id, peer_id;
	ol_txrx_peer_handle peer;
	ol_txrx_pdev_handle pdev;
	struct wma_txrx_node *intr = wma->interfaces;
	struct sir_hw_mode_params hw_mode = {0};

	WMA_LOGD("%s: Enter", __func__);
	if (!wma_find_vdev_by_addr(wma, params->selfStaMacAddr, &vdev_id)) {
		WMA_LOGP("%s: Failed to find vdev id for %pM",
			 __func__, params->selfStaMacAddr);
		status = CDF_STATUS_E_FAILURE;
		goto send_resp;
	}
	pdev = cds_get_context(CDF_MODULE_ID_TXRX);
	if (NULL == pdev) {
		WMA_LOGE("%s: Failed to get pdev", __func__);
		status = CDF_STATUS_E_FAILURE;
		goto send_resp;
	}

	peer = ol_txrx_find_peer_by_addr(pdev, intr[vdev_id].bssid, &peer_id);

	/*
	 * Roam offload feature is currently supported
	 * only in STA mode. Other modes still require
	 * to issue a Vdev Start/Vdev Restart for
	 * channel change.
	 */
	if (((wma->interfaces[vdev_id].type == WMI_VDEV_TYPE_STA) &&
	     (wma->interfaces[vdev_id].sub_type == 0)) &&
	    !wma->interfaces[vdev_id].is_channel_switch) {

		if (peer && (peer->state == ol_txrx_peer_state_conn ||
			     peer->state == ol_txrx_peer_state_auth)) {
			/* Trying to change channel while connected
			 * should not invoke VDEV_START.
			 * Instead, use start scan command in passive
			 * mode to park station on that channel
			 */
			WMA_LOGI("%s: calling set_scan, state 0x%x",
				 __func__, wma->roam_preauth_scan_state);
			if (wma->roam_preauth_scan_state ==
			    WMA_ROAM_PREAUTH_CHAN_NONE) {
				/* Is channel change required?
				 */
				if (cds_chan_to_freq(params->channelNumber) !=
				    wma->interfaces[vdev_id].mhz) {
					status = wma_roam_preauth_chan_set(wma,
									   params,
									   vdev_id);
					/* response will be asynchronous */
					return;
				}
			} else if (wma->roam_preauth_scan_state ==
				   WMA_ROAM_PREAUTH_CHAN_REQUESTED ||
				   wma->roam_preauth_scan_state ==
				   WMA_ROAM_PREAUTH_ON_CHAN) {
				status =
					wma_roam_preauth_chan_cancel(wma, params,
								     vdev_id);
				/* response will be asynchronous */
				return;
			} else if (wma->roam_preauth_scan_state ==
				   WMA_ROAM_PREAUTH_CHAN_COMPLETED) {
				/* Already back on home channel. Complete the request */
				wma->roam_preauth_scan_state =
					WMA_ROAM_PREAUTH_CHAN_NONE;
				status = CDF_STATUS_SUCCESS;
			}
			goto send_resp;
		}
	}
	cdf_mem_zero(&req, sizeof(req));
	req.vdev_id = vdev_id;
	msg = wma_fill_vdev_req(wma, req.vdev_id, WMA_CHNL_SWITCH_REQ,
				WMA_TARGET_REQ_TYPE_VDEV_START, params,
				WMA_VDEV_START_REQUEST_TIMEOUT);
	if (!msg) {
		WMA_LOGP("%s: Failed to fill channel switch request for vdev %d",
			__func__, req.vdev_id);
		status = CDF_STATUS_E_NOMEM;
		goto send_resp;
	}
	req.chan = params->channelNumber;
	req.chan_width = params->ch_width;
	req.vht_capable = params->vhtCapable;
	req.ch_center_freq_seg0 = params->ch_center_freq_seg0;
	req.ch_center_freq_seg1 = params->ch_center_freq_seg1;
	req.dot11_mode = params->dot11_mode;
	status = wma_get_current_hw_mode(&hw_mode);
	if (!CDF_IS_STATUS_SUCCESS(status))
		WMA_LOGE("wma_get_current_hw_mode failed");

	if ((params->nss == 2) && !hw_mode.dbs_cap) {
		req.preferred_rx_streams = 2;
		req.preferred_tx_streams = 2;
	} else {
		req.preferred_rx_streams = 1;
		req.preferred_tx_streams = 1;
	}

#ifdef WLAN_FEATURE_VOWIFI
	req.max_txpow = params->maxTxPower;
#else
	req.max_txpow = params->localPowerConstraint;
#endif /* WLAN_FEATURE_VOWIFI */
	req.beacon_intval = 100;
	req.dtim_period = 1;
	req.is_dfs = params->isDfsChannel;

	/* In case of AP mode, once radar is detected, we need to
	 * issuse VDEV RESTART, so we making is_channel_switch as
	 * true
	 */
	if ((wma_is_vdev_in_ap_mode(wma, req.vdev_id) == true) ||
		(params->restart_on_chan_switch == true))
		wma->interfaces[req.vdev_id].is_channel_switch = true;

	status = wma_vdev_start(wma, &req,
				wma->interfaces[req.vdev_id].is_channel_switch);
	if (status != CDF_STATUS_SUCCESS) {
		wma_remove_vdev_req(wma, req.vdev_id,
				    WMA_TARGET_REQ_TYPE_VDEV_START);
		WMA_LOGP("%s: vdev start failed status = %d", __func__, status);
		goto send_resp;
	}

	if (wma->interfaces[req.vdev_id].is_channel_switch)
		wma->interfaces[req.vdev_id].is_channel_switch = false;
	return;
send_resp:
	WMA_LOGD("%s: channel %d ch_width %d txpower %d status %d", __func__,
		 params->channelNumber, params->ch_width,
#ifdef WLAN_FEATURE_VOWIFI
		 params->maxTxPower,
#else
		 params->localPowerConstraint,
#endif /* WLAN_FEATURE_VOWIFI */
		 status);
	params->status = status;
	WMA_LOGI("%s: sending WMA_SWITCH_CHANNEL_RSP, status = 0x%x",
		 __func__, status);
	wma_send_msg(wma, WMA_SWITCH_CHANNEL_RSP, (void *)params, 0);
}

#ifdef FEATURE_WLAN_SCAN_PNO
/**
 * wma_set_pno_channel_prediction() - Set PNO configuration
 * @buf_ptr:      Buffer passed by upper layers
 * @pno:          Buffer to be sent to the firmware
 *
 * Copy the PNO Channel prediction configuration parameters
 * passed by the upper layers to a WMI format TLV and send it
 * down to the firmware.
 *
 * Return: None
 */
void wma_set_pno_channel_prediction(uint8_t *buf_ptr,
		tpSirPNOScanReq pno)
{
	nlo_channel_prediction_cfg *channel_prediction_cfg =
		(nlo_channel_prediction_cfg *) buf_ptr;
	WMITLV_SET_HDR(&channel_prediction_cfg->tlv_header,
			WMITLV_TAG_ARRAY_BYTE,
			WMITLV_GET_STRUCT_TLVLEN(nlo_channel_prediction_cfg));
	channel_prediction_cfg->enable = pno->pno_channel_prediction;
	channel_prediction_cfg->top_k_num = pno->top_k_num_of_channels;
	channel_prediction_cfg->stationary_threshold = pno->stationary_thresh;
	channel_prediction_cfg->full_scan_period_ms =
		pno->channel_prediction_full_scan;
	buf_ptr += sizeof(nlo_channel_prediction_cfg);
	WMA_LOGD("enable: %d, top_k_num: %d, stat_thresh: %d, full_scan: %d",
			channel_prediction_cfg->enable,
			channel_prediction_cfg->top_k_num,
			channel_prediction_cfg->stationary_threshold,
			channel_prediction_cfg->full_scan_period_ms);
}
/**
 * wma_pno_start() - PNO start request
 * @wma: wma handle
 * @pno: PNO request
 *
 * This function request FW to start PNO request.
 * Request: CDF status
 */
CDF_STATUS wma_pno_start(tp_wma_handle wma, tpSirPNOScanReq pno)
{
	wmi_nlo_config_cmd_fixed_param *cmd;
	nlo_configured_parameters *nlo_list;
	uint32_t *channel_list;
	int32_t len;
	wmi_buf_t buf;
	uint8_t *buf_ptr;
	uint8_t i;
	int ret;

	WMA_LOGD("PNO Start");

	/*
	 * TLV place holder for array nlo_configured_parameters(nlo_list)
	 * TLV place holder for array of uint32_t channel_list
	 * TLV place holder for chnnl prediction cfg
	 */
	len = sizeof(*cmd) +
		WMI_TLV_HDR_SIZE + WMI_TLV_HDR_SIZE + WMI_TLV_HDR_SIZE;

	len += sizeof(uint32_t) * CDF_MIN(pno->aNetworks[0].ucChannelCount,
					  WMI_NLO_MAX_CHAN);
	len += sizeof(nlo_configured_parameters) *
	       CDF_MIN(pno->ucNetworksCount, WMI_NLO_MAX_SSIDS);
	len += sizeof(nlo_channel_prediction_cfg);

	buf = wmi_buf_alloc(wma->wmi_handle, len);
	if (!buf) {
		WMA_LOGE("%s: Failed allocate wmi buffer", __func__);
		return CDF_STATUS_E_NOMEM;
	}

	cmd = (wmi_nlo_config_cmd_fixed_param *) wmi_buf_data(buf);

	buf_ptr = (uint8_t *) cmd;
	WMITLV_SET_HDR(&cmd->tlv_header,
		       WMITLV_TAG_STRUC_wmi_nlo_config_cmd_fixed_param,
		       WMITLV_GET_STRUCT_TLVLEN
			       (wmi_nlo_config_cmd_fixed_param));
	cmd->vdev_id = pno->sessionId;
	cmd->flags = WMI_NLO_CONFIG_START | WMI_NLO_CONFIG_SSID_HIDE_EN;

	/* Current FW does not support min-max range for dwell time */
	cmd->active_dwell_time = pno->active_max_time;
	cmd->passive_dwell_time = pno->passive_max_time;

	/* Copy scan interval */
	cmd->fast_scan_period = pno->fast_scan_period;
	cmd->slow_scan_period = pno->slow_scan_period;
	cmd->fast_scan_max_cycles = pno->fast_scan_max_cycles;
	WMA_LOGD("fast_scan_period: %d msec slow_scan_period: %d msec",
			cmd->fast_scan_period, cmd->slow_scan_period);
	WMA_LOGD("fast_scan_max_cycles: %d", cmd->fast_scan_max_cycles);

	buf_ptr += sizeof(wmi_nlo_config_cmd_fixed_param);

	cmd->no_of_ssids = CDF_MIN(pno->ucNetworksCount, WMI_NLO_MAX_SSIDS);
	WMA_LOGD("SSID count : %d", cmd->no_of_ssids);
	WMITLV_SET_HDR(buf_ptr, WMITLV_TAG_ARRAY_STRUC,
		       cmd->no_of_ssids * sizeof(nlo_configured_parameters));
	buf_ptr += WMI_TLV_HDR_SIZE;

	nlo_list = (nlo_configured_parameters *) buf_ptr;
	for (i = 0; i < cmd->no_of_ssids; i++) {
		WMITLV_SET_HDR(&nlo_list[i].tlv_header,
			       WMITLV_TAG_ARRAY_BYTE,
			       WMITLV_GET_STRUCT_TLVLEN
				       (nlo_configured_parameters));
		/* Copy ssid and it's length */
		nlo_list[i].ssid.valid = true;
		nlo_list[i].ssid.ssid.ssid_len = pno->aNetworks[i].ssId.length;
		cdf_mem_copy(nlo_list[i].ssid.ssid.ssid,
			     pno->aNetworks[i].ssId.ssId,
			     nlo_list[i].ssid.ssid.ssid_len);
		WMA_LOGD("index: %d ssid: %.*s len: %d", i,
			 nlo_list[i].ssid.ssid.ssid_len,
			 (char *)nlo_list[i].ssid.ssid.ssid,
			 nlo_list[i].ssid.ssid.ssid_len);

		/* Copy rssi threshold */
		if (pno->aNetworks[i].rssiThreshold &&
		    pno->aNetworks[i].rssiThreshold > WMA_RSSI_THOLD_DEFAULT) {
			nlo_list[i].rssi_cond.valid = true;
			nlo_list[i].rssi_cond.rssi =
				pno->aNetworks[i].rssiThreshold;
			WMA_LOGD("RSSI threshold : %d dBm",
				 nlo_list[i].rssi_cond.rssi);
		}
		nlo_list[i].bcast_nw_type.valid = true;
		nlo_list[i].bcast_nw_type.bcast_nw_type =
			pno->aNetworks[i].bcastNetwType;
		WMA_LOGI("Broadcast NW type (%u)",
			 nlo_list[i].bcast_nw_type.bcast_nw_type);
	}
	buf_ptr += cmd->no_of_ssids * sizeof(nlo_configured_parameters);

	/* Copy channel info */
	cmd->num_of_channels = CDF_MIN(pno->aNetworks[0].ucChannelCount,
				       WMI_NLO_MAX_CHAN);
	WMA_LOGD("Channel count: %d", cmd->num_of_channels);
	WMITLV_SET_HDR(buf_ptr, WMITLV_TAG_ARRAY_UINT32,
		       (cmd->num_of_channels * sizeof(uint32_t)));
	buf_ptr += WMI_TLV_HDR_SIZE;

	channel_list = (uint32_t *) buf_ptr;
	for (i = 0; i < cmd->num_of_channels; i++) {
		channel_list[i] = pno->aNetworks[0].aChannels[i];

		if (channel_list[i] < WMA_NLO_FREQ_THRESH)
			channel_list[i] = cds_chan_to_freq(channel_list[i]);

		WMA_LOGD("Ch[%d]: %d MHz", i, channel_list[i]);
	}
	buf_ptr += cmd->num_of_channels * sizeof(uint32_t);
	WMITLV_SET_HDR(buf_ptr, WMITLV_TAG_ARRAY_STRUC,
			sizeof(nlo_channel_prediction_cfg));
	buf_ptr += WMI_TLV_HDR_SIZE;
	wma_set_pno_channel_prediction(buf_ptr, pno);
	buf_ptr += WMI_TLV_HDR_SIZE;
	/* TODO: Discrete firmware doesn't have command/option to configure
	 * App IE which comes from wpa_supplicant as of part PNO start request.
	 */
	ret = wmi_unified_cmd_send(wma->wmi_handle, buf, len,
				   WMI_NETWORK_LIST_OFFLOAD_CONFIG_CMDID);
	if (ret) {
		WMA_LOGE("%s: Failed to send nlo wmi cmd", __func__);
		wmi_buf_free(buf);
		return CDF_STATUS_E_FAILURE;
	}

	wma->interfaces[pno->sessionId].pno_in_progress = true;

	WMA_LOGD("PNO start request sent successfully for vdev %d",
		 pno->sessionId);

	return CDF_STATUS_SUCCESS;
}

/**
 * wma_pno_stop() - PNO stop request
 * @wma: wma handle
 * @vdev_id: vdev id
 *
 * This function request FW to stop ongoing PNO operation.
 *
 * Return: CDF status
 */
CDF_STATUS wma_pno_stop(tp_wma_handle wma, uint8_t vdev_id)
{
	wmi_nlo_config_cmd_fixed_param *cmd;
	int32_t len = sizeof(*cmd);
	wmi_buf_t buf;
	uint8_t *buf_ptr;
	int ret;

	if (!wma->interfaces[vdev_id].pno_in_progress) {
		WMA_LOGD("No active pno session found for vdev %d, skip pno stop request",
			vdev_id);
		return CDF_STATUS_SUCCESS;
	}

	WMA_LOGD("PNO Stop");

	/*
	 * TLV place holder for array of structures nlo_configured_parameters
	 * TLV place holder for array of uint32_t channel_list
	 * TLV place holder for chnl prediction cfg
	 */
	len += WMI_TLV_HDR_SIZE + WMI_TLV_HDR_SIZE + WMI_TLV_HDR_SIZE;
	buf = wmi_buf_alloc(wma->wmi_handle, len);
	if (!buf) {
		WMA_LOGE("%s: Failed allocate wmi buffer", __func__);
		return CDF_STATUS_E_NOMEM;
	}

	cmd = (wmi_nlo_config_cmd_fixed_param *) wmi_buf_data(buf);
	buf_ptr = (uint8_t *) cmd;

	WMITLV_SET_HDR(&cmd->tlv_header,
		       WMITLV_TAG_STRUC_wmi_nlo_config_cmd_fixed_param,
		       WMITLV_GET_STRUCT_TLVLEN
			       (wmi_nlo_config_cmd_fixed_param));

	cmd->vdev_id = vdev_id;
	cmd->flags = WMI_NLO_CONFIG_STOP;
	buf_ptr += sizeof(*cmd);

	WMITLV_SET_HDR(buf_ptr, WMITLV_TAG_ARRAY_STRUC, 0);
	buf_ptr += WMI_TLV_HDR_SIZE;

	WMITLV_SET_HDR(buf_ptr, WMITLV_TAG_ARRAY_UINT32, 0);
	buf_ptr += WMI_TLV_HDR_SIZE;

	WMITLV_SET_HDR(buf_ptr, WMITLV_TAG_ARRAY_STRUC, 0);
	buf_ptr += WMI_TLV_HDR_SIZE;


	ret = wmi_unified_cmd_send(wma->wmi_handle, buf, len,
				   WMI_NETWORK_LIST_OFFLOAD_CONFIG_CMDID);
	if (ret) {
		WMA_LOGE("%s: Failed to send nlo wmi cmd", __func__);
		wmi_buf_free(buf);
		return CDF_STATUS_E_FAILURE;
	}

	wma->interfaces[vdev_id].pno_in_progress = false;

	WMA_LOGD("PNO stop request sent successfully for vdev %d", vdev_id);

	return CDF_STATUS_SUCCESS;
}

/**
 * wma_config_pno() - config PNO
 * @wma: wma handle
 * @pno: PNO request parameters
 *
 * Return: none
 */
void wma_config_pno(tp_wma_handle wma, tpSirPNOScanReq pno)
{
	CDF_STATUS ret;

	if (pno->enable)
		ret = wma_pno_start(wma, pno);
	else
		ret = wma_pno_stop(wma, pno->sessionId);

	if (ret)
		WMA_LOGE("%s: PNO %s failed %d", __func__,
			 pno->enable ? "start" : "stop", ret);

	/* SME expects WMA to free tpSirPNOScanReq memory after
	 * processing PNO request. */
	cdf_mem_free(pno);
}

#if defined(FEATURE_WLAN_ESE) && defined(FEATURE_WLAN_ESE_UPLOAD)

/**
 * wma_plm_start() - plm start request
 * @wma: wma handle
 * @plm: plm request parameters
 *
 * This function request FW to start PLM.
 *
 * Return: CDF status
 */
CDF_STATUS wma_plm_start(tp_wma_handle wma, const tpSirPlmReq plm)
{
	wmi_vdev_plmreq_start_cmd_fixed_param *cmd;
	uint32_t *channel_list;
	int32_t len;
	wmi_buf_t buf;
	uint8_t *buf_ptr;
	uint8_t count;
	int ret;

	if (NULL == plm || NULL == wma) {
		WMA_LOGE("%s: input pointer is NULL ", __func__);
		return CDF_STATUS_E_FAILURE;
	}
	WMA_LOGD("PLM Start");

	len = sizeof(*cmd) + WMI_TLV_HDR_SIZE;  /* TLV place holder for channel_list */
	len += sizeof(uint32_t) * plm->plmNumCh;

	buf = wmi_buf_alloc(wma->wmi_handle, len);
	if (!buf) {
		WMA_LOGE("%s: Failed allocate wmi buffer", __func__);
		return CDF_STATUS_E_NOMEM;
	}
	cmd = (wmi_vdev_plmreq_start_cmd_fixed_param *) wmi_buf_data(buf);

	buf_ptr = (uint8_t *) cmd;

	WMITLV_SET_HDR(&cmd->tlv_header,
		       WMITLV_TAG_STRUC_wmi_vdev_plmreq_start_cmd_fixed_param,
		       WMITLV_GET_STRUCT_TLVLEN
			       (wmi_vdev_plmreq_start_cmd_fixed_param));

	cmd->vdev_id = plm->sessionId;

	cmd->meas_token = plm->meas_token;
	cmd->dialog_token = plm->diag_token;
	cmd->number_bursts = plm->numBursts;
	cmd->burst_interval = WMA_SEC_TO_MSEC(plm->burstInt);
	cmd->off_duration = plm->measDuration;
	cmd->burst_cycle = plm->burstLen;
	cmd->tx_power = plm->desiredTxPwr;
	WMI_CHAR_ARRAY_TO_MAC_ADDR(plm->mac_addr.bytes, &cmd->dest_mac);
	cmd->num_chans = plm->plmNumCh;

	buf_ptr += sizeof(wmi_vdev_plmreq_start_cmd_fixed_param);

	WMA_LOGD("vdev : %d measu token : %d", cmd->vdev_id, cmd->meas_token);
	WMA_LOGD("dialog_token: %d", cmd->dialog_token);
	WMA_LOGD("number_bursts: %d", cmd->number_bursts);
	WMA_LOGD("burst_interval: %d", cmd->burst_interval);
	WMA_LOGD("off_duration: %d", cmd->off_duration);
	WMA_LOGD("burst_cycle: %d", cmd->burst_cycle);
	WMA_LOGD("tx_power: %d", cmd->tx_power);
	WMA_LOGD("Number of channels : %d", cmd->num_chans);

	WMITLV_SET_HDR(buf_ptr, WMITLV_TAG_ARRAY_UINT32,
		       (cmd->num_chans * sizeof(uint32_t)));

	buf_ptr += WMI_TLV_HDR_SIZE;
	if (cmd->num_chans) {
		channel_list = (uint32_t *) buf_ptr;
		for (count = 0; count < cmd->num_chans; count++) {
			channel_list[count] = plm->plmChList[count];
			if (channel_list[count] < WMA_NLO_FREQ_THRESH)
				channel_list[count] =
					cds_chan_to_freq(channel_list[count]);
			WMA_LOGD("Ch[%d]: %d MHz", count, channel_list[count]);
		}
		buf_ptr += cmd->num_chans * sizeof(uint32_t);
	}

	ret = wmi_unified_cmd_send(wma->wmi_handle, buf, len,
				   WMI_VDEV_PLMREQ_START_CMDID);
	if (ret) {
		WMA_LOGE("%s: Failed to send plm start wmi cmd", __func__);
		wmi_buf_free(buf);
		return CDF_STATUS_E_FAILURE;
	}
	wma->interfaces[plm->sessionId].plm_in_progress = true;

	WMA_LOGD("Plm start request sent successfully for vdev %d",
		 plm->sessionId);

	return CDF_STATUS_SUCCESS;
}

/**
 * wma_plm_stop() - plm stop request
 * @wma: wma handle
 * @plm: plm request parameters
 *
 * This function request FW to stop PLM.
 *
 * Return: CDF status
 */
CDF_STATUS wma_plm_stop(tp_wma_handle wma, const tpSirPlmReq plm)
{
	wmi_vdev_plmreq_stop_cmd_fixed_param *cmd;
	int32_t len;
	wmi_buf_t buf;
	uint8_t *buf_ptr;
	int ret;

	if (NULL == plm || NULL == wma) {
		WMA_LOGE("%s: input pointer is NULL ", __func__);
		return CDF_STATUS_E_FAILURE;
	}

	if (false == wma->interfaces[plm->sessionId].plm_in_progress) {
		WMA_LOGE("No active plm req found, skip plm stop req");
		return CDF_STATUS_E_FAILURE;
	}

	WMA_LOGD("PLM Stop");

	len = sizeof(*cmd);
	buf = wmi_buf_alloc(wma->wmi_handle, len);
	if (!buf) {
		WMA_LOGE("%s: Failed allocate wmi buffer", __func__);
		return CDF_STATUS_E_NOMEM;
	}

	cmd = (wmi_vdev_plmreq_stop_cmd_fixed_param *) wmi_buf_data(buf);

	buf_ptr = (uint8_t *) cmd;

	WMITLV_SET_HDR(&cmd->tlv_header,
		       WMITLV_TAG_STRUC_wmi_vdev_plmreq_stop_cmd_fixed_param,
		       WMITLV_GET_STRUCT_TLVLEN
			       (wmi_vdev_plmreq_stop_cmd_fixed_param));

	cmd->vdev_id = plm->sessionId;

	cmd->meas_token = plm->meas_token;
	WMA_LOGD("vdev %d meas token %d", cmd->vdev_id, cmd->meas_token);

	ret = wmi_unified_cmd_send(wma->wmi_handle, buf, len,
				   WMI_VDEV_PLMREQ_STOP_CMDID);
	if (ret) {
		WMA_LOGE("%s: Failed to send plm stop wmi cmd", __func__);
		wmi_buf_free(buf);
		return CDF_STATUS_E_FAILURE;
	}
	wma->interfaces[plm->sessionId].plm_in_progress = false;

	WMA_LOGD("Plm stop request sent successfully for vdev %d",
		 plm->sessionId);

	return CDF_STATUS_SUCCESS;
}

/**
 * wma_config_plm()- config PLM
 * @wma: wma handle
 * @plm: plm request parameters
 *
 * Return: none
 */
void wma_config_plm(tp_wma_handle wma, tpSirPlmReq plm)
{
	CDF_STATUS ret = 0;

	if (NULL == plm || NULL == wma)
		return;

	if (plm->enable)
		ret = wma_plm_start(wma, plm);
	else
		ret = wma_plm_stop(wma, plm);

	if (ret)
		WMA_LOGE("%s: PLM %s failed %d", __func__,
			 plm->enable ? "start" : "stop", ret);

	/* SME expects WMA to free tpSirPlmReq memory after
	 * processing PLM request. */
	cdf_mem_free(plm);
	plm = NULL;
}
#endif

/**
 * wma_scan_cache_updated_ind() - scan update indication
 * @wma: wma handle
 * @sessionId: session ID
 *
 * After pushing cached scan results (that are stored in LIM) to SME,
 * PE will post WMA_SME_SCAN_CACHE_UPDATED message indication to
 * wma and intern this function handles that message. This function will
 * check for PNO completion (by checking NLO match event) and post PNO
 * completion back to SME if PNO operation is completed successfully.
 *
 * Return: none
 */
void wma_scan_cache_updated_ind(tp_wma_handle wma, uint8_t sessionId)
{
	tSirPrefNetworkFoundInd *nw_found_ind;
	CDF_STATUS status;
	cds_msg_t cds_msg;
	uint8_t len, i;

	for (i = 0; i < wma->max_bssid; i++) {
		if (wma->interfaces[i].nlo_match_evt_received)
			break;
	}

	if (i == wma->max_bssid) {
		WMA_LOGD("PNO match event is not received in any vdev, skip scan cache update indication");
		return;
	}
	wma->interfaces[i].nlo_match_evt_received = false;

	WMA_LOGD("Posting PNO completion to umac");

	len = sizeof(tSirPrefNetworkFoundInd);
	nw_found_ind = (tSirPrefNetworkFoundInd *) cdf_mem_malloc(len);

	if (NULL == nw_found_ind) {
		WMA_LOGE("%s: Memory allocation failure", __func__);
		return;
	}

	nw_found_ind->mesgType = eWNI_SME_PREF_NETWORK_FOUND_IND;
	nw_found_ind->mesgLen = len;
	nw_found_ind->sessionId = sessionId;

	cds_msg.type = eWNI_SME_PREF_NETWORK_FOUND_IND;
	cds_msg.bodyptr = (void *)nw_found_ind;
	cds_msg.bodyval = 0;

	status = cds_mq_post_message(CDS_MQ_ID_SME, &cds_msg);
	if (status != CDF_STATUS_SUCCESS) {
		WMA_LOGE("%s: Failed to post PNO completion match event to SME",
			 __func__);
		cdf_mem_free(nw_found_ind);
	}
}

#ifdef FEATURE_WLAN_EXTSCAN
/**
 * wma_extscan_get_eventid_from_tlvtag() - map tlv tag to corresponding event id
 * @tag: WMI TLV tag
 *
 * Return:
 *	0 if TLV tag is invalid
 *	else return corresponding WMI event id
 */
static int wma_extscan_get_eventid_from_tlvtag(uint32_t tag)
{
	uint32_t event_id;

	switch (tag) {
	case WMITLV_TAG_STRUC_wmi_extscan_start_stop_event_fixed_param:
		event_id = WMI_EXTSCAN_START_STOP_EVENTID;
		break;

	case WMITLV_TAG_STRUC_wmi_extscan_operation_event_fixed_param:
		event_id = WMI_EXTSCAN_OPERATION_EVENTID;
		break;

	case WMITLV_TAG_STRUC_wmi_extscan_table_usage_event_fixed_param:
		event_id = WMI_EXTSCAN_TABLE_USAGE_EVENTID;
		break;

	case WMITLV_TAG_STRUC_wmi_extscan_cached_results_event_fixed_param:
		event_id = WMI_EXTSCAN_CACHED_RESULTS_EVENTID;
		break;

	case WMITLV_TAG_STRUC_wmi_extscan_wlan_change_results_event_fixed_param:
		event_id = WMI_EXTSCAN_WLAN_CHANGE_RESULTS_EVENTID;
		break;

	case WMITLV_TAG_STRUC_wmi_extscan_hotlist_match_event_fixed_param:
		event_id = WMI_EXTSCAN_HOTLIST_MATCH_EVENTID;
		break;

	case WMITLV_TAG_STRUC_wmi_extscan_capabilities_event_fixed_param:
		event_id = WMI_EXTSCAN_CAPABILITIES_EVENTID;
		break;

	case WMITLV_TAG_STRUC_wmi_extscan_hotlist_ssid_match_event_fixed_param:
		event_id = WMI_EXTSCAN_HOTLIST_SSID_MATCH_EVENTID;
		break;

	default:
		event_id = 0;
		WMA_LOGE("%s: Unknown tag: %d", __func__, tag);
		break;
	}

	WMA_LOGI("%s: For tag %d WMI event 0x%x", __func__, tag, event_id);
	return event_id;
}

/**
 * wma_extscan_wow_event_callback() - extscan wow event callback
 * @handle: WMA handle
 * @event: event buffer
 * @len: length of @event buffer
 *
 * In wow case, the wow event is followed by the payload of the event
 * which generated the wow event.
 * payload is 4 bytes of length followed by event buffer. the first 4 bytes
 * of event buffer is common tlv header, which is a combination
 * of tag (higher 2 bytes) and length (lower 2 bytes). The tag is used to
 * identify the event which triggered wow event.
 *
 * @Return: none
 */
void wma_extscan_wow_event_callback(void *handle, void *event, uint32_t len)
{
	uint32_t id;
	int tlv_ok_status = 0;
	void *wmi_cmd_struct_ptr = NULL;
	uint32_t tag = WMITLV_GET_TLVTAG(WMITLV_GET_HDR(event));

	id = wma_extscan_get_eventid_from_tlvtag(tag);
	if (!id) {
		WMA_LOGE("%s: Invalid  Tag: %d", __func__, tag);
		return;
	}

	tlv_ok_status = wmitlv_check_and_pad_event_tlvs(
				handle, event, len, id,
				&wmi_cmd_struct_ptr);
	if (tlv_ok_status != 0) {
		WMA_LOGE("%s: Invalid Tag: %d could not check and pad tlvs",
			 __func__, tag);
		return;
	}

	switch (tag) {
	case WMITLV_TAG_STRUC_wmi_extscan_start_stop_event_fixed_param:
		wma_extscan_start_stop_event_handler(handle,
				wmi_cmd_struct_ptr, len);
		break;

	case WMITLV_TAG_STRUC_wmi_extscan_operation_event_fixed_param:
		wma_extscan_operations_event_handler(handle,
				wmi_cmd_struct_ptr, len);
		break;

	case WMITLV_TAG_STRUC_wmi_extscan_table_usage_event_fixed_param:
		wma_extscan_table_usage_event_handler(handle,
				wmi_cmd_struct_ptr, len);
		break;

	case WMITLV_TAG_STRUC_wmi_extscan_cached_results_event_fixed_param:
		wma_extscan_cached_results_event_handler(handle,
				wmi_cmd_struct_ptr, len);
		break;

	case WMITLV_TAG_STRUC_wmi_extscan_wlan_change_results_event_fixed_param:
		wma_extscan_change_results_event_handler(handle,
				wmi_cmd_struct_ptr, len);
		break;

	case WMITLV_TAG_STRUC_wmi_extscan_hotlist_match_event_fixed_param:
		wma_extscan_hotlist_match_event_handler(handle,
				wmi_cmd_struct_ptr, len);
		break;

	case WMITLV_TAG_STRUC_wmi_extscan_capabilities_event_fixed_param:
		wma_extscan_capabilities_event_handler(handle,
				wmi_cmd_struct_ptr, len);
		break;

	case WMITLV_TAG_STRUC_wmi_extscan_hotlist_ssid_match_event_fixed_param:
		wma_extscan_hotlist_ssid_match_event_handler(handle,
				wmi_cmd_struct_ptr, len);
		break;

	default:
		WMA_LOGE("%s: Unknown tag: %d", __func__, tag);
		break;
	}
	wmitlv_free_allocated_event_tlvs(id, &wmi_cmd_struct_ptr);

	return;
}
#endif

/**
 * wma_nlo_match_evt_handler() - nlo match event handler
 * @handle: wma handle
 * @event: event data
 * @len: data length
 *
 * Record NLO match event comes from FW. It's a indication that
 * one of the profile is matched.
 *
 * Return: 0 for success or error code.
 */
int wma_nlo_match_evt_handler(void *handle, uint8_t *event,
				     uint32_t len)
{
	tp_wma_handle wma = (tp_wma_handle) handle;
	wmi_nlo_event *nlo_event;
	WMI_NLO_MATCH_EVENTID_param_tlvs *param_buf =
		(WMI_NLO_MATCH_EVENTID_param_tlvs *) event;
	struct wma_txrx_node *node;

	if (!param_buf) {
		WMA_LOGE("Invalid NLO match event buffer");
		return -EINVAL;
	}

	nlo_event = param_buf->fixed_param;
	WMA_LOGD("PNO match event received for vdev %d", nlo_event->vdev_id);

	node = &wma->interfaces[nlo_event->vdev_id];
	if (node)
		node->nlo_match_evt_received = true;

	cdf_wake_lock_timeout_acquire(&wma->pno_wake_lock,
				      WMA_PNO_MATCH_WAKE_LOCK_TIMEOUT,
				      WIFI_POWER_EVENT_WAKELOCK_PNO);

	return 0;
}

/**
 * wma_nlo_scan_cmp_evt_handler() - nlo scan completion handler
 * @handle: wma handle
 * @event: event handler
 * @len: length of data
 *
 * This function handles NLO scan completion event.
 *
 * Return: 0 for success or error code.
 */
int wma_nlo_scan_cmp_evt_handler(void *handle, uint8_t *event,
					uint32_t len)
{
	tp_wma_handle wma = (tp_wma_handle) handle;
	wmi_nlo_event *nlo_event;
	WMI_NLO_SCAN_COMPLETE_EVENTID_param_tlvs *param_buf =
		(WMI_NLO_SCAN_COMPLETE_EVENTID_param_tlvs *) event;
	tSirScanOffloadEvent *scan_event;
	struct wma_txrx_node *node;

	if (!param_buf) {
		WMA_LOGE("Invalid NLO scan comp event buffer");
		return -EINVAL;
	}

	nlo_event = param_buf->fixed_param;
	WMA_LOGD("PNO scan completion event received for vdev %d",
		 nlo_event->vdev_id);

	node = &wma->interfaces[nlo_event->vdev_id];

	/* Handle scan completion event only after NLO match event. */
	if (!node || !node->nlo_match_evt_received) {
		WMA_LOGD("NLO match not received skip PNO complete ind for vdev %d",
			nlo_event->vdev_id);
		goto skip_pno_cmp_ind;
	}

	cdf_wake_lock_release(&wma->pno_wake_lock,
		WIFI_POWER_EVENT_WAKELOCK_PNO);
	scan_event =
		(tSirScanOffloadEvent *)
		cdf_mem_malloc(sizeof(tSirScanOffloadEvent));
	if (scan_event) {
		/* Posting scan completion msg would take scan cache result
		 * from LIM module and update in scan cache maintained in SME.*/
		WMA_LOGE("Posting PNO Scan completion to umac");
		cdf_wake_lock_timeout_acquire(&wma->pno_wake_lock,
				WMA_PNO_SCAN_COMPLETE_WAKE_LOCK_TIMEOUT,
				WIFI_POWER_EVENT_WAKELOCK_PNO);
		cdf_mem_zero(scan_event, sizeof(tSirScanOffloadEvent));
		scan_event->reasonCode = eSIR_PNO_SCAN_SUCCESS;
		scan_event->event = SCAN_EVENT_COMPLETED;
		scan_event->sessionId = nlo_event->vdev_id;
		wma_send_msg(wma, WMA_RX_SCAN_EVENT, (void *)scan_event, 0);
	} else {
		WMA_LOGE("Memory allocation failed for tSirScanOffloadEvent");
	}

skip_pno_cmp_ind:
	return 0;
}

#endif

/**
 * wma_register_extscan_event_handler() - register extscan event handler
 * @wma_handle: wma handle
 *
 * This function register extscan related event handlers.
 *
 * Return: none
 */
void wma_register_extscan_event_handler(tp_wma_handle wma_handle)
{
	if (!wma_handle) {
		WMA_LOGE("%s: extscan wma_handle is NULL", __func__);
		return;
	}
	wmi_unified_register_event_handler(wma_handle->wmi_handle,
					   WMI_EXTSCAN_START_STOP_EVENTID,
					   wma_extscan_start_stop_event_handler);

	wmi_unified_register_event_handler(wma_handle->wmi_handle,
					   WMI_EXTSCAN_CAPABILITIES_EVENTID,
					   wma_extscan_capabilities_event_handler);

	wmi_unified_register_event_handler(wma_handle->wmi_handle,
					   WMI_EXTSCAN_HOTLIST_MATCH_EVENTID,
					   wma_extscan_hotlist_match_event_handler);

	wmi_unified_register_event_handler(wma_handle->wmi_handle,
					   WMI_EXTSCAN_WLAN_CHANGE_RESULTS_EVENTID,
					   wma_extscan_change_results_event_handler);

	wmi_unified_register_event_handler(wma_handle->wmi_handle,
					   WMI_EXTSCAN_OPERATION_EVENTID,
					   wma_extscan_operations_event_handler);
	wmi_unified_register_event_handler(wma_handle->wmi_handle,
					   WMI_EXTSCAN_TABLE_USAGE_EVENTID,
					   wma_extscan_table_usage_event_handler);

	wmi_unified_register_event_handler(wma_handle->wmi_handle,
					   WMI_EXTSCAN_CACHED_RESULTS_EVENTID,
					   wma_extscan_cached_results_event_handler);

	wmi_unified_register_event_handler(wma_handle->wmi_handle,
			WMI_PASSPOINT_MATCH_EVENTID,
			wma_passpoint_match_event_handler);


	wmi_unified_register_event_handler(wma_handle->wmi_handle,
			WMI_EXTSCAN_HOTLIST_SSID_MATCH_EVENTID,
			wma_extscan_hotlist_ssid_match_event_handler);

	return;
}

#ifdef FEATURE_WLAN_EXTSCAN

/**
 * wma_extscan_start_stop_event_handler() -  extscan start/stop event handler
 * @handle: wma handle
 * @cmd_param_info: event buffer
 * @len: data length
 *
 * This function handles different extscan related commands
 * like start/stop/get results etc and indicate to upper layers.
 *
 * Return: 0 for success or error code.
 */
int wma_extscan_start_stop_event_handler(void *handle,
					 uint8_t *cmd_param_info,
					 uint32_t len)
{
	WMI_EXTSCAN_START_STOP_EVENTID_param_tlvs *param_buf;
	wmi_extscan_start_stop_event_fixed_param *event;
	struct sir_extscan_generic_response   *extscan_ind;
	uint16_t event_type;
	tpAniSirGlobal pMac = cds_get_context(CDF_MODULE_ID_PE);
	if (!pMac) {
		WMA_LOGE("%s: Invalid pMac", __func__);
		return -EINVAL;
	}
	if (!pMac->sme.pExtScanIndCb) {
		WMA_LOGE("%s: Callback not registered", __func__);
		return -EINVAL;
	}
	param_buf = (WMI_EXTSCAN_START_STOP_EVENTID_param_tlvs *)
		    cmd_param_info;
	if (!param_buf) {
		WMA_LOGE("%s: Invalid extscan event", __func__);
		return -EINVAL;
	}
	event = param_buf->fixed_param;
	extscan_ind = cdf_mem_malloc(sizeof(*extscan_ind));
	if (!extscan_ind) {
		WMA_LOGE("%s: extscan memory allocation failed", __func__);
		return -ENOMEM;
	}
	switch (event->command) {
	case WMI_EXTSCAN_START_CMDID:
		event_type = eSIR_EXTSCAN_START_RSP;
		extscan_ind->status = event->status;
		extscan_ind->request_id = event->request_id;
		break;
	case WMI_EXTSCAN_STOP_CMDID:
		event_type = eSIR_EXTSCAN_STOP_RSP;
		extscan_ind->status = event->status;
		extscan_ind->request_id = event->request_id;
		break;
	case WMI_EXTSCAN_CONFIGURE_WLAN_CHANGE_MONITOR_CMDID:
		extscan_ind->status = event->status;
		extscan_ind->request_id = event->request_id;
		if (event->mode == WMI_EXTSCAN_MODE_STOP) {
			event_type =
				eSIR_EXTSCAN_RESET_SIGNIFICANT_WIFI_CHANGE_RSP;
		} else {
			event_type =
				eSIR_EXTSCAN_SET_SIGNIFICANT_WIFI_CHANGE_RSP;
		}
		break;
	case WMI_EXTSCAN_CONFIGURE_HOTLIST_MONITOR_CMDID:
		extscan_ind->status = event->status;
		extscan_ind->request_id = event->request_id;
		if (event->mode == WMI_EXTSCAN_MODE_STOP) {
			event_type = eSIR_EXTSCAN_RESET_BSSID_HOTLIST_RSP;
		} else {
			event_type = eSIR_EXTSCAN_SET_BSSID_HOTLIST_RSP;
		}
		break;
	case WMI_EXTSCAN_GET_CACHED_RESULTS_CMDID:
		extscan_ind->status = event->status;
		extscan_ind->request_id = event->request_id;
		event_type = eSIR_EXTSCAN_CACHED_RESULTS_RSP;
		break;
	case WMI_EXTSCAN_CONFIGURE_HOTLIST_SSID_MONITOR_CMDID:
		extscan_ind->status = event->status;
		extscan_ind->request_id = event->request_id;
		if (event->mode == WMI_EXTSCAN_MODE_STOP) {
			event_type =
				eSIR_EXTSCAN_RESET_SSID_HOTLIST_RSP;
		} else {
			event_type =
				eSIR_EXTSCAN_SET_SSID_HOTLIST_RSP;
		}
		break;
	default:
		WMA_LOGE("%s: Unknown event(%d) from target",
			 __func__, event->status);
		cdf_mem_free(extscan_ind);
		return -EINVAL;
	}
	pMac->sme.pExtScanIndCb(pMac->hHdd, event_type, extscan_ind);
	WMA_LOGD("%s: sending event to umac for requestid %u"
		 "with status %d", __func__,
		 extscan_ind->request_id, extscan_ind->status);
	cdf_mem_free(extscan_ind);
	return 0;
}

/**
 * wma_extscan_operations_event_handler() - extscan operation event handler
 * @handle: wma handle
 * @cmd_param_info: event buffer
 * @len: length
 *
 * This function handles different operations related event and indicate
 * upper layers with appropriate callback.
 *
 * Return: 0 for success or error code.
 */
int wma_extscan_operations_event_handler(void *handle,
					 uint8_t *cmd_param_info,
					 uint32_t len)
{
	tp_wma_handle wma = (tp_wma_handle) handle;
	WMI_EXTSCAN_OPERATION_EVENTID_param_tlvs *param_buf;
	wmi_extscan_operation_event_fixed_param *oprn_event;
	tSirExtScanOnScanEventIndParams *oprn_ind;
	tpAniSirGlobal pMac = cds_get_context(CDF_MODULE_ID_PE);
	if (!pMac) {
		WMA_LOGE("%s: Invalid pMac", __func__);
		return -EINVAL;
	}
	if (!pMac->sme.pExtScanIndCb) {
		WMA_LOGE("%s: Callback not registered", __func__);
		return -EINVAL;
	}
	param_buf = (WMI_EXTSCAN_OPERATION_EVENTID_param_tlvs *)
		    cmd_param_info;
	if (!param_buf) {
		WMA_LOGE("%s: Invalid scan operation event", __func__);
		return -EINVAL;
	}
	oprn_event = param_buf->fixed_param;
	oprn_ind = cdf_mem_malloc(sizeof(*oprn_ind));
	if (!oprn_ind) {
		WMA_LOGE("%s: extscan memory allocation failed", __func__);
		cdf_mem_free(oprn_ind);
		return -ENOMEM;
	}

	oprn_ind->requestId = oprn_event->request_id;

	switch (oprn_event->event) {
	case WMI_EXTSCAN_BUCKET_COMPLETED_EVENT:
		oprn_ind->scanEventType = WIFI_SCAN_COMPLETE;
		oprn_ind->status = 0;
		break;
	case WMI_EXTSCAN_CYCLE_STARTED_EVENT:
		WMA_LOGD("%s: received WMI_EXTSCAN_CYCLE_STARTED_EVENT",
			 __func__);
		cdf_wake_lock_timeout_acquire(&wma->extscan_wake_lock,
				      WMA_EXTSCAN_CYCLE_WAKE_LOCK_DURATION,
				      WIFI_POWER_EVENT_WAKELOCK_EXT_SCAN);
		goto exit_handler;
	case WMI_EXTSCAN_CYCLE_COMPLETED_EVENT:
		WMA_LOGD("%s: received WMI_EXTSCAN_CYCLE_COMPLETED_EVENT",
			 __func__);
		cdf_wake_lock_release(&wma->extscan_wake_lock,
				      WIFI_POWER_EVENT_WAKELOCK_EXT_SCAN);
		goto exit_handler;
	default:
		WMA_LOGE("%s: Unknown event(%d) from target",
			 __func__, oprn_event->event);
		cdf_mem_free(oprn_ind);
		return -EINVAL;
	}
	pMac->sme.pExtScanIndCb(pMac->hHdd,
				eSIR_EXTSCAN_SCAN_PROGRESS_EVENT_IND, oprn_ind);
	WMA_LOGI("%s: sending scan progress event to hdd", __func__);
exit_handler:
	cdf_mem_free(oprn_ind);
	return 0;
}

/**
 * wma_extscan_table_usage_event_handler() - extscan table usage event handler
 * @handle: wma handle
 * @cmd_param_info: event buffer
 * @len: length
 *
 * This function handles table usage related event and indicate
 * upper layers with appropriate callback.
 *
 * Return: 0 for success or error code.
 */
int wma_extscan_table_usage_event_handler(void *handle,
					  uint8_t *cmd_param_info,
					  uint32_t len)
{
	WMI_EXTSCAN_TABLE_USAGE_EVENTID_param_tlvs *param_buf;
	wmi_extscan_table_usage_event_fixed_param *event;
	tSirExtScanResultsAvailableIndParams *tbl_usg_ind;
	tpAniSirGlobal pMac = cds_get_context(CDF_MODULE_ID_PE);
	if (!pMac) {
		WMA_LOGE("%s: Invalid pMac", __func__);
		return -EINVAL;
	}
	if (!pMac->sme.pExtScanIndCb) {
		WMA_LOGE("%s: Callback not registered", __func__);
		return -EINVAL;
	}
	param_buf = (WMI_EXTSCAN_TABLE_USAGE_EVENTID_param_tlvs *)
		    cmd_param_info;
	if (!param_buf) {
		WMA_LOGE("%s: Invalid table usage event", __func__);
		return -EINVAL;
	}
	event = param_buf->fixed_param;
	tbl_usg_ind = cdf_mem_malloc(sizeof(*tbl_usg_ind));
	if (!tbl_usg_ind) {
		WMA_LOGE("%s: table usage allocation failed", __func__);
		return -ENOMEM;
	}
	tbl_usg_ind->requestId = event->request_id;
	tbl_usg_ind->numResultsAvailable = event->entries_in_use;

	pMac->sme.pExtScanIndCb(pMac->hHdd,
				eSIR_EXTSCAN_SCAN_RES_AVAILABLE_IND,
				tbl_usg_ind);
	WMA_LOGI("%s: sending scan_res available event to hdd", __func__);
	cdf_mem_free(tbl_usg_ind);
	return 0;
}

/**
 * wma_extscan_capabilities_event_handler() - extscan capabilities event handler
 * @handle: wma handle
 * @cmd_param_info: event buffer
 * @len: length
 *
 * This function handles capabilities event and indicate
 * upper layers with registered callback.
 *
 * Return: 0 for success or error code.
 */
int wma_extscan_capabilities_event_handler(void *handle,
					   uint8_t *cmd_param_info,
					   uint32_t len)
{
	WMI_EXTSCAN_CAPABILITIES_EVENTID_param_tlvs *param_buf;
	wmi_extscan_capabilities_event_fixed_param *event;
	wmi_extscan_cache_capabilities *src_cache;
	wmi_extscan_hotlist_monitor_capabilities *src_hotlist;
	wmi_extscan_wlan_change_monitor_capabilities *src_change;

	struct ext_scan_capabilities_response  *dest_capab;
	tpAniSirGlobal pMac = cds_get_context(CDF_MODULE_ID_PE);
	if (!pMac) {
		WMA_LOGE("%s: Invalid pMac", __func__);
		return -EINVAL;
	}
	if (!pMac->sme.pExtScanIndCb) {
		WMA_LOGE("%s: Callback not registered", __func__);
		return -EINVAL;
	}
	param_buf = (WMI_EXTSCAN_CAPABILITIES_EVENTID_param_tlvs *)
		    cmd_param_info;
	if (!param_buf) {
		WMA_LOGE("%s: Invalid capabilities event", __func__);
		return -EINVAL;
	}
	event = param_buf->fixed_param;
	src_cache = param_buf->extscan_cache_capabilities;
	src_hotlist = param_buf->hotlist_capabilities;
	src_change = param_buf->wlan_change_capabilities;

	if (!src_cache || !src_hotlist || !src_change) {
		WMA_LOGE("%s: Invalid capabilities list", __func__);
		return -EINVAL;
	}
	dest_capab = cdf_mem_malloc(sizeof(*dest_capab));
	if (!dest_capab) {
		WMA_LOGE("%s: Allocation failed for capabilities buffer",
			 __func__);
		return -ENOMEM;
	}
	dest_capab->requestId = event->request_id;
	dest_capab->max_scan_buckets = src_cache->max_buckets;
	dest_capab->max_scan_cache_size = src_cache->scan_cache_entry_size;
	dest_capab->max_ap_cache_per_scan = src_cache->max_bssid_per_scan;
	dest_capab->max_scan_reporting_threshold =
		src_cache->max_table_usage_threshold;

	dest_capab->max_hotlist_bssids = src_hotlist->max_hotlist_entries;
	dest_capab->max_rssi_sample_size = src_change->max_rssi_averaging_samples;
	dest_capab->max_bssid_history_entries =
		src_change->max_rssi_history_entries;
	dest_capab->max_significant_wifi_change_aps =
		src_change->max_wlan_change_entries;
	dest_capab->max_hotlist_ssids =
				event->num_extscan_hotlist_ssid;
	dest_capab->max_number_epno_networks =
				event->num_epno_networks;
	dest_capab->max_number_epno_networks_by_ssid =
				event->num_epno_networks;
	dest_capab->max_number_of_white_listed_ssid =
				event->num_roam_ssid_whitelist;
	dest_capab->status = 0;

	WMA_LOGD("%s: request_id: %u status: %d",
		__func__, dest_capab->requestId, dest_capab->status);

	WMA_LOGD("%s: Capabilities: max_scan_buckets: %d,"
		"max_hotlist_bssids: %d, max_scan_cache_size: %d,"
		"max_ap_cache_per_scan: %d, max_scan_reporting_threshold: %d,"
		"max_rssi_sample_size: %d, max_bssid_history_entries: %d,"
		"max_significant_wifi_change_aps: %d",
		 __func__, dest_capab->max_scan_buckets,
		dest_capab->max_hotlist_bssids,
		dest_capab->max_scan_cache_size,
		dest_capab->max_ap_cache_per_scan,
		dest_capab->max_scan_reporting_threshold,
		dest_capab->max_rssi_sample_size,
		dest_capab->max_bssid_history_entries,
		dest_capab->max_significant_wifi_change_aps);

	WMA_LOGD("%s: Capabilities: max_hotlist_ssids: %d,"
		 "max_number_epno_networks: %d, max_number_epno_networks_by_ssid: %d,"
		 "max_number_of_white_listed_ssid: %d",
		 __func__, dest_capab->max_hotlist_ssids,
		dest_capab->max_number_epno_networks,
		dest_capab->max_number_epno_networks_by_ssid,
		dest_capab->max_number_of_white_listed_ssid);

	pMac->sme.pExtScanIndCb(pMac->hHdd,
				eSIR_EXTSCAN_GET_CAPABILITIES_IND, dest_capab);
	WMA_LOGI("%s: sending capabilities event to hdd", __func__);
	cdf_mem_free(dest_capab);
	return 0;
}

/**
 * wma_extscan_hotlist_match_event_handler() - hotlist match event handler
 * @handle: wma handle
 * @cmd_param_info: event buffer
 * @len: length
 *
 * This function handles hotlist match event and indicate
 * upper layers with registered callback.
 *
 * Return: 0 for success or error code.
 */
int wma_extscan_hotlist_match_event_handler(void *handle,
					    uint8_t *cmd_param_info,
					    uint32_t len)
{
	WMI_EXTSCAN_HOTLIST_MATCH_EVENTID_param_tlvs *param_buf;
	wmi_extscan_hotlist_match_event_fixed_param *event;
	struct extscan_hotlist_match *dest_hotlist;
	tSirWifiScanResult *dest_ap;
	wmi_extscan_wlan_descriptor *src_hotlist;
	int numap, j, ap_found = 0;
	tpAniSirGlobal pMac = cds_get_context(CDF_MODULE_ID_PE);

	if (!pMac) {
		WMA_LOGE("%s: Invalid pMac", __func__);
		return -EINVAL;
	}
	if (!pMac->sme.pExtScanIndCb) {
		WMA_LOGE("%s: Callback not registered", __func__);
		return -EINVAL;
	}
	param_buf = (WMI_EXTSCAN_HOTLIST_MATCH_EVENTID_param_tlvs *)
		    cmd_param_info;
	if (!param_buf) {
		WMA_LOGE("%s: Invalid hotlist match event", __func__);
		return -EINVAL;
	}
	event = param_buf->fixed_param;
	src_hotlist = param_buf->hotlist_match;
	numap = event->total_entries;

	if (!src_hotlist || !numap) {
		WMA_LOGE("%s: Hotlist AP's list invalid", __func__);
		return -EINVAL;
	}
	dest_hotlist = cdf_mem_malloc(sizeof(*dest_hotlist) +
				      sizeof(*dest_ap) * numap);
	if (!dest_hotlist) {
		WMA_LOGE("%s: Allocation failed for hotlist buffer", __func__);
		return -ENOMEM;
	}
	dest_ap = &dest_hotlist->ap[0];
	dest_hotlist->numOfAps = event->total_entries;
	dest_hotlist->requestId = event->config_request_id;

	if (event->first_entry_index +
		event->num_entries_in_page < event->total_entries)
		dest_hotlist->moreData = 1;
	else
		dest_hotlist->moreData = 0;

	WMA_LOGD("%s: Hotlist match: requestId: %u,"
		 "numOfAps: %d", __func__,
		 dest_hotlist->requestId, dest_hotlist->numOfAps);

	/*
	 * Currently firmware sends only one bss information in-case
	 * of both hotlist ap found and lost.
	 */
	for (j = 0; j < numap; j++) {
		dest_ap->rssi = 0;
		dest_ap->channel = src_hotlist->channel;
		dest_ap->ts = src_hotlist->tstamp;
		ap_found = src_hotlist->flags & WMI_HOTLIST_FLAG_PRESENCE;
		dest_ap->rtt = src_hotlist->rtt;
		dest_ap->rtt_sd = src_hotlist->rtt_sd;
		dest_ap->beaconPeriod = src_hotlist->beacon_interval;
		dest_ap->capability = src_hotlist->capabilities;
		dest_ap->ieLength = src_hotlist->ie_length;
		WMI_MAC_ADDR_TO_CHAR_ARRAY(&src_hotlist->bssid,
					   dest_ap->bssid.bytes);
		cdf_mem_copy(dest_ap->ssid, src_hotlist->ssid.ssid,
			     src_hotlist->ssid.ssid_len);
		dest_ap->ssid[src_hotlist->ssid.ssid_len] = '\0';
		dest_ap++;
		src_hotlist++;
	}
	dest_hotlist->ap_found = ap_found;
	pMac->sme.pExtScanIndCb(pMac->hHdd,
				eSIR_EXTSCAN_HOTLIST_MATCH_IND, dest_hotlist);
	WMA_LOGI("%s: sending hotlist match event to hdd", __func__);
	cdf_mem_free(dest_hotlist);
	return 0;
}

/** wma_extscan_find_unique_scan_ids() - find unique scan ids
 * @cmd_param_info: event data.
 *
 * This utility function parses the input bss table of information
 * and find the unique number of scan ids
 *
 * Return: 0 on success; error number otherwise
 */
static int wma_extscan_find_unique_scan_ids(const u_int8_t *cmd_param_info)
{
	WMI_EXTSCAN_CACHED_RESULTS_EVENTID_param_tlvs *param_buf;
	wmi_extscan_cached_results_event_fixed_param  *event;
	wmi_extscan_wlan_descriptor  *src_hotlist;
	wmi_extscan_rssi_info  *src_rssi;
	int prev_scan_id, scan_ids_cnt, i;

	param_buf = (WMI_EXTSCAN_CACHED_RESULTS_EVENTID_param_tlvs *)
						cmd_param_info;
	event = param_buf->fixed_param;
	src_hotlist = param_buf->bssid_list;
	src_rssi = param_buf->rssi_list;

	/* Find the unique number of scan_id's for grouping */
	prev_scan_id = src_rssi->scan_cycle_id;
	scan_ids_cnt = 1;
	for (i = 1; i < event->num_entries_in_page; i++) {
		src_rssi++;

		if (prev_scan_id != src_rssi->scan_cycle_id) {
			scan_ids_cnt++;
			prev_scan_id = src_rssi->scan_cycle_id;
		}
	}

	return scan_ids_cnt;
}

/** wma_fill_num_results_per_scan_id() - fill number of bss per scan id
 * @cmd_param_info: event data.
 * @scan_id_group: pointer to scan id group.
 *
 * This utility function parses the input bss table of information
 * and finds how many bss are there per unique scan id.
 *
 * Return: 0 on success; error number otherwise
 */
static int wma_fill_num_results_per_scan_id(const u_int8_t *cmd_param_info,
			struct extscan_cached_scan_result *scan_id_group)
{
	WMI_EXTSCAN_CACHED_RESULTS_EVENTID_param_tlvs *param_buf;
	wmi_extscan_cached_results_event_fixed_param  *event;
	wmi_extscan_wlan_descriptor  *src_hotlist;
	wmi_extscan_rssi_info  *src_rssi;
	struct extscan_cached_scan_result *t_scan_id_grp;
	int i, prev_scan_id;

	param_buf = (WMI_EXTSCAN_CACHED_RESULTS_EVENTID_param_tlvs *)
						cmd_param_info;
	event = param_buf->fixed_param;
	src_hotlist = param_buf->bssid_list;
	src_rssi = param_buf->rssi_list;
	t_scan_id_grp = scan_id_group;

	prev_scan_id = src_rssi->scan_cycle_id;

	t_scan_id_grp->scan_id = src_rssi->scan_cycle_id;
	t_scan_id_grp->flags = src_rssi->flags;
	t_scan_id_grp->num_results = 1;
	for (i = 1; i < event->num_entries_in_page; i++) {
		src_rssi++;
		if (prev_scan_id == src_rssi->scan_cycle_id) {
			t_scan_id_grp->num_results++;
		} else {
			t_scan_id_grp++;
			prev_scan_id = t_scan_id_grp->scan_id =
				src_rssi->scan_cycle_id;
			t_scan_id_grp->flags = src_rssi->flags;
			t_scan_id_grp->num_results = 1;
		}
	}
	return 0;
}

/** wma_group_num_bss_to_scan_id() - group bss to scan id table
 * @cmd_param_info: event data.
 * @cached_result: pointer to cached table.
 *
 * This function reads the bss information from the format
 * ------------------------------------------------------------------------
 * | bss info {rssi, channel, ssid, bssid, timestamp} | scan id_1 | flags |
 * | bss info {rssi, channel, ssid, bssid, timestamp} | scan id_2 | flags |
 * ........................................................................
 * | bss info {rssi, channel, ssid, bssid, timestamp} | scan id_N | flags |
 * ------------------------------------------------------------------------
 *
 * and converts it into the below format and store it
 *
 * ------------------------------------------------------------------------
 * | scan id_1 | -> bss info_1 -> bss info_2 -> .... bss info_M1
 * | scan id_2 | -> bss info_1 -> bss info_2 -> .... bss info_M2
 * ......................
 * | scan id_N | -> bss info_1 -> bss info_2 -> .... bss info_Mn
 * ------------------------------------------------------------------------
 *
 * Return: 0 on success; error number otherwise
 */
static int wma_group_num_bss_to_scan_id(const u_int8_t *cmd_param_info,
			struct extscan_cached_scan_results *cached_result)
{
	WMI_EXTSCAN_CACHED_RESULTS_EVENTID_param_tlvs *param_buf;
	wmi_extscan_cached_results_event_fixed_param  *event;
	wmi_extscan_wlan_descriptor  *src_hotlist;
	wmi_extscan_rssi_info  *src_rssi;
	struct extscan_cached_scan_results *t_cached_result;
	struct extscan_cached_scan_result *t_scan_id_grp;
	int i, j;
	tSirWifiScanResult *ap;

	param_buf = (WMI_EXTSCAN_CACHED_RESULTS_EVENTID_param_tlvs *)
						cmd_param_info;
	event = param_buf->fixed_param;
	src_hotlist = param_buf->bssid_list;
	src_rssi = param_buf->rssi_list;
	t_cached_result = cached_result;
	t_scan_id_grp = &t_cached_result->result[0];

	WMA_LOGD("%s: num_scan_ids:%d", __func__,
			t_cached_result->num_scan_ids);
	for (i = 0; i < t_cached_result->num_scan_ids; i++) {
		WMA_LOGD("%s: num_results:%d", __func__,
			t_scan_id_grp->num_results);
		t_scan_id_grp->ap = cdf_mem_malloc(t_scan_id_grp->num_results *
						sizeof(*ap));
		if (!t_scan_id_grp->ap) {
			WMA_LOGD("%s: cdf_mem_malloc failed", __func__);
			return -ENOMEM;
		}

		ap = &t_scan_id_grp->ap[0];
		for (j = 0; j < t_scan_id_grp->num_results; j++) {
			ap->channel = src_hotlist->channel;
			ap->ts = WMA_MSEC_TO_USEC(src_rssi->tstamp);
			ap->rtt = src_hotlist->rtt;
			ap->rtt_sd = src_hotlist->rtt_sd;
			ap->beaconPeriod = src_hotlist->beacon_interval;
			ap->capability = src_hotlist->capabilities;
			ap->ieLength = src_hotlist->ie_length;

			/* Firmware already applied noise floor adjustment and
			 * due to WMI interface "UINT32 rssi", host driver
			 * receives a positive value, hence convert to
			 * signed char to get the absolute rssi.
			 */
			ap->rssi = (signed char) src_rssi->rssi;
			WMI_MAC_ADDR_TO_CHAR_ARRAY(&src_hotlist->bssid,
						   ap->bssid.bytes);

			cdf_mem_copy(ap->ssid, src_hotlist->ssid.ssid,
					src_hotlist->ssid.ssid_len);
			ap->ssid[src_hotlist->ssid.ssid_len] = '\0';
			ap++;
			src_rssi++;
			src_hotlist++;
		}
		t_scan_id_grp++;
	}
	return 0;
}

/**
 * wma_extscan_cached_results_event_handler() - cached results event handler
 * @handle: wma handle
 * @cmd_param_info: event buffer
 * @len: length of @cmd_param_info
 *
 * This function handles cached results event and indicate
 * cached results to upper layer.
 *
 * Return: 0 for success or error code.
 */
int wma_extscan_cached_results_event_handler(void *handle,
					     uint8_t *cmd_param_info,
					     uint32_t len)
{
	WMI_EXTSCAN_CACHED_RESULTS_EVENTID_param_tlvs *param_buf;
	wmi_extscan_cached_results_event_fixed_param *event;
	struct extscan_cached_scan_results *dest_cachelist;
	struct extscan_cached_scan_result *dest_result;
	struct extscan_cached_scan_results empty_cachelist;
	wmi_extscan_wlan_descriptor *src_hotlist;
	wmi_extscan_rssi_info *src_rssi;
	int numap, i, moredata, scan_ids_cnt, buf_len;

	tpAniSirGlobal pMac = cds_get_context(CDF_MODULE_ID_PE);
	if (!pMac) {
		WMA_LOGE("%s: Invalid pMac", __func__);
		return -EINVAL;
	}
	if (!pMac->sme.pExtScanIndCb) {
		WMA_LOGE("%s: Callback not registered", __func__);
		return -EINVAL;
	}
	param_buf = (WMI_EXTSCAN_CACHED_RESULTS_EVENTID_param_tlvs *)
		    cmd_param_info;
	if (!param_buf) {
		WMA_LOGE("%s: Invalid cached results event", __func__);
		return -EINVAL;
	}
	event = param_buf->fixed_param;
	src_hotlist = param_buf->bssid_list;
	src_rssi = param_buf->rssi_list;
	numap = event->num_entries_in_page;
	WMA_LOGI("Total_entries %u first_entry_index %u", event->total_entries,
			event->first_entry_index);
	WMA_LOGI("num_entries_in_page %d", numap);
	if (!src_hotlist || !src_rssi || !numap) {
		WMA_LOGW("%s: Cached results empty, send 0 results", __func__);
		goto noresults;
	}
	if (event->first_entry_index +
	    event->num_entries_in_page < event->total_entries)
		moredata = 1;
	else
		moredata = 0;

	dest_cachelist = cdf_mem_malloc(sizeof(*dest_cachelist));
	if (!dest_cachelist) {
		WMA_LOGE("%s: cdf_mem_malloc failed", __func__);
		return -ENOMEM;
	}
	cdf_mem_zero(dest_cachelist, sizeof(*dest_cachelist));
	dest_cachelist->request_id = event->request_id;
	dest_cachelist->more_data = moredata;

	scan_ids_cnt = wma_extscan_find_unique_scan_ids(cmd_param_info);
	WMA_LOGI("%s: scan_ids_cnt %d", __func__, scan_ids_cnt);
	dest_cachelist->num_scan_ids = scan_ids_cnt;

	buf_len = sizeof(*dest_result) * scan_ids_cnt;
	dest_cachelist->result = cdf_mem_malloc(buf_len);
	if (!dest_cachelist->result) {
		WMA_LOGE("%s: Allocation failed for scanid grouping", __func__);
		cdf_mem_free(dest_cachelist);
		return -ENOMEM;
	}

	dest_result = dest_cachelist->result;
	wma_fill_num_results_per_scan_id(cmd_param_info, dest_result);
	wma_group_num_bss_to_scan_id(cmd_param_info, dest_cachelist);

	pMac->sme.pExtScanIndCb(pMac->hHdd,
				eSIR_EXTSCAN_CACHED_RESULTS_IND,
				dest_cachelist);
	WMA_LOGI("%s: sending cached results event", __func__);
	dest_result = dest_cachelist->result;
	for (i = 0; i < dest_cachelist->num_scan_ids; i++) {
		cdf_mem_free(dest_result->ap);
		dest_result++;
	}
	cdf_mem_free(dest_cachelist->result);
	cdf_mem_free(dest_cachelist);
	return 0;

noresults:
	empty_cachelist.request_id = event->request_id;
	empty_cachelist.more_data = 0;
	empty_cachelist.num_scan_ids = 0;

	pMac->sme.pExtScanIndCb(pMac->hHdd,
				eSIR_EXTSCAN_CACHED_RESULTS_IND,
				&empty_cachelist);
	WMA_LOGI("%s: sending cached results event", __func__);
	return 0;
}

/**
 * wma_extscan_change_results_event_handler() - change results event handler
 * @handle: wma handle
 * @cmd_param_info: event buffer
 * @len: length
 *
 * This function handles change results event and indicate
 * change results to upper layer.
 *
 * Return: 0 for success or error code.
 */
int wma_extscan_change_results_event_handler(void *handle,
					     uint8_t *cmd_param_info,
					     uint32_t len)
{
	WMI_EXTSCAN_WLAN_CHANGE_RESULTS_EVENTID_param_tlvs *param_buf;
	wmi_extscan_wlan_change_results_event_fixed_param *event;
	tSirWifiSignificantChangeEvent *dest_chglist;
	tSirWifiSignificantChange *dest_ap;
	wmi_extscan_wlan_change_result_bssid *src_chglist;

	int numap;
	int i, k;
	uint8_t *src_rssi;
	int count = 0;
	int moredata;
	int rssi_num = 0;
	tpAniSirGlobal pMac = cds_get_context(CDF_MODULE_ID_PE);
	if (!pMac) {
		WMA_LOGE("%s: Invalid pMac", __func__);
		return -EINVAL;
	}
	if (!pMac->sme.pExtScanIndCb) {
		WMA_LOGE("%s: Callback not registered", __func__);
		return -EINVAL;
	}
	param_buf = (WMI_EXTSCAN_WLAN_CHANGE_RESULTS_EVENTID_param_tlvs *)
		    cmd_param_info;
	if (!param_buf) {
		WMA_LOGE("%s: Invalid change monitor event", __func__);
		return -EINVAL;
	}
	event = param_buf->fixed_param;
	src_chglist = param_buf->bssid_signal_descriptor_list;
	src_rssi = param_buf->rssi_list;
	numap = event->num_entries_in_page;

	if (!src_chglist || !numap) {
		WMA_LOGE("%s: Results invalid", __func__);
		return -EINVAL;
	}
	for (i = 0; i < numap; i++) {
		rssi_num += src_chglist->num_rssi_samples;
	}
	if (event->first_entry_index +
	    event->num_entries_in_page < event->total_entries) {
		moredata = 1;
	} else {
		moredata = 0;
	}
	dest_chglist = cdf_mem_malloc(sizeof(*dest_chglist) +
				      sizeof(*dest_ap) * numap +
				      sizeof(int32_t) * rssi_num);
	if (!dest_chglist) {
		WMA_LOGE("%s: Allocation failed for change monitor", __func__);
		return -ENOMEM;
	}
	dest_ap = &dest_chglist->ap[0];
	for (i = 0; i < numap; i++) {
		dest_ap->channel = src_chglist->channel;
		WMI_MAC_ADDR_TO_CHAR_ARRAY(&src_chglist->bssid,
					   dest_ap->bssid.bytes);
		dest_ap->numOfRssi = src_chglist->num_rssi_samples;
		if (dest_ap->numOfRssi) {
			for (k = 0; k < dest_ap->numOfRssi; k++) {
				dest_ap->rssi[k] = WMA_TGT_NOISE_FLOOR_DBM +
						   src_rssi[count++];
			}
		}
		dest_ap += dest_ap->numOfRssi * sizeof(int32_t);
		src_chglist++;
	}
	dest_chglist->requestId = event->request_id;
	dest_chglist->moreData = moredata;
	dest_chglist->numResults = event->total_entries;

	pMac->sme.pExtScanIndCb(pMac->hHdd,
				eSIR_EXTSCAN_SIGNIFICANT_WIFI_CHANGE_RESULTS_IND,
				dest_chglist);
	WMA_LOGI("%s: sending change monitor results", __func__);
	cdf_mem_free(dest_chglist);
	return 0;
}

/**
 * wma_passpoint_match_event_handler() - passpoint match found event handler
 * @handle: WMA handle
 * @cmd_param_info: event data
 * @len: event data length
 *
 * This is the passpoint match found event handler; it reads event data from
 * @cmd_param_info and fill in the destination buffer and sends indication
 * up layer.
 *
 * Return: 0 on success; error number otherwise
 */
int wma_passpoint_match_event_handler(void *handle,
				     uint8_t  *cmd_param_info,
				     uint32_t len)
{
	WMI_PASSPOINT_MATCH_EVENTID_param_tlvs *param_buf;
	wmi_passpoint_event_hdr  *event;
	struct wifi_passpoint_match  *dest_match;
	tSirWifiScanResult      *dest_ap;
	uint8_t *buf_ptr;
	tpAniSirGlobal mac = cds_get_context(CDF_MODULE_ID_PE);

	if (!mac) {
		WMA_LOGE("%s: Invalid mac", __func__);
		return -EINVAL;
	}
	if (!mac->sme.pExtScanIndCb) {
		WMA_LOGE("%s: Callback not registered", __func__);
		return -EINVAL;
	}

	param_buf = (WMI_PASSPOINT_MATCH_EVENTID_param_tlvs *) cmd_param_info;
	if (!param_buf) {
		WMA_LOGE("%s: Invalid passpoint match event", __func__);
		return -EINVAL;
	}
	event = param_buf->fixed_param;
	buf_ptr = (uint8_t *)param_buf->fixed_param;

	dest_match = cdf_mem_malloc(sizeof(*dest_match) +
				event->ie_length + event->anqp_length);
	if (!dest_match) {
		WMA_LOGE("%s: cdf_mem_malloc failed", __func__);
		return -EINVAL;
	}
	dest_ap = &dest_match->ap;
	dest_match->request_id = 0;
	dest_match->id = event->id;
	dest_match->anqp_len = event->anqp_length;
	WMA_LOGI("%s: passpoint match: id: %u anqp length %u", __func__,
		 dest_match->id, dest_match->anqp_len);

	dest_ap->channel = event->channel_mhz;
	dest_ap->ts = event->timestamp;
	dest_ap->rtt = event->rtt;
	dest_ap->rssi = event->rssi;
	dest_ap->rtt_sd = event->rtt_sd;
	dest_ap->beaconPeriod = event->beacon_period;
	dest_ap->capability = event->capability;
	dest_ap->ieLength = event->ie_length;
	WMI_MAC_ADDR_TO_CHAR_ARRAY(&event->bssid, dest_ap->bssid.bytes);
	cdf_mem_copy(dest_ap->ssid, event->ssid.ssid,
				event->ssid.ssid_len);
	dest_ap->ssid[event->ssid.ssid_len] = '\0';
	cdf_mem_copy(dest_ap->ieData, buf_ptr + sizeof(*event) +
			WMI_TLV_HDR_SIZE, dest_ap->ieLength);
	cdf_mem_copy(dest_match->anqp, buf_ptr + sizeof(*event) +
			WMI_TLV_HDR_SIZE + dest_ap->ieLength,
			dest_match->anqp_len);

	mac->sme.pExtScanIndCb(mac->hHdd,
				eSIR_PASSPOINT_NETWORK_FOUND_IND,
				dest_match);
	WMA_LOGI("%s: sending passpoint match event to hdd", __func__);
	cdf_mem_free(dest_match);
	return 0;
}

/**
 * wma_extscan_hotlist_ssid_match_event_handler() -
 *	Handler for SSID hotlist match event from firmware
 * @handle: WMA handle
 * @cmd_param_info: WMI command buffer
 * @len: length of @cmd_param_info
 *
 * Return: 0 on success, non-zero on failure
 */
int
wma_extscan_hotlist_ssid_match_event_handler(void *handle,
					     uint8_t *cmd_param_info,
					     uint32_t len)
{
	WMI_EXTSCAN_HOTLIST_SSID_MATCH_EVENTID_param_tlvs *param_buf;
	wmi_extscan_hotlist_ssid_match_event_fixed_param  *event;
	tSirWifiScanResultEvent  *dest_hotlist;
	tSirWifiScanResult      *dest_ap;
	wmi_extscan_wlan_descriptor    *src_hotlist;
	int numap, j;
	bool ssid_found = false;
	tpAniSirGlobal mac = cds_get_context(CDF_MODULE_ID_PE);

	if (!mac) {
		WMA_LOGE("%s: Invalid mac", __func__);
		return -EINVAL;
	}

	if (!mac->sme.pExtScanIndCb) {
		WMA_LOGE("%s: Callback not registered", __func__);
		return -EINVAL;
	}

	param_buf = (WMI_EXTSCAN_HOTLIST_SSID_MATCH_EVENTID_param_tlvs *)
					cmd_param_info;
	if (!param_buf) {
		WMA_LOGE("%s: Invalid hotlist match event", __func__);
		return -EINVAL;
	}

	event = param_buf->fixed_param;
	src_hotlist = param_buf->hotlist_ssid_match;
	numap = event->total_entries;
	if (!src_hotlist || !numap) {
		WMA_LOGE("%s: Hotlist AP's list invalid", __func__);
		return -EINVAL;
	}

	dest_hotlist = cdf_mem_malloc(sizeof(*dest_hotlist) +
					sizeof(*dest_ap) * numap);
	if (!dest_hotlist) {
		WMA_LOGE("%s: Allocation failed for hotlist buffer",
			__func__);
		return -EINVAL;
	}

	dest_ap = &dest_hotlist->ap[0];
	dest_hotlist->numOfAps = event->total_entries;
	dest_hotlist->requestId = event->config_request_id;

	if (event->first_entry_index +
		event->num_entries_in_page < event->total_entries)
		dest_hotlist->moreData = 1;
	else
		dest_hotlist->moreData = 0;

	WMA_LOGD("%s: Hotlist match: requestId: %u,numOfAps: %d", __func__,
		 dest_hotlist->requestId, dest_hotlist->numOfAps);

	for (j = 0; j < numap; j++) {
		dest_ap->channel = src_hotlist->channel;
		dest_ap->ts = src_hotlist->tstamp;
		ssid_found = src_hotlist->flags & WMI_HOTLIST_FLAG_PRESENCE;
		dest_ap->rtt = src_hotlist->rtt;
		dest_ap->rtt_sd = src_hotlist->rtt_sd;
		dest_ap->beaconPeriod = src_hotlist->beacon_interval;
		dest_ap->capability = src_hotlist->capabilities;
		dest_ap->ieLength = src_hotlist->ie_length;
		WMI_MAC_ADDR_TO_CHAR_ARRAY(&src_hotlist->bssid,
					   dest_ap->bssid.bytes);
		cdf_mem_copy(dest_ap->ssid, src_hotlist->ssid.ssid,
					src_hotlist->ssid.ssid_len);
		dest_ap->ssid[src_hotlist->ssid.ssid_len] = '\0';
		dest_ap++;
		src_hotlist++;
	}

	dest_hotlist->ap_found = ssid_found;
	mac->sme.pExtScanIndCb(mac->hHdd,
			       eSIR_EXTSCAN_HOTLIST_SSID_MATCH_IND,
			       dest_hotlist);
	WMA_LOGI("%s: sending hotlist ssid match event", __func__);
	cdf_mem_free(dest_hotlist);
	return 0;
}

/**
 * wma_get_buf_extscan_start_cmd() - Fill extscan start request
 * @handle: wma handle
 * @pstart: scan command request params
 * @buf: event buffer
 * @buf_len: length of buffer
 *
 * This function fills individual elements of extscan request and
 * TLV for buckets, channel list.
 *
 * Return: CDF Status.
 */
CDF_STATUS wma_get_buf_extscan_start_cmd(tp_wma_handle wma_handle,
					 tSirWifiScanCmdReqParams *pstart,
					 wmi_buf_t *buf, int *buf_len)
{
	wmi_extscan_start_cmd_fixed_param *cmd;
	wmi_extscan_bucket *dest_blist;
	wmi_extscan_bucket_channel *dest_clist;
	tSirWifiScanBucketSpec *src_bucket = pstart->buckets;
	tSirWifiScanChannelSpec *src_channel = src_bucket->channels;
	tSirWifiScanChannelSpec save_channel[WLAN_EXTSCAN_MAX_CHANNELS];

	uint8_t *buf_ptr;
	int i, k, count = 0;
	int len = sizeof(*cmd);
	int nbuckets = pstart->numBuckets;
	int nchannels = 0;

	/* These TLV's are are NULL by default */
	uint32_t ie_len_with_pad = 0;
	int num_ssid = 0;
	int num_bssid = 0;
	int ie_len = 0;

	uint32_t base_period = pstart->basePeriod;

	/* TLV placeholder for ssid_list (NULL) */
	len += WMI_TLV_HDR_SIZE;
	len += num_ssid * sizeof(wmi_ssid);

	/* TLV placeholder for bssid_list (NULL) */
	len += WMI_TLV_HDR_SIZE;
	len += num_bssid * sizeof(wmi_mac_addr);

	/* TLV placeholder for ie_data (NULL) */
	len += WMI_TLV_HDR_SIZE;
	len += ie_len * sizeof(uint32_t);

	/* TLV placeholder for bucket */
	len += WMI_TLV_HDR_SIZE;
	len += nbuckets * sizeof(wmi_extscan_bucket);

	/* TLV channel placeholder */
	len += WMI_TLV_HDR_SIZE;
	for (i = 0; i < nbuckets; i++) {
		nchannels += src_bucket->numChannels;
		src_bucket++;
	}

	WMA_LOGD("%s: Total buckets: %d total #of channels is %d",
		__func__, nbuckets, nchannels);
	len += nchannels * sizeof(wmi_extscan_bucket_channel);
	/* Allocate the memory */
	*buf = wmi_buf_alloc(wma_handle->wmi_handle, len);
	if (!*buf) {
		WMA_LOGP("%s: failed to allocate memory"
			 " for start extscan cmd", __func__);
		return CDF_STATUS_E_NOMEM;
	}
	buf_ptr = (uint8_t *) wmi_buf_data(*buf);
	cmd = (wmi_extscan_start_cmd_fixed_param *) buf_ptr;
	WMITLV_SET_HDR(&cmd->tlv_header,
		       WMITLV_TAG_STRUC_wmi_extscan_start_cmd_fixed_param,
		       WMITLV_GET_STRUCT_TLVLEN
			       (wmi_extscan_start_cmd_fixed_param));

	cmd->request_id = pstart->requestId;
	cmd->vdev_id = pstart->sessionId;
	cmd->base_period = pstart->basePeriod;
	cmd->num_buckets = nbuckets;
	cmd->configuration_flags = 0;
	if (pstart->configuration_flags & EXTSCAN_LP_EXTENDED_BATCHING)
		cmd->configuration_flags |= WMI_EXTSCAN_EXTENDED_BATCHING_EN;
	WMA_LOGI("%s: configuration_flags: 0x%x", __func__,
			cmd->configuration_flags);

	cmd->min_rest_time = WMA_EXTSCAN_REST_TIME;
	cmd->max_rest_time = WMA_EXTSCAN_REST_TIME;
	cmd->max_bssids_per_scan_cycle = pstart->maxAPperScan;

	/* The max dwell time is retrieved from the first channel
	 * of the first bucket and kept common for all channels.
	 */
	cmd->min_dwell_time_active = pstart->min_dwell_time_active;
	cmd->max_dwell_time_active = pstart->max_dwell_time_active;
	cmd->min_dwell_time_passive = pstart->min_dwell_time_passive;
	cmd->max_dwell_time_passive = pstart->max_dwell_time_passive;
	cmd->max_bssids_per_scan_cycle = pstart->maxAPperScan;
	cmd->max_table_usage = pstart->report_threshold_percent;
	cmd->report_threshold_num_scans = pstart->report_threshold_num_scans;

	cmd->repeat_probe_time = cmd->max_dwell_time_active /
					WMA_SCAN_NPROBES_DEFAULT;
	cmd->max_scan_time = WMA_EXTSCAN_MAX_SCAN_TIME;
	cmd->probe_delay = 0;
	cmd->probe_spacing_time = 0;
	cmd->idle_time = 0;
	cmd->burst_duration = WMA_EXTSCAN_BURST_DURATION;
	cmd->scan_ctrl_flags = WMI_SCAN_ADD_BCAST_PROBE_REQ |
			       WMI_SCAN_ADD_CCK_RATES |
			       WMI_SCAN_ADD_OFDM_RATES |
			       WMI_SCAN_ADD_SPOOFED_MAC_IN_PROBE_REQ |
			       WMI_SCAN_ADD_DS_IE_IN_PROBE_REQ;
	cmd->scan_priority = WMI_SCAN_PRIORITY_HIGH;
	cmd->num_ssids = 0;
	cmd->num_bssid = 0;
	cmd->ie_len = 0;
	cmd->n_probes = (cmd->repeat_probe_time > 0) ?
			cmd->max_dwell_time_active / cmd->repeat_probe_time : 0;

	buf_ptr += sizeof(*cmd);
	WMITLV_SET_HDR(buf_ptr,
		       WMITLV_TAG_ARRAY_FIXED_STRUC,
		       num_ssid * sizeof(wmi_ssid));
	buf_ptr += WMI_TLV_HDR_SIZE + (num_ssid * sizeof(wmi_ssid));

	WMITLV_SET_HDR(buf_ptr,
		       WMITLV_TAG_ARRAY_FIXED_STRUC,
		       num_bssid * sizeof(wmi_mac_addr));
	buf_ptr += WMI_TLV_HDR_SIZE + (num_bssid * sizeof(wmi_mac_addr));

	ie_len_with_pad = 0;
	WMITLV_SET_HDR(buf_ptr, WMITLV_TAG_ARRAY_BYTE, ie_len_with_pad);
	buf_ptr += WMI_TLV_HDR_SIZE + ie_len_with_pad;

	WMITLV_SET_HDR(buf_ptr,
		       WMITLV_TAG_ARRAY_STRUC,
		       nbuckets * sizeof(wmi_extscan_bucket));
	dest_blist = (wmi_extscan_bucket *)
		     (buf_ptr + WMI_TLV_HDR_SIZE);
	src_bucket = pstart->buckets;

	/* Retrieve scanning information from each bucket and
	 * channels and send it to the target
	 */
	for (i = 0; i < nbuckets; i++) {
		WMITLV_SET_HDR(dest_blist,
			       WMITLV_TAG_STRUC_wmi_extscan_bucket_cmd_fixed_param,
			       WMITLV_GET_STRUCT_TLVLEN(wmi_extscan_bucket));

		dest_blist->bucket_id = src_bucket->bucket;
		dest_blist->base_period_multiplier =
			src_bucket->period / base_period;
		dest_blist->min_period = src_bucket->period;
		dest_blist->max_period = src_bucket->max_period;
		dest_blist->exp_backoff = src_bucket->exponent;
		dest_blist->exp_max_step_count = src_bucket->step_count;
		dest_blist->channel_band = src_bucket->band;
		dest_blist->num_channels = src_bucket->numChannels;
		dest_blist->notify_extscan_events = 0;

		if (src_bucket->reportEvents & EXTSCAN_REPORT_EVENTS_EACH_SCAN)
			dest_blist->notify_extscan_events =
					WMI_EXTSCAN_BUCKET_COMPLETED_EVENT;

		if (src_bucket->reportEvents &
				EXTSCAN_REPORT_EVENTS_FULL_RESULTS) {
			dest_blist->forwarding_flags =
				WMI_EXTSCAN_FORWARD_FRAME_TO_HOST;
			dest_blist->notify_extscan_events |=
				WMI_EXTSCAN_BUCKET_COMPLETED_EVENT |
				WMI_EXTSCAN_CYCLE_STARTED_EVENT |
				WMI_EXTSCAN_CYCLE_COMPLETED_EVENT;
		} else {
			dest_blist->forwarding_flags =
				WMI_EXTSCAN_NO_FORWARDING;
		}

		if (src_bucket->reportEvents & EXTSCAN_REPORT_EVENTS_NO_BATCH)
			dest_blist->configuration_flags = 0;
		else
			dest_blist->configuration_flags =
				WMI_EXTSCAN_BUCKET_CACHE_RESULTS;

		WMA_LOGI("%s: ntfy_extscan_events:%u cfg_flags:%u fwd_flags:%u",
			__func__, dest_blist->notify_extscan_events,
			dest_blist->configuration_flags,
			dest_blist->forwarding_flags);

		dest_blist->min_dwell_time_active = src_bucket->min_dwell_time_active;
		dest_blist->max_dwell_time_active = src_bucket->max_dwell_time_active;
		dest_blist->min_dwell_time_passive = src_bucket->min_dwell_time_passive;
		dest_blist->max_dwell_time_passive = src_bucket->max_dwell_time_passive;
		src_channel = src_bucket->channels;

		/* save the channel info to later populate
		 * the  channel TLV
		 */
		for (k = 0; k < src_bucket->numChannels; k++) {
			save_channel[count++].channel = src_channel->channel;
			src_channel++;
		}
		dest_blist++;
		src_bucket++;
	}
	buf_ptr += WMI_TLV_HDR_SIZE + (nbuckets * sizeof(wmi_extscan_bucket));
	WMITLV_SET_HDR(buf_ptr,
		       WMITLV_TAG_ARRAY_STRUC,
		       nchannels * sizeof(wmi_extscan_bucket_channel));
	dest_clist = (wmi_extscan_bucket_channel *)
		     (buf_ptr + WMI_TLV_HDR_SIZE);

	/* Active or passive scan is based on the bucket dwell time
	 * and channel specific active,passive scans are not
	 * supported yet
	 */
	for (i = 0; i < nchannels; i++) {
		WMITLV_SET_HDR(dest_clist,
			       WMITLV_TAG_STRUC_wmi_extscan_bucket_channel_event_fixed_param,
			       WMITLV_GET_STRUCT_TLVLEN
				       (wmi_extscan_bucket_channel));
		dest_clist->channel = save_channel[i].channel;
		dest_clist++;
	}
	buf_ptr += WMI_TLV_HDR_SIZE +
		   (nchannels * sizeof(wmi_extscan_bucket_channel));
	*buf_len = len;
	return CDF_STATUS_SUCCESS;
}

/**
 * wma_start_extscan() - start extscan command to fw.
 * @handle: wma handle
 * @pstart: scan command request params
 *
 * This function sends start extscan request to fw.
 *
 * Return: CDF Status.
 */
CDF_STATUS wma_start_extscan(tp_wma_handle wma,
			     tSirWifiScanCmdReqParams *pstart)
{
	CDF_STATUS cdf_status = CDF_STATUS_SUCCESS;
	wmi_buf_t buf;
	int len;
	if (!wma || !wma->wmi_handle) {
		WMA_LOGE("%s: WMA is closed,can not issue extscan cmd",
			 __func__);
		return CDF_STATUS_E_INVAL;
	}
	if (!WMI_SERVICE_IS_ENABLED(wma->wmi_service_bitmap,
				    WMI_SERVICE_EXTSCAN)) {
		WMA_LOGE("%s: extscan feature bit not enabled", __func__);
		return CDF_STATUS_E_FAILURE;
	}
	/* Fill individual elements of extscan request and
	 * TLV for buckets, channel list.
	 */
	cdf_status = wma_get_buf_extscan_start_cmd(wma, pstart, &buf, &len);
	if (cdf_status != CDF_STATUS_SUCCESS) {
		WMA_LOGE("%s: Failed to get buffer for ext scan cmd", __func__);
		return CDF_STATUS_E_FAILURE;
	}
	if (!buf) {
		WMA_LOGE("%s:Failed to get buffer"
			 "for current extscan info", __func__);
		return CDF_STATUS_E_FAILURE;
	}
	if (wmi_unified_cmd_send(wma->wmi_handle, buf,
				 len, WMI_EXTSCAN_START_CMDID)) {
		WMA_LOGE("%s: failed to send command", __func__);
		cdf_nbuf_free(buf);
		return CDF_STATUS_E_FAILURE;
	}
	wma->interfaces[pstart->sessionId].extscan_in_progress = true;
	WMA_LOGD("Extscan start request sent successfully for vdev %d",
		 pstart->sessionId);

	return CDF_STATUS_SUCCESS;
}

/**
 * wma_stop_extscan() - stop extscan command to fw.
 * @handle: wma handle
 * @pstopcmd: stop scan command request params
 *
 * This function sends stop extscan request to fw.
 *
 * Return: CDF Status.
 */
CDF_STATUS wma_stop_extscan(tp_wma_handle wma,
			    tSirExtScanStopReqParams *pstopcmd)
{
	wmi_extscan_stop_cmd_fixed_param *cmd;
	wmi_buf_t wmi_buf;
	uint32_t len;
	uint8_t *buf_ptr;

	if (!wma || !wma->wmi_handle) {
		WMA_LOGE("%s: WMA is closed, cannot issue cmd", __func__);
		return CDF_STATUS_E_INVAL;
	}
	if (!WMI_SERVICE_IS_ENABLED(wma->wmi_service_bitmap,
				    WMI_SERVICE_EXTSCAN)) {
		WMA_LOGE("%s: extscan not enabled", __func__);
		return CDF_STATUS_E_FAILURE;
	}
	len = sizeof(*cmd);
	wmi_buf = wmi_buf_alloc(wma->wmi_handle, len);
	if (!wmi_buf) {
		WMA_LOGE("%s: wmi_buf_alloc failed", __func__);
		return CDF_STATUS_E_NOMEM;
	}
	buf_ptr = (uint8_t *) wmi_buf_data(wmi_buf);
	cmd = (wmi_extscan_stop_cmd_fixed_param *) buf_ptr;
	WMITLV_SET_HDR(&cmd->tlv_header,
		       WMITLV_TAG_STRUC_wmi_extscan_stop_cmd_fixed_param,
		       WMITLV_GET_STRUCT_TLVLEN
			       (wmi_extscan_stop_cmd_fixed_param));

	cmd->request_id = pstopcmd->requestId;
	cmd->vdev_id = pstopcmd->sessionId;

	if (wmi_unified_cmd_send(wma->wmi_handle, wmi_buf, len,
				 WMI_EXTSCAN_STOP_CMDID)) {
		WMA_LOGE("%s: failed to  command", __func__);
		cdf_nbuf_free(wmi_buf);
		return CDF_STATUS_E_FAILURE;
	}
	wma->interfaces[pstopcmd->sessionId].extscan_in_progress = false;
	WMA_LOGD("Extscan stop request sent successfully for vdev %d",
		 pstopcmd->sessionId);

	return CDF_STATUS_SUCCESS;
}

/** wma_get_hotlist_entries_per_page() - hotlist entries per page
 * @wmi_handle: wmi handle.
 * @cmd: size of command structure.
 * @per_entry_size: per entry size.
 *
 * This utility function calculates how many hotlist entries can
 * fit in one page.
 *
 * Return: number of entries
 */
static inline int wma_get_hotlist_entries_per_page(wmi_unified_t wmi_handle,
						   size_t cmd_size,
						   size_t per_entry_size)
{
	uint32_t avail_space = 0;
	int num_entries = 0;
	uint16_t max_msg_len = wmi_get_max_msg_len(wmi_handle);

	/* Calculate number of hotlist entries that can
	 * be passed in wma message request.
	 */
	avail_space = max_msg_len - cmd_size;
	num_entries = avail_space / per_entry_size;
	return num_entries;
}

/**
 * wma_get_buf_extscan_hotlist_cmd() - prepare hotlist command
 * @handle: wma handle
 * @photlist: hotlist command params
 * @buf_len: buffer length
 *
 * This function fills individual elements for  hotlist request and
 * TLV for bssid entries
 *
 * Return: CDF Status.
 */
CDF_STATUS wma_get_buf_extscan_hotlist_cmd(tp_wma_handle wma_handle,
					   tSirExtScanSetBssidHotListReqParams *
					   photlist, int *buf_len)
{
	wmi_extscan_configure_hotlist_monitor_cmd_fixed_param *cmd = NULL;
	wmi_extscan_hotlist_entry *dest_hotlist;
	tSirAPThresholdParam *src_ap = photlist->ap;
	wmi_buf_t buf;
	uint8_t *buf_ptr;

	int j, index = 0;
	int cmd_len = 0;
	int num_entries = 0;
	int min_entries = 0;
	int numap = photlist->numAp;
	int len = sizeof(*cmd);

	len += WMI_TLV_HDR_SIZE;
	cmd_len = len;

	/* setbssid hotlist expects the bssid list
	 * to be non zero value
	 */
	if (!numap) {
		WMA_LOGE("%s: Invalid number of bssid's", __func__);
		return CDF_STATUS_E_INVAL;
	}
	num_entries = wma_get_hotlist_entries_per_page(wma_handle->wmi_handle,
							cmd_len,
							sizeof(*dest_hotlist));

	/* Split the hot list entry pages and send multiple command
	 * requests if the buffer reaches the maximum request size
	 */
	while (index < numap) {
		min_entries = CDF_MIN(num_entries, numap);
		len += min_entries * sizeof(wmi_extscan_hotlist_entry);
		buf = wmi_buf_alloc(wma_handle->wmi_handle, len);
		if (!buf) {
			WMA_LOGP("%s: wmi_buf_alloc failed", __func__);
			return CDF_STATUS_E_FAILURE;
		}
		buf_ptr = (uint8_t *) wmi_buf_data(buf);
		cmd = (wmi_extscan_configure_hotlist_monitor_cmd_fixed_param *)
		      buf_ptr;
		WMITLV_SET_HDR(&cmd->tlv_header,
			       WMITLV_TAG_STRUC_wmi_extscan_configure_hotlist_monitor_cmd_fixed_param,
			       WMITLV_GET_STRUCT_TLVLEN
				       (wmi_extscan_configure_hotlist_monitor_cmd_fixed_param));

		/* Multiple requests are sent until the num_entries_in_page
		 * matches the total_entries
		 */
		cmd->request_id = photlist->requestId;
		cmd->vdev_id = photlist->sessionId;
		cmd->total_entries = numap;
		cmd->mode = 1;
		cmd->num_entries_in_page = min_entries;
		cmd->lost_ap_scan_count = photlist->lost_ap_sample_size;
		cmd->first_entry_index = index;

		WMA_LOGD("%s: vdev id:%d total_entries: %d num_entries: %d lost_ap_sample_size: %d",
			__func__, cmd->vdev_id, cmd->total_entries,
			cmd->num_entries_in_page,
			cmd->lost_ap_scan_count);

		buf_ptr += sizeof(*cmd);
		WMITLV_SET_HDR(buf_ptr,
			       WMITLV_TAG_ARRAY_STRUC,
			       min_entries * sizeof(wmi_extscan_hotlist_entry));
		dest_hotlist = (wmi_extscan_hotlist_entry *)
			       (buf_ptr + WMI_TLV_HDR_SIZE);

		/* Populate bssid, channel info and rssi
		 * for the bssid's that are sent as hotlists.
		 */
		for (j = 0; j < min_entries; j++) {
			WMITLV_SET_HDR(dest_hotlist,
				       WMITLV_TAG_STRUC_wmi_extscan_bucket_cmd_fixed_param,
				       WMITLV_GET_STRUCT_TLVLEN
					       (wmi_extscan_hotlist_entry));

			dest_hotlist->min_rssi = src_ap->low;
			WMI_CHAR_ARRAY_TO_MAC_ADDR(src_ap->bssid.bytes,
						   &dest_hotlist->bssid);

			WMA_LOGD("%s:channel:%d min_rssi %d",
				 __func__, dest_hotlist->channel,
				 dest_hotlist->min_rssi);
			WMA_LOGD
				("%s: bssid mac_addr31to0: 0x%x, mac_addr47to32: 0x%x",
				__func__, dest_hotlist->bssid.mac_addr31to0,
				dest_hotlist->bssid.mac_addr47to32);
			dest_hotlist++;
			src_ap++;
		}
		buf_ptr += WMI_TLV_HDR_SIZE +
			   (min_entries * sizeof(wmi_extscan_hotlist_entry));

		if (wmi_unified_cmd_send(wma_handle->wmi_handle, buf, len,
					 WMI_EXTSCAN_CONFIGURE_HOTLIST_MONITOR_CMDID)) {
			WMA_LOGE("%s: failed to send command", __func__);
			cdf_nbuf_free(buf);
			return CDF_STATUS_E_FAILURE;
		}
		index = index + min_entries;
		num_entries = numap - min_entries;
		len = cmd_len;
	}
	return CDF_STATUS_SUCCESS;
}

/**
 * wma_extscan_start_hotlist_monitor() - start hotlist monitor
 * @wma: wma handle
 * @photlist: hotlist request params
 *
 * This function configures hotlist monitor in fw.
 *
 * Return: CDF status
 */
CDF_STATUS wma_extscan_start_hotlist_monitor(tp_wma_handle wma,
					     tSirExtScanSetBssidHotListReqParams
					     *photlist)
{
	CDF_STATUS cdf_status = CDF_STATUS_SUCCESS;
	int len;

	if (!wma || !wma->wmi_handle) {
		WMA_LOGE("%s: WMA is closed, can not issue hotlist cmd",
			 __func__);
		return CDF_STATUS_E_INVAL;
	}
	/* Fill individual elements for  hotlist request and
	 * TLV for bssid entries
	 */
	cdf_status = wma_get_buf_extscan_hotlist_cmd(wma, photlist, &len);
	if (cdf_status != CDF_STATUS_SUCCESS) {
		WMA_LOGE("%s: Failed to get buffer"
			 "for hotlist scan cmd", __func__);
		return CDF_STATUS_E_FAILURE;
	}
	return CDF_STATUS_SUCCESS;
}

/**
 * wma_extscan_stop_hotlist_monitor() - stop hotlist monitor
 * @wma: wma handle
 * @photlist_reset: hotlist reset params
 *
 * This function configures hotlist monitor to stop in fw.
 *
 * Return: CDF status
 */
CDF_STATUS wma_extscan_stop_hotlist_monitor(tp_wma_handle wma,
					    tSirExtScanResetBssidHotlistReqParams
					    *photlist_reset)
{
	wmi_extscan_configure_hotlist_monitor_cmd_fixed_param *cmd;
	wmi_buf_t wmi_buf;
	uint32_t len;
	uint8_t *buf_ptr;
	int hotlist_entries = 0;

	if (!wma || !wma->wmi_handle) {
		WMA_LOGE("%s: WMA is closed, can not issue  cmd", __func__);
		return CDF_STATUS_E_INVAL;
	}
	if (!photlist_reset) {
		WMA_LOGE("%s: Invalid reset hotlist buffer", __func__);
		return CDF_STATUS_E_INVAL;
	}
	if (!WMI_SERVICE_IS_ENABLED(wma->wmi_service_bitmap,
				    WMI_SERVICE_EXTSCAN)) {
		WMA_LOGE("%s: extscan not enabled", __func__);
		return CDF_STATUS_E_FAILURE;
	}
	len = sizeof(*cmd);

	/* reset bssid hotlist with tlv set to 0 */
	len += WMI_TLV_HDR_SIZE;
	len += hotlist_entries * sizeof(wmi_extscan_hotlist_entry);

	wmi_buf = wmi_buf_alloc(wma->wmi_handle, len);
	if (!wmi_buf) {
		WMA_LOGE("%s: wmi_buf_alloc failed", __func__);
		return CDF_STATUS_E_NOMEM;
	}

	buf_ptr = (uint8_t *) wmi_buf_data(wmi_buf);
	cmd = (wmi_extscan_configure_hotlist_monitor_cmd_fixed_param *)
	      buf_ptr;
	WMITLV_SET_HDR(&cmd->tlv_header,
		       WMITLV_TAG_STRUC_wmi_extscan_configure_hotlist_monitor_cmd_fixed_param,
		       WMITLV_GET_STRUCT_TLVLEN
			       (wmi_extscan_configure_hotlist_monitor_cmd_fixed_param));

	cmd->request_id = photlist_reset->requestId;
	cmd->vdev_id = photlist_reset->sessionId;
	cmd->mode = 0;

	buf_ptr += sizeof(*cmd);
	WMITLV_SET_HDR(buf_ptr,
		       WMITLV_TAG_ARRAY_STRUC,
		       hotlist_entries * sizeof(wmi_extscan_hotlist_entry));
	buf_ptr += WMI_TLV_HDR_SIZE +
		   (hotlist_entries * sizeof(wmi_extscan_hotlist_entry));

	if (wmi_unified_cmd_send(wma->wmi_handle, wmi_buf, len,
				 WMI_EXTSCAN_CONFIGURE_HOTLIST_MONITOR_CMDID)) {
		WMA_LOGE("%s: failed to  command", __func__);
		cdf_nbuf_free(wmi_buf);
		return CDF_STATUS_E_FAILURE;
	}
	return CDF_STATUS_SUCCESS;
}

/**
 * wma_get_buf_extscan_change_monitor_cmd() - fill change monitor request
 * @wma: wma handle
 * @psigchange: change monitor request params
 * @buf: wmi buffer
 * @buf_len: buffer length
 *
 * This function fills elements of change monitor request buffer.
 *
 * Return: CDF status
 */
CDF_STATUS wma_get_buf_extscan_change_monitor_cmd(tp_wma_handle wma_handle,
						  tSirExtScanSetSigChangeReqParams
						  *psigchange, wmi_buf_t *buf,
						  int *buf_len)
{
	wmi_extscan_configure_wlan_change_monitor_cmd_fixed_param *cmd;
	wmi_extscan_wlan_change_bssid_param *dest_chglist;
	uint8_t *buf_ptr;
	int j;
	int len = sizeof(*cmd);
	int numap = psigchange->numAp;
	tSirAPThresholdParam *src_ap = psigchange->ap;

	if (!numap) {
		WMA_LOGE("%s: Invalid number of bssid's", __func__);
		return CDF_STATUS_E_INVAL;
	}
	len += WMI_TLV_HDR_SIZE;
	len += numap * sizeof(wmi_extscan_wlan_change_bssid_param);

	*buf = wmi_buf_alloc(wma_handle->wmi_handle, len);
	if (!*buf) {
		WMA_LOGP("%s: failed to allocate memory for change monitor cmd",
			 __func__);
		return CDF_STATUS_E_FAILURE;
	}
	buf_ptr = (uint8_t *) wmi_buf_data(*buf);
	cmd =
		(wmi_extscan_configure_wlan_change_monitor_cmd_fixed_param *)
		buf_ptr;
	WMITLV_SET_HDR(&cmd->tlv_header,
		       WMITLV_TAG_STRUC_wmi_extscan_configure_wlan_change_monitor_cmd_fixed_param,
		       WMITLV_GET_STRUCT_TLVLEN
			       (wmi_extscan_configure_wlan_change_monitor_cmd_fixed_param));

	cmd->request_id = psigchange->requestId;
	cmd->vdev_id = psigchange->sessionId;
	cmd->total_entries = numap;
	cmd->mode = 1;
	cmd->num_entries_in_page = numap;
	cmd->lost_ap_scan_count = psigchange->lostApSampleSize;
	cmd->max_rssi_samples = psigchange->rssiSampleSize;
	cmd->rssi_averaging_samples = psigchange->rssiSampleSize;
	cmd->max_out_of_range_count = psigchange->minBreaching;

	buf_ptr += sizeof(*cmd);
	WMITLV_SET_HDR(buf_ptr,
		       WMITLV_TAG_ARRAY_STRUC,
		       numap * sizeof(wmi_extscan_wlan_change_bssid_param));
	dest_chglist = (wmi_extscan_wlan_change_bssid_param *)
		       (buf_ptr + WMI_TLV_HDR_SIZE);

	for (j = 0; j < numap; j++) {
		WMITLV_SET_HDR(dest_chglist,
			       WMITLV_TAG_STRUC_wmi_extscan_bucket_cmd_fixed_param,
			       WMITLV_GET_STRUCT_TLVLEN
				       (wmi_extscan_wlan_change_bssid_param));

		dest_chglist->lower_rssi_limit = src_ap->low;
		dest_chglist->upper_rssi_limit = src_ap->high;
		WMI_CHAR_ARRAY_TO_MAC_ADDR(src_ap->bssid.bytes,
					   &dest_chglist->bssid);

		WMA_LOGD("%s: min_rssi %d", __func__,
			 dest_chglist->lower_rssi_limit);
		dest_chglist++;
		src_ap++;
	}
	buf_ptr += WMI_TLV_HDR_SIZE +
		   (numap * sizeof(wmi_extscan_wlan_change_bssid_param));
	*buf_len = len;
	return CDF_STATUS_SUCCESS;
}

/**
 * wma_extscan_start_change_monitor() - send start change monitor cmd
 * @wma: wma handle
 * @psigchange: change monitor request params
 *
 * This function sends start change monitor request to fw.
 *
 * Return: CDF status
 */
CDF_STATUS wma_extscan_start_change_monitor(tp_wma_handle wma,
					    tSirExtScanSetSigChangeReqParams *
					    psigchange)
{
	CDF_STATUS cdf_status = CDF_STATUS_SUCCESS;
	wmi_buf_t buf;
	int len;

	if (!wma || !wma->wmi_handle) {
		WMA_LOGE("%s: WMA is closed,can not issue extscan cmd",
			 __func__);
		return CDF_STATUS_E_INVAL;
	}
	/* Fill individual elements of change monitor and
	 * TLV info ... */

	cdf_status = wma_get_buf_extscan_change_monitor_cmd(wma,
							    psigchange, &buf,
							    &len);
	if (cdf_status != CDF_STATUS_SUCCESS) {
		WMA_LOGE("%s: Failed to get buffer for change monitor cmd",
			 __func__);
		return CDF_STATUS_E_FAILURE;
	}
	if (!buf) {
		WMA_LOGE("%s: Failed to get buffer", __func__);
		return CDF_STATUS_E_FAILURE;
	}
	if (wmi_unified_cmd_send(wma->wmi_handle, buf, len,
				 WMI_EXTSCAN_CONFIGURE_WLAN_CHANGE_MONITOR_CMDID)) {
		WMA_LOGE("%s: failed to send command", __func__);
		cdf_nbuf_free(buf);
		return CDF_STATUS_E_FAILURE;
	}
	return CDF_STATUS_SUCCESS;
}

/**
 * wma_extscan_stop_change_monitor() - send stop change monitor cmd
 * @wma: wma handle
 * @pResetReq: Reset change request params
 *
 * This function sends stop change monitor request to fw.
 *
 * Return: CDF status
 */
CDF_STATUS wma_extscan_stop_change_monitor(tp_wma_handle wma,
					   tSirExtScanResetSignificantChangeReqParams
					   *pResetReq)
{
	wmi_extscan_configure_wlan_change_monitor_cmd_fixed_param *cmd;
	wmi_buf_t wmi_buf;
	uint32_t len;
	uint8_t *buf_ptr;
	int change_list = 0;

	if (!wma || !wma->wmi_handle) {
		WMA_LOGE("%s: WMA is closed, can not issue  cmd", __func__);
		return CDF_STATUS_E_INVAL;
	}
	if (!WMI_SERVICE_IS_ENABLED(wma->wmi_service_bitmap,
				    WMI_SERVICE_EXTSCAN)) {
		WMA_LOGE("%s: ext scan not enabled", __func__);
		return CDF_STATUS_E_FAILURE;
	}
	len = sizeof(*cmd);

	/* reset significant change tlv is set to 0 */
	len += WMI_TLV_HDR_SIZE;
	len += change_list * sizeof(wmi_extscan_wlan_change_bssid_param);
	wmi_buf = wmi_buf_alloc(wma->wmi_handle, len);
	if (!wmi_buf) {
		WMA_LOGE("%s: wmi_buf_alloc failed", __func__);
		return CDF_STATUS_E_NOMEM;
	}
	buf_ptr = (uint8_t *) wmi_buf_data(wmi_buf);

	cmd = (wmi_extscan_configure_wlan_change_monitor_cmd_fixed_param *)
		buf_ptr;
	WMITLV_SET_HDR(&cmd->tlv_header,
		       WMITLV_TAG_STRUC_wmi_extscan_configure_wlan_change_monitor_cmd_fixed_param,
		       WMITLV_GET_STRUCT_TLVLEN
			       (wmi_extscan_configure_wlan_change_monitor_cmd_fixed_param));

	cmd->request_id = pResetReq->requestId;
	cmd->vdev_id = pResetReq->sessionId;
	cmd->mode = 0;

	buf_ptr += sizeof(*cmd);
	WMITLV_SET_HDR(buf_ptr,
		       WMITLV_TAG_ARRAY_STRUC,
		       change_list *
		       sizeof(wmi_extscan_wlan_change_bssid_param));
	buf_ptr += WMI_TLV_HDR_SIZE + (change_list *
				       sizeof
				       (wmi_extscan_wlan_change_bssid_param));

	if (wmi_unified_cmd_send(wma->wmi_handle, wmi_buf, len,
				 WMI_EXTSCAN_CONFIGURE_WLAN_CHANGE_MONITOR_CMDID)) {
		WMA_LOGE("%s: failed to  command", __func__);
		cdf_nbuf_free(wmi_buf);
		return CDF_STATUS_E_FAILURE;
	}
	return CDF_STATUS_SUCCESS;
}

/**
 * wma_extscan_get_cached_results() - extscan get cached results
 * @wma: wma handle
 * @pcached_results: cached results parameters
 *
 * This function send request to fw to get cached results.
 *
 * Return: CDF status
 */
CDF_STATUS wma_extscan_get_cached_results(tp_wma_handle wma,
					  tSirExtScanGetCachedResultsReqParams *
					  pcached_results)
{
	wmi_extscan_get_cached_results_cmd_fixed_param *cmd;
	wmi_buf_t wmi_buf;
	uint32_t len;
	uint8_t *buf_ptr;

	if (!wma || !wma->wmi_handle) {
		WMA_LOGE("%s: WMA is closed, cannot issue cmd", __func__);
		return CDF_STATUS_E_INVAL;
	}
	if (!WMI_SERVICE_IS_ENABLED(wma->wmi_service_bitmap,
				    WMI_SERVICE_EXTSCAN)) {
		WMA_LOGE("%s: extscan not enabled", __func__);
		return CDF_STATUS_E_FAILURE;
	}
	len = sizeof(*cmd);
	wmi_buf = wmi_buf_alloc(wma->wmi_handle, len);
	if (!wmi_buf) {
		WMA_LOGE("%s: wmi_buf_alloc failed", __func__);
		return CDF_STATUS_E_NOMEM;
	}
	buf_ptr = (uint8_t *) wmi_buf_data(wmi_buf);

	cmd = (wmi_extscan_get_cached_results_cmd_fixed_param *) buf_ptr;
	WMITLV_SET_HDR(&cmd->tlv_header,
		       WMITLV_TAG_STRUC_wmi_extscan_get_cached_results_cmd_fixed_param,
		       WMITLV_GET_STRUCT_TLVLEN
			       (wmi_extscan_get_cached_results_cmd_fixed_param));

	cmd->request_id = pcached_results->requestId;
	cmd->vdev_id = pcached_results->sessionId;
	cmd->control_flags = pcached_results->flush;

	if (wmi_unified_cmd_send(wma->wmi_handle, wmi_buf, len,
				 WMI_EXTSCAN_GET_CACHED_RESULTS_CMDID)) {
		WMA_LOGE("%s: failed to  command", __func__);
		cdf_nbuf_free(wmi_buf);
		return CDF_STATUS_E_FAILURE;
	}
	return CDF_STATUS_SUCCESS;
}

/**
 * wma_extscan_get_capabilities() - extscan get capabilities
 * @wma: wma handle
 * @pgetcapab: get capabilities params
 *
 * This function send request to fw to get extscan capabilities.
 *
 * Return: CDF status
 */
CDF_STATUS wma_extscan_get_capabilities(tp_wma_handle wma,
					tSirGetExtScanCapabilitiesReqParams *
					pgetcapab)
{
	wmi_extscan_get_capabilities_cmd_fixed_param *cmd;
	wmi_buf_t wmi_buf;
	uint32_t len;
	uint8_t *buf_ptr;

	if (!wma || !wma->wmi_handle) {
		WMA_LOGE("%s: WMA is closed, can not issue  cmd", __func__);
		return CDF_STATUS_E_INVAL;
	}
	if (!WMI_SERVICE_IS_ENABLED(wma->wmi_service_bitmap,
				    WMI_SERVICE_EXTSCAN)) {
		WMA_LOGE("%s: extscan not enabled", __func__);
		return CDF_STATUS_E_FAILURE;
	}
	len = sizeof(*cmd);
	wmi_buf = wmi_buf_alloc(wma->wmi_handle, len);
	if (!wmi_buf) {
		WMA_LOGE("%s: wmi_buf_alloc failed", __func__);
		return CDF_STATUS_E_NOMEM;
	}
	buf_ptr = (uint8_t *) wmi_buf_data(wmi_buf);

	cmd = (wmi_extscan_get_capabilities_cmd_fixed_param *) buf_ptr;
	WMITLV_SET_HDR(&cmd->tlv_header,
		       WMITLV_TAG_STRUC_wmi_extscan_get_capabilities_cmd_fixed_param,
		       WMITLV_GET_STRUCT_TLVLEN
			       (wmi_extscan_get_capabilities_cmd_fixed_param));

	cmd->request_id = pgetcapab->requestId;

	if (wmi_unified_cmd_send(wma->wmi_handle, wmi_buf, len,
				 WMI_EXTSCAN_GET_CAPABILITIES_CMDID)) {
		WMA_LOGE("%s: failed to  command", __func__);
		cdf_nbuf_free(wmi_buf);
		return CDF_STATUS_E_FAILURE;
	}
	return CDF_STATUS_SUCCESS;
}

CDF_STATUS  wma_ipa_offload_enable_disable(tp_wma_handle wma,
		struct sir_ipa_offload_enable_disable *ipa_offload)
{
	wmi_ipa_offload_enable_disable_cmd_fixed_param *cmd;
	wmi_buf_t wmi_buf;
	uint32_t len;
	u_int8_t *buf_ptr;
	ol_txrx_vdev_handle vdev;
	struct txrx_pdev_cfg_t *cfg;
	int32_t intra_bss_fwd = 0;

	if (!wma || !wma->wmi_handle) {
		WMA_LOGE("%s: WMA is closed, can not issue  cmd",
			__func__);
		return CDF_STATUS_E_INVAL;
	}

	if (!WMI_SERVICE_IS_ENABLED(wma->wmi_service_bitmap,
			((ipa_offload->offload_type == AP_RX_DATA_OFFLOAD) ?
			WMI_SERVICE_HSOFFLOAD :
			WMI_SERVICE_STA_RX_IPA_OFFLOAD_SUPPORT))) {
		WMA_LOGE("%s: %s not supported", __func__,
			((ipa_offload->offload_type == AP_RX_DATA_OFFLOAD) ?
			"WMI_SERVICE_HSOFFLOAD" :
			"WMI_SERVICE_STA_RX_IPA_OFFLOAD_SUPPORT"));
		return CDF_STATUS_E_FAILURE;
	}

	if (ipa_offload->offload_type > STA_RX_DATA_OFFLOAD) {
		return CDF_STATUS_E_INVAL;
	}

	len  = sizeof(*cmd);
	wmi_buf = wmi_buf_alloc(wma->wmi_handle, len);
	if (!wmi_buf) {
		WMA_LOGE("%s: wmi_buf_alloc failed (len=%d)", __func__, len);
		return CDF_STATUS_E_NOMEM;
	}

	WMA_LOGE("%s: offload_type=%d, enable=%d", __func__,
		ipa_offload->offload_type, ipa_offload->enable);

	buf_ptr = (u_int8_t *)wmi_buf_data(wmi_buf);

	cmd = (wmi_ipa_offload_enable_disable_cmd_fixed_param *)buf_ptr;
	WMITLV_SET_HDR(&cmd->tlv_header,
		WMITLV_TAG_STRUCT_wmi_ipa_offload_enable_disable_cmd_fixed_param,
		WMITLV_GET_STRUCT_TLVLEN(
		wmi_ipa_offload_enable_disable_cmd_fixed_param));

	cmd->offload_type = ipa_offload->offload_type;
	cmd->vdev_id = ipa_offload->vdev_id;
	cmd->enable = ipa_offload->enable;

	if (wmi_unified_cmd_send(wma->wmi_handle, wmi_buf, len,
		WMI_IPA_OFFLOAD_ENABLE_DISABLE_CMDID)) {
		WMA_LOGE("%s: failed to command", __func__);
		wmi_buf_free(wmi_buf);
		return CDF_STATUS_E_FAILURE;
	}

	/*
	 * Check if VDEV is already deleted. If deleted, don't
	 * send INTRA BSS FWD WMI command
	 */
	vdev = wma_find_vdev_by_id(wma, ipa_offload->vdev_id);
	if (!vdev)
		return CDF_STATUS_SUCCESS;

	/* Disable Intra-BSS FWD offload when gDisableIntraBssFwd=1 in INI */
	cfg = (struct txrx_pdev_cfg_t *)vdev->pdev->ctrl_pdev;
	if (!ipa_offload->enable || cfg->rx_fwd_disabled) {
		WMA_LOGE("%s: ipa_offload->enable=%d, rx_fwd_disabled=%d",
				__func__,
				ipa_offload->enable, cfg->rx_fwd_disabled);
		intra_bss_fwd = 1;
	}

	/* Disable/enable WMI_VDEV_PARAM_INTRA_BSS_FWD */
	if (wmi_unified_vdev_set_param_send(wma->wmi_handle,
		ipa_offload->vdev_id, WMI_VDEV_PARAM_INTRA_BSS_FWD,
		intra_bss_fwd)) {
		WMA_LOGE("Failed to disable WMI_VDEV_PARAM_INTRA_BSS_FWD");
		return CDF_STATUS_E_FAILURE;
	}

	return CDF_STATUS_SUCCESS;
}

/** wma_set_epno_network_list() - set epno network list
 * @wma: WMA handle
 * @req: epno config params request structure
 *
 * This function reads the incoming epno config request structure
 * and constructs the WMI message to the firmware.
 *
 * Returns: 0 on success, error number otherwise
 */
CDF_STATUS wma_set_epno_network_list(tp_wma_handle wma,
		struct wifi_epno_params *req)
{
	wmi_nlo_config_cmd_fixed_param *cmd;
	nlo_configured_parameters *nlo_list;
	u_int8_t i, *buf_ptr;
	wmi_buf_t buf;
	uint32_t len;
	int ret;

	WMA_LOGD("wma_set_epno_network_list");

	if (!wma || !wma->wmi_handle) {
		WMA_LOGE("%s: WMA is closed, can not issue cmd", __func__);
		return CDF_STATUS_E_FAILURE;
	}
	if (!WMI_SERVICE_IS_ENABLED(wma->wmi_service_bitmap,
			WMI_SERVICE_EXTSCAN)) {
		WMA_LOGE("%s: extscan not enabled", __func__);
		return CDF_STATUS_E_NOSUPPORT;
	}

	/* TLV place holder for array of structures
	 * nlo_configured_parameters(nlo_list) */
	len = sizeof(*cmd) + WMI_TLV_HDR_SIZE;
	len += sizeof(nlo_configured_parameters) *
				CDF_MIN(req->num_networks, WMI_NLO_MAX_SSIDS);
	len += WMI_TLV_HDR_SIZE; /* TLV for channel_list */
	len += WMI_TLV_HDR_SIZE; /* TLV for channel prediction cfg*/

	buf = wmi_buf_alloc(wma->wmi_handle, len);
	if (!buf) {
		WMA_LOGE("%s: Failed allocate wmi buffer", __func__);
		return CDF_STATUS_E_NOMEM;
	}

	cmd = (wmi_nlo_config_cmd_fixed_param *) wmi_buf_data(buf);

	buf_ptr = (u_int8_t *) cmd;
	WMITLV_SET_HDR(&cmd->tlv_header,
		       WMITLV_TAG_STRUC_wmi_nlo_config_cmd_fixed_param,
		       WMITLV_GET_STRUCT_TLVLEN(
			       wmi_nlo_config_cmd_fixed_param));
	cmd->vdev_id = req->session_id;
	cmd->flags = WMI_NLO_CONFIG_ENLO;

	buf_ptr += sizeof(wmi_nlo_config_cmd_fixed_param);

	cmd->no_of_ssids = CDF_MIN(req->num_networks, WMI_NLO_MAX_SSIDS);
	WMA_LOGD("SSID count: %d", cmd->no_of_ssids);
	WMITLV_SET_HDR(buf_ptr, WMITLV_TAG_ARRAY_STRUC,
		       cmd->no_of_ssids * sizeof(nlo_configured_parameters));
	buf_ptr += WMI_TLV_HDR_SIZE;

	nlo_list = (nlo_configured_parameters *) buf_ptr;
	for (i = 0; i < cmd->no_of_ssids; i++) {
		WMITLV_SET_HDR(&nlo_list[i].tlv_header,
			WMITLV_TAG_ARRAY_BYTE,
			WMITLV_GET_STRUCT_TLVLEN(nlo_configured_parameters));
		/* Copy ssid and it's length */
		nlo_list[i].ssid.valid = true;
		nlo_list[i].ssid.ssid.ssid_len = req->networks[i].ssid.length;
		cdf_mem_copy(nlo_list[i].ssid.ssid.ssid,
			     req->networks[i].ssid.ssId,
			     nlo_list[i].ssid.ssid.ssid_len);
		WMA_LOGD("index: %d ssid: %.*s len: %d", i,
			 nlo_list[i].ssid.ssid.ssid_len,
			 (char *) nlo_list[i].ssid.ssid.ssid,
			 nlo_list[i].ssid.ssid.ssid_len);

		/* Copy rssi threshold */
		nlo_list[i].rssi_cond.valid = true;
		nlo_list[i].rssi_cond.rssi =
			req->networks[i].rssi_threshold;
		WMA_LOGD("RSSI threshold : %d dBm",
			nlo_list[i].rssi_cond.rssi);

		/* Copy pno flags */
		nlo_list[i].bcast_nw_type.valid = true;
		nlo_list[i].bcast_nw_type.bcast_nw_type =
				req->networks[i].flags;
		WMA_LOGD("PNO flags (%u)",
				nlo_list[i].bcast_nw_type.bcast_nw_type);

		/* Copy auth bit field */
		nlo_list[i].auth_type.valid = true;
		nlo_list[i].auth_type.auth_type =
				req->networks[i].auth_bit_field;
		WMA_LOGD("Auth bit field (%u)",
				nlo_list[i].auth_type.auth_type);
	}

	buf_ptr += cmd->no_of_ssids * sizeof(nlo_configured_parameters);
	WMITLV_SET_HDR(buf_ptr, WMITLV_TAG_ARRAY_UINT32, 0);
	buf_ptr += WMI_TLV_HDR_SIZE;

	WMITLV_SET_HDR(buf_ptr, WMITLV_TAG_ARRAY_STRUC, 0);
	buf_ptr += WMI_TLV_HDR_SIZE;
	ret = wmi_unified_cmd_send(wma->wmi_handle, buf, len,
				   WMI_NETWORK_LIST_OFFLOAD_CONFIG_CMDID);
	if (ret) {
		WMA_LOGE("%s: Failed to send nlo wmi cmd", __func__);
		wmi_buf_free(buf);
		return CDF_STATUS_E_FAILURE;
	}

	WMA_LOGD("set ePNO list request sent successfully for vdev %d",
		 req->session_id);

	return CDF_STATUS_SUCCESS;
}

/**
 * wma_set_passpoint_network_list() - set passpoint network list
 * @handle: WMA handle
 * @req: passpoint network request structure
 *
 * This function reads the incoming @req and fill in the destination
 * WMI structure and send down the passpoint configs down to the firmware
 *
 * Return: CDF_STATUS enumeration
 */
CDF_STATUS wma_set_passpoint_network_list(tp_wma_handle wma,
					struct wifi_passpoint_req *req)
{
	wmi_passpoint_config_cmd_fixed_param *cmd;
	u_int8_t i, j, *bytes;
	wmi_buf_t buf;
	uint32_t len;
	int ret;

	WMA_LOGD("wma_set_passpoint_network_list");

	if (!wma || !wma->wmi_handle) {
		WMA_LOGE("%s: WMA is closed, can not issue cmd", __func__);
		return CDF_STATUS_E_FAILURE;
	}
	if (!WMI_SERVICE_IS_ENABLED(wma->wmi_service_bitmap,
			WMI_SERVICE_EXTSCAN)) {
		WMA_LOGE("%s: extscan not enabled", __func__);
		return CDF_STATUS_E_NOSUPPORT;
	}

	len = sizeof(*cmd);
	for (i = 0; i < req->num_networks; i++) {
		buf = wmi_buf_alloc(wma->wmi_handle, len);
		if (!buf) {
			WMA_LOGE("%s: Failed allocate wmi buffer", __func__);
			return CDF_STATUS_E_NOMEM;
		}

		cmd = (wmi_passpoint_config_cmd_fixed_param *)
				wmi_buf_data(buf);

		WMITLV_SET_HDR(&cmd->tlv_header,
			WMITLV_TAG_STRUC_wmi_passpoint_config_cmd_fixed_param,
			WMITLV_GET_STRUCT_TLVLEN(
			wmi_passpoint_config_cmd_fixed_param));
		cmd->id = req->networks[i].id;
		WMA_LOGD("%s: network id: %u", __func__, cmd->id);
		cdf_mem_copy(cmd->realm, req->networks[i].realm,
			strlen(req->networks[i].realm) + 1);
		WMA_LOGD("%s: realm: %s", __func__, cmd->realm);
		for (j = 0; j < PASSPOINT_ROAMING_CONSORTIUM_ID_NUM; j++) {
			bytes = (uint8_t *) &req->networks[i].roaming_consortium_ids[j];
			WMA_LOGD("index: %d rcids: %02x %02x %02x %02x %02x %02x %02x %02x",
				j, bytes[0], bytes[1], bytes[2], bytes[3],
				bytes[4], bytes[5], bytes[6], bytes[7]);

			cdf_mem_copy(&cmd->roaming_consortium_ids[j],
				&req->networks[i].roaming_consortium_ids[j],
				PASSPOINT_ROAMING_CONSORTIUM_ID_LEN);
		}
		cdf_mem_copy(cmd->plmn, req->networks[i].plmn,
				PASSPOINT_PLMN_ID_LEN);
		WMA_LOGD("%s: plmn: %02x:%02x:%02x", __func__,
			cmd->plmn[0], cmd->plmn[1], cmd->plmn[2]);

		ret = wmi_unified_cmd_send(wma->wmi_handle, buf, len,
					   WMI_PASSPOINT_LIST_CONFIG_CMDID);
		if (ret) {
			WMA_LOGE("%s: Failed to send set passpoint network list wmi cmd",
				 __func__);
			wmi_buf_free(buf);
			return CDF_STATUS_E_FAILURE;
		}
	}

	WMA_LOGD("Set passpoint network list request is sent successfully for vdev %d",
		 req->session_id);

	return CDF_STATUS_SUCCESS;
}

/**
 * wma_reset_passpoint_network_list() - reset passpoint network list
 * @handle: WMA handle
 * @req: passpoint network request structure
 *
 * This function sends down WMI command with network id set to wildcard id.
 * firmware shall clear all the config entries
 *
 * Return: CDF_STATUS enumeration
 */
CDF_STATUS wma_reset_passpoint_network_list(tp_wma_handle wma,
					struct wifi_passpoint_req *req)
{
	wmi_passpoint_config_cmd_fixed_param *cmd;
	wmi_buf_t buf;
	uint32_t len;
	int ret;

	WMA_LOGD("wma_reset_passpoint_network_list");

	if (!wma || !wma->wmi_handle) {
		WMA_LOGE("%s: WMA is closed, can not issue cmd", __func__);
		return CDF_STATUS_E_FAILURE;
	}
	if (!WMI_SERVICE_IS_ENABLED(wma->wmi_service_bitmap,
			WMI_SERVICE_EXTSCAN)) {
		WMA_LOGE("%s: extscan not enabled", __func__);
		return CDF_STATUS_E_NOSUPPORT;
	}

	len = sizeof(*cmd);
	buf = wmi_buf_alloc(wma->wmi_handle, len);
	if (!buf) {
		WMA_LOGE("%s: Failed allocate wmi buffer", __func__);
		return CDF_STATUS_E_NOMEM;
	}

	cmd = (wmi_passpoint_config_cmd_fixed_param *) wmi_buf_data(buf);

	WMITLV_SET_HDR(&cmd->tlv_header,
			WMITLV_TAG_STRUC_wmi_passpoint_config_cmd_fixed_param,
			WMITLV_GET_STRUCT_TLVLEN(
			wmi_passpoint_config_cmd_fixed_param));
	cmd->id = WMI_PASSPOINT_NETWORK_ID_WILDCARD;

	ret = wmi_unified_cmd_send(wma->wmi_handle, buf, len,
				   WMI_PASSPOINT_LIST_CONFIG_CMDID);
	if (ret) {
		WMA_LOGE("%s: Failed to send reset passpoint network list wmi cmd",
			 __func__);
		wmi_buf_free(buf);
		return CDF_STATUS_E_FAILURE;
	}

	WMA_LOGD("Reset passpoint network list request is sent successfully for vdev %d",
		 req->session_id);

	return CDF_STATUS_SUCCESS;
}

/**
 * wma_set_ssid_hotlist() - Handle an SSID hotlist set request
 * @wma: WMA handle
 * @request: SSID hotlist set request from SME
 *
 * Return: CDF_STATUS enumeration
 */
CDF_STATUS
wma_set_ssid_hotlist(tp_wma_handle wma,
		     struct sir_set_ssid_hotlist_request *request)
{
	wmi_extscan_configure_hotlist_ssid_monitor_cmd_fixed_param *cmd;
	wmi_buf_t wmi_buf;
	uint32_t len;
	uint32_t array_size;
	uint8_t *buf_ptr;

	if (!wma || !wma->wmi_handle) {
		WMA_LOGE("%s: WMA is closed, can not issue hotlist cmd",
			 __func__);
		return CDF_STATUS_E_FAILURE;
	}
	if (!request) {
		WMA_LOGE("%s: Invalid request buffer", __func__);
		return CDF_STATUS_E_FAILURE;
	}
	if (!WMI_SERVICE_IS_ENABLED(wma->wmi_service_bitmap,
				    WMI_SERVICE_EXTSCAN)) {
		WMA_LOGE("%s: extscan not enabled",
			__func__);
		return CDF_STATUS_E_NOSUPPORT;
	}

	/* length of fixed portion */
	len = sizeof(*cmd);

	/* length of variable portion */
	array_size =
		request->ssid_count * sizeof(wmi_extscan_hotlist_ssid_entry);
	len += WMI_TLV_HDR_SIZE + array_size;

	wmi_buf = wmi_buf_alloc(wma->wmi_handle, len);
	if (!wmi_buf) {
		WMA_LOGE("%s: wmi_buf_alloc failed", __func__);
		return CDF_STATUS_E_NOMEM;
	}

	buf_ptr = (uint8_t *) wmi_buf_data(wmi_buf);
	cmd = (wmi_extscan_configure_hotlist_ssid_monitor_cmd_fixed_param *)
						buf_ptr;
	WMITLV_SET_HDR
		(&cmd->tlv_header,
		 WMITLV_TAG_STRUC_wmi_extscan_configure_hotlist_ssid_monitor_cmd_fixed_param,
		 WMITLV_GET_STRUCT_TLVLEN
			(wmi_extscan_configure_hotlist_ssid_monitor_cmd_fixed_param));

	cmd->request_id = request->request_id;
	cmd->requestor_id = 0;
	cmd->vdev_id = request->session_id;
	cmd->table_id = 0;
	cmd->lost_ap_scan_count = request->lost_ssid_sample_size;
	cmd->total_entries = request->ssid_count;
	cmd->num_entries_in_page = request->ssid_count;
	cmd->first_entry_index = 0;

	buf_ptr += sizeof(*cmd);
	WMITLV_SET_HDR(buf_ptr, WMITLV_TAG_ARRAY_STRUC, array_size);

	if (request->ssid_count) {
		wmi_extscan_hotlist_ssid_entry *entry;
		int i;

		buf_ptr += WMI_TLV_HDR_SIZE;
		entry = (wmi_extscan_hotlist_ssid_entry *)buf_ptr;
		for (i = 0; i < request->ssid_count; i++) {
			WMITLV_SET_HDR
				(entry,
				 WMITLV_TAG_ARRAY_STRUC,
				 WMITLV_GET_STRUCT_TLVLEN
					(wmi_extscan_hotlist_ssid_entry));
			entry->ssid.ssid_len = request->ssids[i].ssid.length;
			cdf_mem_copy(entry->ssid.ssid,
				     request->ssids[i].ssid.ssId,
				     request->ssids[i].ssid.length);
			entry->band = request->ssids[i].band;
			entry->min_rssi = request->ssids[i].rssi_low;
			entry->max_rssi = request->ssids[i].rssi_high;
			entry++;
		}
		cmd->mode = WMI_EXTSCAN_MODE_START;
	} else {
		cmd->mode = WMI_EXTSCAN_MODE_STOP;
	}

	if (wmi_unified_cmd_send
		(wma->wmi_handle, wmi_buf, len,
		 WMI_EXTSCAN_CONFIGURE_HOTLIST_SSID_MONITOR_CMDID)) {
		WMA_LOGE("%s: failed to send command", __func__);
		wmi_buf_free(wmi_buf);
		return CDF_STATUS_E_FAILURE;
	}
	return CDF_STATUS_SUCCESS;
}
#endif

/**
 * wma_scan_probe_setoui() - set scan probe OUI
 * @wma: wma handle
 * @psetoui: OUI parameters
 *
 * set scan probe OUI parameters in firmware
 *
 * Return: CDF status
 */
CDF_STATUS wma_scan_probe_setoui(tp_wma_handle wma, tSirScanMacOui *psetoui)
{
	wmi_scan_prob_req_oui_cmd_fixed_param *cmd;
	wmi_buf_t wmi_buf;
	uint32_t len;
	uint8_t *buf_ptr;
	uint32_t *oui_buf;

	if (!wma || !wma->wmi_handle) {
		WMA_LOGE("%s: WMA is closed, can not issue  cmd", __func__);
		return CDF_STATUS_E_INVAL;
	}
	len = sizeof(*cmd);
	wmi_buf = wmi_buf_alloc(wma->wmi_handle, len);
	if (!wmi_buf) {
		WMA_LOGE("%s: wmi_buf_alloc failed", __func__);
		return CDF_STATUS_E_NOMEM;
	}
	buf_ptr = (uint8_t *) wmi_buf_data(wmi_buf);
	cmd = (wmi_scan_prob_req_oui_cmd_fixed_param *) buf_ptr;
	WMITLV_SET_HDR(&cmd->tlv_header,
		       WMITLV_TAG_STRUC_wmi_scan_prob_req_oui_cmd_fixed_param,
		       WMITLV_GET_STRUCT_TLVLEN
			       (wmi_scan_prob_req_oui_cmd_fixed_param));

	oui_buf = &cmd->prob_req_oui;
	cdf_mem_zero(oui_buf, sizeof(cmd->prob_req_oui));
	*oui_buf = psetoui->oui[0] << 16 | psetoui->oui[1] << 8
		   | psetoui->oui[2];
	WMA_LOGD("%s: wma:oui received from hdd %08x", __func__,
		 cmd->prob_req_oui);

	if (wmi_unified_cmd_send(wma->wmi_handle, wmi_buf, len,
				 WMI_SCAN_PROB_REQ_OUI_CMDID)) {
		WMA_LOGE("%s: failed to send command", __func__);
		cdf_nbuf_free(wmi_buf);
		return CDF_STATUS_E_FAILURE;
	}
	return CDF_STATUS_SUCCESS;
}

/**
 * wma_scan_event_callback() - scan event callback
 * @handle: wma handle
 * @data: event data
 * @len: data length
 *
 * This function process scan event and provide indication
 * to upper layers.
 *
 * Return: 0 for success or error code.
 */
int wma_scan_event_callback(WMA_HANDLE handle, uint8_t *data,
			    uint32_t len)
{
	tp_wma_handle wma_handle = (tp_wma_handle) handle;
	WMI_SCAN_EVENTID_param_tlvs *param_buf = NULL;
	wmi_scan_event_fixed_param *wmi_event = NULL;
	tSirScanOffloadEvent *scan_event;
	uint8_t vdev_id;
	uint32_t scan_id;

	param_buf = (WMI_SCAN_EVENTID_param_tlvs *) data;
	wmi_event = param_buf->fixed_param;
	vdev_id = wmi_event->vdev_id;
	scan_id = wma_handle->interfaces[vdev_id].scan_info.scan_id;

	if (wma_handle->roam_preauth_scan_id == wmi_event->scan_id) {
		/* This is the scan requested by roam preauth set_channel operation */

		if (wmi_event->event & WMI_SCAN_FINISH_EVENTS) {
			WMA_LOGE("roam scan complete: scan_id 0x%x, vdev_id %d",
				 wmi_event->scan_id, vdev_id);
			/*wma_reset_scan_info(wma_handle, vdev_id);*/
		}

		vdev_id = wmi_event->vdev_id;
		if (vdev_id >= wma_handle->max_bssid) {
			WMA_LOGE("%s:Invalid vdev_id %d wmi_event %p", __func__,
					 vdev_id, wmi_event);
			return -EFAULT;
		}

		wma_roam_preauth_scan_event_handler(wma_handle,
						 vdev_id, wmi_event);
		return 0;

	}

	scan_event = (tSirScanOffloadEvent *) cdf_mem_malloc
			     (sizeof(tSirScanOffloadEvent));
	if (!scan_event) {
		WMA_LOGE("Memory allocation failed for tSirScanOffloadEvent");
		return -ENOMEM;
	}

	scan_event->event = wmi_event->event;

	WMA_LOGI("WMA <-- wmi_scan_event : event %u, scan_id %u, "
		 "freq %u, reason %u",
		 wmi_event->event, wmi_event->scan_id,
		 wmi_event->channel_freq, wmi_event->reason);

	scan_event->scanId = wmi_event->scan_id;
	scan_event->chanFreq = wmi_event->channel_freq;

	if (scan_event->scanId ==
		wma_handle->interfaces[vdev_id].p2p_scan_info.scan_id) {
		scan_event->p2pScanType = P2P_SCAN_TYPE_LISTEN;
		if  (scan_event->event == SCAN_EVENT_COMPLETED)
			wma_reset_p2p_scan_info(wma_handle, vdev_id);
	}
	scan_event->sessionId = vdev_id;

	if (wmi_event->reason == WMI_SCAN_REASON_COMPLETED ||
	    wmi_event->reason == WMI_SCAN_REASON_TIMEDOUT)
		scan_event->reasonCode = eSIR_SME_SUCCESS;
	else
		scan_event->reasonCode = eSIR_SME_SCAN_FAILED;

	switch (wmi_event->event) {
	case WMI_SCAN_EVENT_COMPLETED:
	case WMI_SCAN_EVENT_DEQUEUED:
		/*
		 * return success always so that SME can pick whatever scan
		 * results is available in scan cache(due to partial or
		 * aborted scan)
		 */
		scan_event->event = WMI_SCAN_EVENT_COMPLETED;
		scan_event->reasonCode = eSIR_SME_SUCCESS;
		break;
	case WMI_SCAN_EVENT_START_FAILED:
		scan_event->event = WMI_SCAN_EVENT_COMPLETED;
		scan_event->reasonCode = eSIR_SME_SCAN_FAILED;
		break;
	case WMI_SCAN_EVENT_PREEMPTED:
		WMA_LOGW("%s: Unhandled Scan Event WMI_SCAN_EVENT_PREEMPTED",
			 __func__);
		break;
	case WMI_SCAN_EVENT_RESTARTED:
		WMA_LOGW("%s: Unhandled Scan Event WMI_SCAN_EVENT_RESTARTED",
			 __func__);
		break;
	}

	/* Stop the scan completion timeout if the event is WMI_SCAN_EVENT_COMPLETED */
	if (scan_event->event == (tSirScanEventType) WMI_SCAN_EVENT_COMPLETED) {
		WMA_LOGE(" scan complete - scan_id 0x%x, vdev_id %d",
			 wmi_event->scan_id, vdev_id);
	}

	wma_send_msg(wma_handle, WMA_RX_SCAN_EVENT, (void *)scan_event, 0);
	return 0;
}


/**
 * wma_roam_better_ap_handler() - better ap event handler
 * @wma: wma handle
 * @vdev_id: vdev id
 *
 * Handler for WMI_ROAM_REASON_BETTER_AP event from roam firmware in Rome.
 * This event means roam algorithm in Rome has found a better matching
 * candidate AP. The indication is sent to SME.
 *
 * Return: none
 */
void wma_roam_better_ap_handler(tp_wma_handle wma, uint32_t vdev_id)
{
	cds_msg_t cds_msg;
	tSirSmeCandidateFoundInd *candidate_ind;

	/* abort existing scans from GUI, but not roaming preauth scan */
	if (wma->interfaces[vdev_id].scan_info.scan_id != 0 &&
	    (wma->interfaces[vdev_id].scan_info.scan_id &
	     WMA_HOST_ROAM_SCAN_REQID_PREFIX) !=
		WMA_HOST_ROAM_SCAN_REQID_PREFIX) {
		tAbortScanParams abortScan;
		abortScan.SessionId = vdev_id;
		wma_stop_scan(wma, &abortScan);
	}

	candidate_ind = cdf_mem_malloc(sizeof(tSirSmeCandidateFoundInd));
	if (!candidate_ind) {
		WMA_LOGE("%s: Alloc failed for tSirSmeCandidateFoundInd",
			__func__);
		return;
	}

	candidate_ind->messageType = eWNI_SME_CANDIDATE_FOUND_IND;
	candidate_ind->sessionId = vdev_id;
	candidate_ind->length = sizeof(tSirSmeCandidateFoundInd);

	cds_msg.type = eWNI_SME_CANDIDATE_FOUND_IND;
	cds_msg.bodyptr = candidate_ind;
	CDF_TRACE(CDF_MODULE_ID_WMA, CDF_TRACE_LEVEL_INFO,
		  FL("posting candidate ind to SME"));

	if (CDF_STATUS_SUCCESS != cds_mq_post_message(CDS_MQ_ID_SME,
						(cds_msg_t *) &cds_msg)) {
		cdf_mem_free(candidate_ind);
		CDF_TRACE(CDF_MODULE_ID_WMA, CDF_TRACE_LEVEL_ERROR,
			  FL("Failed to post candidate ind to SME"));
	}
}

/**
 * wma_roam_event_callback() - roam event callback
 * @handle: wma handle
 * @event_buf: event buffer
 * @len: buffer length
 *
 * Handler for all events from roam engine in firmware
 *
 * Return: 0 for success or error code
 */
int wma_roam_event_callback(WMA_HANDLE handle, uint8_t *event_buf,
				   uint32_t len)
{
	tp_wma_handle wma_handle = (tp_wma_handle) handle;
	WMI_ROAM_EVENTID_param_tlvs *param_buf;
	wmi_roam_event_fixed_param *wmi_event;

	param_buf = (WMI_ROAM_EVENTID_param_tlvs *) event_buf;
	if (!param_buf) {
		WMA_LOGE("Invalid roam event buffer");
		return -EINVAL;
	}

	wmi_event = param_buf->fixed_param;
	WMA_LOGD("%s: Reason %x for vdevid %x, rssi %d",
		 __func__, wmi_event->reason, wmi_event->vdev_id,
		 wmi_event->rssi);

	switch (wmi_event->reason) {
	case WMI_ROAM_REASON_BMISS:
		WMA_LOGD("Beacon Miss for vdevid %x", wmi_event->vdev_id);
		wma_beacon_miss_handler(wma_handle, wmi_event->vdev_id);
		break;
	case WMI_ROAM_REASON_BETTER_AP:
		WMA_LOGD("%s:Better AP found for vdevid %x, rssi %d", __func__,
			 wmi_event->vdev_id, wmi_event->rssi);
		wma_handle->suitable_ap_hb_failure = false;
		wma_roam_better_ap_handler(wma_handle, wmi_event->vdev_id);
		break;
	case WMI_ROAM_REASON_SUITABLE_AP:
		wma_handle->suitable_ap_hb_failure = true;
		WMA_LOGD("%s:Bmiss scan AP found for vdevid %x, rssi %d",
			 __func__, wmi_event->vdev_id, wmi_event->rssi);
		wma_roam_better_ap_handler(wma_handle, wmi_event->vdev_id);
		break;
#ifdef WLAN_FEATURE_ROAM_OFFLOAD
	case WMI_ROAM_REASON_HO_FAILED:
		WMA_LOGE("LFR3:Hand-Off Failed for vdevid %x",
			 wmi_event->vdev_id);
		wma_roam_ho_fail_handler(wma_handle, wmi_event->vdev_id);
		break;
#endif
	default:
		WMA_LOGD("%s:Unhandled Roam Event %x for vdevid %x", __func__,
			 wmi_event->reason, wmi_event->vdev_id);
		break;
	}
	return 0;
}


/**
 * wma_set_rssi_monitoring() - set rssi monitoring
 * @handle: WMA handle
 * @req: rssi monitoring request structure
 *
 * This function reads the incoming @req and fill in the destination
 * WMI structure and send down the rssi monitoring configs down to the firmware
 *
 * Return: 0 on success; error number otherwise
 */
CDF_STATUS wma_set_rssi_monitoring(tp_wma_handle wma,
					struct rssi_monitor_req *req)
{
	wmi_rssi_breach_monitor_config_fixed_param *cmd;
	wmi_buf_t buf;
	int ret, len = sizeof(*cmd);

	buf = wmi_buf_alloc(wma->wmi_handle, len);
	if (!buf) {
		WMA_LOGP("%s: wmi_buf_alloc failed", __func__);
		return CDF_STATUS_E_NOMEM;
	}

	cmd = (wmi_rssi_breach_monitor_config_fixed_param *) wmi_buf_data(buf);
	WMITLV_SET_HDR(&cmd->tlv_header,
		WMITLV_TAG_STRUC_wmi_rssi_breach_monitor_config_fixed_param,
		WMITLV_GET_STRUCT_TLVLEN(
			wmi_rssi_breach_monitor_config_fixed_param));

	cmd->vdev_id = req->session_id;
	cmd->request_id = req->request_id;
	cmd->lo_rssi_reenable_hysteresis = 0;
	cmd->hi_rssi_reenable_histeresis = 0;
	cmd->min_report_interval = 0;
	cmd->max_num_report = 1;
	if (req->control) {
		/* enable one threshold for each min/max */
		cmd->enabled_bitmap = 0x09;
		cmd->low_rssi_breach_threshold[0] = req->min_rssi;
		cmd->hi_rssi_breach_threshold[0] = req->max_rssi;
	} else {
		cmd->enabled_bitmap = 0;
		cmd->low_rssi_breach_threshold[0] = 0;
		cmd->hi_rssi_breach_threshold[0] = 0;
	}

	ret = wmi_unified_cmd_send(wma->wmi_handle, buf, len,
				   WMI_RSSI_BREACH_MONITOR_CONFIG_CMDID);
	if (ret != EOK) {
		WMA_LOGE("Failed to send WMI_RSSI_BREACH_MONITOR_CONFIG_CMDID");
		wmi_buf_free(buf);
		return CDF_STATUS_E_FAILURE;
	}

	WMA_LOGI("Sent WMI_RSSI_BREACH_MONITOR_CONFIG_CMDID to FW");
	return CDF_STATUS_SUCCESS;
}

/**
 * wma_get_scan_id() - Generates scan id
 * @scan_id: Scan id
 *
 * This function generates the scan id.
 *
 * Return: CDF_STATUS
 */

CDF_STATUS wma_get_scan_id(uint32_t *scan_id)
{
	tp_wma_handle wma = cds_get_context(CDF_MODULE_ID_WMA);

	if (!scan_id) {
		WMA_LOGE("Scan_id is NULL");
		return CDF_STATUS_E_FAULT;
	}

	/* host need to cycle through the lower 12 bits to generate ids */
	*scan_id = cdf_atomic_inc_return(&wma->scan_id_counter) &
			WMA_SCAN_ID_MASK;
	/*
	 * Firmware expects the host scan request id appended
	 * by PREFIX 0xA000
	 */
	*scan_id = *scan_id | WMI_HOST_SCAN_REQ_ID_PREFIX;
	return CDF_STATUS_SUCCESS;
}

