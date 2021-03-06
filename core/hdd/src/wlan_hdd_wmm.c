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
 * DOC: HDD WMM
 *
 * This module (wlan_hdd_wmm.h interface + wlan_hdd_wmm.c implementation)
 * houses all the logic for WMM in HDD.
 *
 * On the control path, it has the logic to setup QoS, modify QoS and delete
 * QoS (QoS here refers to a TSPEC). The setup QoS comes in two flavors: an
 * explicit application invoked and an internal HDD invoked.  The implicit QoS
 * is for applications that do NOT call the custom QCT WLAN OIDs for QoS but
 * which DO mark their traffic for priortization. It also has logic to start,
 * update and stop the U-APSD trigger frame generation. It also has logic to
 * read WMM related config parameters from the registry.
 *
 * On the data path, it has the logic to figure out the WMM AC of an egress
 * packet and when to signal TL to serve a particular AC queue. It also has the
 * logic to retrieve a packet based on WMM priority in response to a fetch from
 * TL.
 *
 * The remaining functions are utility functions for information hiding.
 */

/* Include files */
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/etherdevice.h>
#include <linux/if_vlan.h>
#include <linux/ip.h>
#include <linux/semaphore.h>
#include <linux/ipv6.h>
#include <wlan_hdd_tx_rx.h>
#include <wlan_hdd_wmm.h>
#include <wlan_hdd_ether.h>
#include <wlan_hdd_hostapd.h>
#include <wlan_hdd_softap_tx_rx.h>
#include <cds_sched.h>

/* change logging behavior based upon debug flag */
#ifdef HDD_WMM_DEBUG
#define WMM_TRACE_LEVEL_FATAL      CDF_TRACE_LEVEL_FATAL
#define WMM_TRACE_LEVEL_ERROR      CDF_TRACE_LEVEL_FATAL
#define WMM_TRACE_LEVEL_WARN       CDF_TRACE_LEVEL_FATAL
#define WMM_TRACE_LEVEL_INFO       CDF_TRACE_LEVEL_FATAL
#define WMM_TRACE_LEVEL_INFO_HIGH  CDF_TRACE_LEVEL_FATAL
#define WMM_TRACE_LEVEL_INFO_LOW   CDF_TRACE_LEVEL_FATAL
#else
#define WMM_TRACE_LEVEL_FATAL      CDF_TRACE_LEVEL_FATAL
#define WMM_TRACE_LEVEL_ERROR      CDF_TRACE_LEVEL_ERROR
#define WMM_TRACE_LEVEL_WARN       CDF_TRACE_LEVEL_WARN
#define WMM_TRACE_LEVEL_INFO       CDF_TRACE_LEVEL_INFO
#define WMM_TRACE_LEVEL_INFO_HIGH  CDF_TRACE_LEVEL_INFO_HIGH
#define WMM_TRACE_LEVEL_INFO_LOW   CDF_TRACE_LEVEL_INFO_LOW
#endif

#define WLAN_HDD_MAX_DSCP 0x3f

/* DHCP Port number */
#define DHCP_SOURCE_PORT 0x4400
#define DHCP_DESTINATION_PORT 0x4300

#define HDD_WMM_UP_TO_AC_MAP_SIZE 8

const uint8_t hdd_wmm_up_to_ac_map[] = {
	SME_AC_BE,
	SME_AC_BK,
	SME_AC_BK,
	SME_AC_BE,
	SME_AC_VI,
	SME_AC_VI,
	SME_AC_VO,
	SME_AC_VO
};

/**
 * enum hdd_wmm_linuxac: AC/Queue Index values for Linux Qdisc to
 * operate on different traffic.
 */
#ifdef QCA_LL_TX_FLOW_CONTROL_V2
enum hdd_wmm_linuxac {
	HDD_LINUX_AC_VO = 0,
	HDD_LINUX_AC_VI = 1,
	HDD_LINUX_AC_BE = 2,
	HDD_LINUX_AC_BK = 3,
	HDD_LINUX_AC_HI_PRIO = 4,
};

void wlan_hdd_process_peer_unauthorised_pause(hdd_adapter_t *adapter)
{
	/* Enable HI_PRIO queue */
	netif_stop_subqueue(adapter->dev, HDD_LINUX_AC_VO);
	netif_stop_subqueue(adapter->dev, HDD_LINUX_AC_VI);
	netif_stop_subqueue(adapter->dev, HDD_LINUX_AC_BE);
	netif_stop_subqueue(adapter->dev, HDD_LINUX_AC_BK);
	netif_wake_subqueue(adapter->dev, HDD_LINUX_AC_HI_PRIO);

}
#else
enum hdd_wmm_linuxac {
	HDD_LINUX_AC_VO = 0,
	HDD_LINUX_AC_VI = 1,
	HDD_LINUX_AC_BE = 2,
	HDD_LINUX_AC_BK = 3
};

void wlan_hdd_process_peer_unauthorised_pause(hdd_adapter_t *adapter)
{
	return;
}
#endif

/* Linux based UP -> AC Mapping */
const uint8_t hdd_linux_up_to_ac_map[HDD_WMM_UP_TO_AC_MAP_SIZE] = {
	HDD_LINUX_AC_BE,
	HDD_LINUX_AC_BK,
	HDD_LINUX_AC_BK,
	HDD_LINUX_AC_BE,
	HDD_LINUX_AC_VI,
	HDD_LINUX_AC_VI,
	HDD_LINUX_AC_VO,
	HDD_LINUX_AC_VO
};

#ifndef WLAN_MDM_CODE_REDUCTION_OPT
/**
 * hdd_wmm_enable_tl_uapsd() - function which decides whether and
 * how to update UAPSD parameters in TL
 *
 * @pQosContext: [in] the pointer the QoS instance control block
 *
 * Return: None
 */
static void hdd_wmm_enable_tl_uapsd(hdd_wmm_qos_context_t *pQosContext)
{
	hdd_adapter_t *pAdapter = pQosContext->pAdapter;
	sme_ac_enum_type acType = pQosContext->acType;
	hdd_wmm_ac_status_t *pAc = &pAdapter->hddWmmStatus.wmmAcStatus[acType];
	hdd_context_t *pHddCtx = WLAN_HDD_GET_CTX(pAdapter);
	CDF_STATUS status;
	uint32_t service_interval;
	uint32_t suspension_interval;
	sme_QosWmmDirType direction;
	bool psb;

	/* The TSPEC must be valid */
	if (pAc->wmmAcTspecValid == false) {
		CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_ERROR,
			  "%s: Invoked with invalid TSPEC", __func__);
		return;
	}
	/* determine the service interval */
	if (pAc->wmmAcTspecInfo.min_service_interval) {
		service_interval = pAc->wmmAcTspecInfo.min_service_interval;
	} else if (pAc->wmmAcTspecInfo.max_service_interval) {
		service_interval = pAc->wmmAcTspecInfo.max_service_interval;
	} else {
		/* no service interval is present in the TSPEC */
		/* this is OK, there just won't be U-APSD */
		CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO,
			  "%s: No service interval supplied", __func__);
		service_interval = 0;
	}

	/* determine the suspension interval & direction */
	suspension_interval = pAc->wmmAcTspecInfo.suspension_interval;
	direction = pAc->wmmAcTspecInfo.ts_info.direction;
	psb = pAc->wmmAcTspecInfo.ts_info.psb;

	/* if we have previously enabled U-APSD, have any params changed? */
	if ((pAc->wmmAcUapsdInfoValid) &&
	    (pAc->wmmAcUapsdServiceInterval == service_interval) &&
	    (pAc->wmmAcUapsdSuspensionInterval == suspension_interval) &&
	    (pAc->wmmAcUapsdDirection == direction) &&
	    (pAc->wmmAcIsUapsdEnabled == psb)) {
		CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO,
			  "%s: No change in U-APSD parameters", __func__);
		return;
	}
	/* everything is in place to notify TL */
	status =
		sme_enable_uapsd_for_ac((WLAN_HDD_GET_CTX(pAdapter))->pcds_context,
					   (WLAN_HDD_GET_STATION_CTX_PTR(pAdapter))->
					   conn_info.staId[0], acType,
					   pAc->wmmAcTspecInfo.ts_info.tid,
					   pAc->wmmAcTspecInfo.ts_info.up,
					   service_interval, suspension_interval,
					   direction, psb, pAdapter->sessionId,
					   pHddCtx->config->DelayedTriggerFrmInt);

	if (!CDF_IS_STATUS_SUCCESS(status)) {
		CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_ERROR,
			  "%s: Failed to enable U-APSD for AC=%d",
			  __func__, acType);
		return;
	}
	/* stash away the parameters that were used */
	pAc->wmmAcUapsdInfoValid = true;
	pAc->wmmAcUapsdServiceInterval = service_interval;
	pAc->wmmAcUapsdSuspensionInterval = suspension_interval;
	pAc->wmmAcUapsdDirection = direction;
	pAc->wmmAcIsUapsdEnabled = psb;

	CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO,
		  "%s: Enabled UAPSD in TL srv_int=%d "
		  "susp_int=%d dir=%d AC=%d",
		  __func__,
		  service_interval, suspension_interval, direction, acType);

}

/**
 * hdd_wmm_disable_tl_uapsd() - function which decides whether
 * to disable UAPSD parameters in TL
 *
 * @pQosContext: [in] the pointer the QoS instance control block
 *
 * Return: None
 */
static void hdd_wmm_disable_tl_uapsd(hdd_wmm_qos_context_t *pQosContext)
{
	hdd_adapter_t *pAdapter = pQosContext->pAdapter;
	sme_ac_enum_type acType = pQosContext->acType;
	hdd_wmm_ac_status_t *pAc = &pAdapter->hddWmmStatus.wmmAcStatus[acType];
	CDF_STATUS status;

	/* have we previously enabled UAPSD? */
	if (pAc->wmmAcUapsdInfoValid == true) {
		status =
			sme_disable_uapsd_for_ac((WLAN_HDD_GET_CTX(pAdapter))->
						    pcds_context,
						    (WLAN_HDD_GET_STATION_CTX_PTR
							     (pAdapter))->conn_info.staId[0],
						    acType, pAdapter->sessionId);

		if (!CDF_IS_STATUS_SUCCESS(status)) {
			CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_ERROR,
				  "%s: Failed to disable U-APSD for AC=%d",
				  __func__, acType);
		} else {
			/* TL no longer has valid UAPSD info */
			pAc->wmmAcUapsdInfoValid = false;
			CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO,
				  "%s: Disabled UAPSD in TL for AC=%d",
				  __func__, acType);
		}
	}
}

#endif

/**
 * hdd_wmm_free_context() - function which frees a QoS context
 *
 * @pQosContext: [in] the pointer the QoS instance control block
 *
 * Return: None
 */
static void hdd_wmm_free_context(hdd_wmm_qos_context_t *pQosContext)
{
	hdd_adapter_t *pAdapter;

	CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO_LOW,
		  "%s: Entered, context %p", __func__, pQosContext);

	if (unlikely((NULL == pQosContext) ||
		     (HDD_WMM_CTX_MAGIC != pQosContext->magic))) {
		/* must have been freed in another thread */
		return;
	}
	/* get pointer to the adapter context */
	pAdapter = pQosContext->pAdapter;

	/* take the wmmLock since we're manipulating the context list */
	mutex_lock(&pAdapter->hddWmmStatus.wmmLock);

	/* make sure nobody thinks this is a valid context */
	pQosContext->magic = 0;

	/* unlink the context */
	list_del(&pQosContext->node);

	/* done manipulating the list */
	mutex_unlock(&pAdapter->hddWmmStatus.wmmLock);

	/* reclaim memory */
	kfree(pQosContext);

}

#ifndef WLAN_MDM_CODE_REDUCTION_OPT
/**
 * hdd_wmm_notify_app() - function which notifies an application
 *			  of changes in state of it flow
 *
 * @pQosContext: [in] the pointer the QoS instance control block
 *
 * Return: None
 */
#define MAX_NOTIFY_LEN 50
static void hdd_wmm_notify_app(hdd_wmm_qos_context_t *pQosContext)
{
	hdd_adapter_t *pAdapter;
	union iwreq_data wrqu;
	char buf[MAX_NOTIFY_LEN + 1];

	CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO_LOW,
		  "%s: Entered, context %p", __func__, pQosContext);

	if (unlikely((NULL == pQosContext) ||
		     (HDD_WMM_CTX_MAGIC != pQosContext->magic))) {
		CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_ERROR,
			  "%s: Invalid QoS Context", __func__);
		return;
	}

	/* create the event */
	memset(&wrqu, 0, sizeof(wrqu));
	memset(buf, 0, sizeof(buf));

	snprintf(buf, MAX_NOTIFY_LEN, "QCOM: TS change[%u: %u]",
		 (unsigned int)pQosContext->handle,
		 (unsigned int)pQosContext->lastStatus);

	wrqu.data.pointer = buf;
	wrqu.data.length = strlen(buf);

	/* get pointer to the adapter */
	pAdapter = pQosContext->pAdapter;

	/* send the event */
	CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO,
		  "%s: Sending [%s]", __func__, buf);
	wireless_send_event(pAdapter->dev, IWEVCUSTOM, &wrqu, buf);
}

#ifdef FEATURE_WLAN_ESE
/**
 * hdd_wmm_inactivity_timer_cb() - inactivity timer callback function
 *
 * @user_data: opaque user data registered with the timer.  In the
 * case of this timer, the associated wmm QoS context is registered.
 *
 * This timer handler function is called for every inactivity interval
 * per AC. This function gets the current transmitted packets on the
 * given AC, and checks if there was any TX activity from the previous
 * interval. If there was no traffic then it would delete the TS that
 * was negotiated on that AC.
 *
 * Return: None
 */
static void hdd_wmm_inactivity_timer_cb(void *user_data)
{
	hdd_wmm_qos_context_t *pQosContext = user_data;
	hdd_adapter_t *pAdapter;
	hdd_wmm_ac_status_t *pAc;
	hdd_wlan_wmm_status_e status;
	CDF_STATUS cdf_status;
	uint32_t currentTrafficCnt = 0;
	sme_ac_enum_type acType = pQosContext->acType;

	pAdapter = pQosContext->pAdapter;
	pAc = &pAdapter->hddWmmStatus.wmmAcStatus[acType];

	/* Get the Tx stats for this AC. */
	currentTrafficCnt =
		pAdapter->hdd_stats.hddTxRxStats.txXmitClassifiedAC[pQosContext->
								    acType];

	CDF_TRACE(CDF_MODULE_ID_HDD, CDF_TRACE_LEVEL_WARN,
		  FL(
		     "WMM inactivity Timer for AC=%d, currentCnt=%d, prevCnt=%d"
		     ),
		  acType, (int)currentTrafficCnt, (int)pAc->wmmPrevTrafficCnt);
	if (pAc->wmmPrevTrafficCnt == currentTrafficCnt) {
		/* there is no traffic activity, delete the TSPEC for this AC */
		status = hdd_wmm_delts(pAdapter, pQosContext->handle);
		CDF_TRACE(CDF_MODULE_ID_HDD, CDF_TRACE_LEVEL_WARN,
			  FL(
			     "Deleted TS on AC %d, due to inactivity with status = %d!!!"
			     ),
			  acType, status);
	} else {
		pAc->wmmPrevTrafficCnt = currentTrafficCnt;
		if (pAc->wmmInactivityTimer.state == CDF_TIMER_STATE_STOPPED) {
			/* Restart the timer */
			cdf_status =
				cdf_mc_timer_start(&pAc->wmmInactivityTimer,
						   pAc->wmmInactivityTime);
			if (!CDF_IS_STATUS_SUCCESS(cdf_status)) {
				CDF_TRACE(CDF_MODULE_ID_HDD,
					  CDF_TRACE_LEVEL_ERROR,
					  FL(
					     "Restarting inactivity timer failed on AC %d"
					     ),
					  acType);
			}
		} else {
			CDF_ASSERT(cdf_mc_timer_get_current_state
					   (&pAc->wmmInactivityTimer) ==
				   CDF_TIMER_STATE_STOPPED);
		}
	}

	return;
}

/**
 * hdd_wmm_enable_inactivity_timer() -
 *	function to enable the traffic inactivity timer for the given AC
 *
 * @pQosContext: [in] pointer to pQosContext
 * @inactivityTime: [in] value of the inactivity interval in millisecs
 *
 * When a QoS-Tspec is successfully setup, if the inactivity interval
 * time specified in the AddTS parameters is non-zero, this function
 * is invoked to start a traffic inactivity timer for the given AC.
 *
 * Return: CDF_STATUS enumeration
 */
static CDF_STATUS
hdd_wmm_enable_inactivity_timer(hdd_wmm_qos_context_t *pQosContext,
				uint32_t inactivityTime)
{
	CDF_STATUS cdf_status = CDF_STATUS_E_FAILURE;
	hdd_adapter_t *pAdapter = pQosContext->pAdapter;
	sme_ac_enum_type acType = pQosContext->acType;
	hdd_wmm_ac_status_t *pAc;

	pAdapter = pQosContext->pAdapter;
	pAc = &pAdapter->hddWmmStatus.wmmAcStatus[acType];

	cdf_status = cdf_mc_timer_init(&pAc->wmmInactivityTimer,
				       CDF_TIMER_TYPE_SW,
				       hdd_wmm_inactivity_timer_cb,
				       pQosContext);
	if (!CDF_IS_STATUS_SUCCESS(cdf_status)) {
		CDF_TRACE(CDF_MODULE_ID_HDD, CDF_TRACE_LEVEL_ERROR,
			  FL("Initializing inactivity timer failed on AC %d"),
			  acType);
		return cdf_status;
	}
	/* Start the inactivity timer */
	cdf_status = cdf_mc_timer_start(&pAc->wmmInactivityTimer,
					inactivityTime);
	if (!CDF_IS_STATUS_SUCCESS(cdf_status)) {
		CDF_TRACE(CDF_MODULE_ID_HDD, CDF_TRACE_LEVEL_ERROR,
			  FL("Starting inactivity timer failed on AC %d"),
			  acType);
		cdf_status = cdf_mc_timer_destroy(&pAc->wmmInactivityTimer);
		if (!CDF_IS_STATUS_SUCCESS(cdf_status)) {
			hdd_err("Failed to destroy inactivity timer");
		}
		return cdf_status;
	}
	pAc->wmmInactivityTime = inactivityTime;
	/* Initialize the current tx traffic count on this AC */
	pAc->wmmPrevTrafficCnt =
		pAdapter->hdd_stats.hddTxRxStats.txXmitClassifiedAC[pQosContext->
								    acType];
	pQosContext->is_inactivity_timer_running = true;
	return cdf_status;
}

/**
 * hdd_wmm_disable_inactivity_timer() -
 *	function to disable the traffic inactivity timer for the given AC.
 *
 * @pQosContext: [in] pointer to pQosContext
 *
 * This function is invoked to disable the traffic inactivity timer
 * for the given AC.  This is normally done when the TS is deleted.
 *
 * Return: CDF_STATUS enumeration
 */
static CDF_STATUS
hdd_wmm_disable_inactivity_timer(hdd_wmm_qos_context_t *pQosContext)
{
	hdd_adapter_t *pAdapter = pQosContext->pAdapter;
	sme_ac_enum_type acType = pQosContext->acType;
	hdd_wmm_ac_status_t *pAc = &pAdapter->hddWmmStatus.wmmAcStatus[acType];
	CDF_STATUS cdf_status = CDF_STATUS_E_FAILURE;

	/* Clear the timer and the counter */
	pAc->wmmInactivityTime = 0;
	pAc->wmmPrevTrafficCnt = 0;

	if (pQosContext->is_inactivity_timer_running == true) {
		pQosContext->is_inactivity_timer_running = false;
		cdf_status = cdf_mc_timer_stop(&pAc->wmmInactivityTimer);
		if (!CDF_IS_STATUS_SUCCESS(cdf_status)) {
			hdd_err("Failed to stop inactivity timer");
			return cdf_status;
		}
		cdf_status = cdf_mc_timer_destroy(&pAc->wmmInactivityTimer);
		if (!CDF_IS_STATUS_SUCCESS(cdf_status))
			hdd_err("Failed to destroy inactivity timer:Timer started");
	}

	return cdf_status;
}
#endif /* FEATURE_WLAN_ESE */

/**
 * hdd_wmm_sme_callback() - callback for QoS notifications
 *
 * @hHal: [in] the HAL handle
 * @hddCtx : [in] the HDD specified handle
 * @pCurrentQosInfo : [in] the TSPEC params
 * @smeStatus : [in] the QoS related SME status
 * @qosFlowId: [in] the unique identifier of the flow
 *
 * This callback is registered by HDD with SME for receiving QoS
 * notifications. Even though this function has a static scope it
 * gets called externally through some function pointer magic (so
 * there is a need for rigorous parameter checking).
 *
 * Return: CDF_STATUS enumeration
 */
static CDF_STATUS hdd_wmm_sme_callback(tHalHandle hHal,
				       void *hddCtx,
				       sme_QosWmmTspecInfo *pCurrentQosInfo,
				       sme_QosStatusType smeStatus,
				       uint32_t qosFlowId)
{
	hdd_wmm_qos_context_t *pQosContext = hddCtx;
	hdd_adapter_t *pAdapter;
	sme_ac_enum_type acType;
	hdd_wmm_ac_status_t *pAc;

	CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO_LOW,
		  "%s: Entered, context %p", __func__, pQosContext);

	if (unlikely((NULL == pQosContext) ||
		     (HDD_WMM_CTX_MAGIC != pQosContext->magic))) {
		CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_ERROR,
			  "%s: Invalid QoS Context", __func__);
		return CDF_STATUS_E_FAILURE;
	}

	pAdapter = pQosContext->pAdapter;
	acType = pQosContext->acType;
	pAc = &pAdapter->hddWmmStatus.wmmAcStatus[acType];

	CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO_LOW,
		  "%s: status %d flowid %d info %p",
		  __func__, smeStatus, qosFlowId, pCurrentQosInfo);

	switch (smeStatus) {

	case SME_QOS_STATUS_SETUP_SUCCESS_IND:
		CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO_LOW,
			  "%s: Setup is complete", __func__);

		/* there will always be a TSPEC returned with this
		 * status, even if a TSPEC is not exchanged OTA
		 */
		if (pCurrentQosInfo) {
			pAc->wmmAcTspecValid = true;
			memcpy(&pAc->wmmAcTspecInfo,
			       pCurrentQosInfo, sizeof(pAc->wmmAcTspecInfo));
		}
		pAc->wmmAcAccessAllowed = true;
		pAc->wmmAcAccessGranted = true;
		pAc->wmmAcAccessPending = false;
		pAc->wmmAcAccessFailed = false;

		if (HDD_WMM_HANDLE_IMPLICIT != pQosContext->handle) {

			CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO,
				  "%s: Explicit Qos, notifying user space",
				  __func__);

			/* this was triggered by an application */
			pQosContext->lastStatus =
				HDD_WLAN_WMM_STATUS_SETUP_SUCCESS;
			hdd_wmm_notify_app(pQosContext);
		}

#ifdef FEATURE_WLAN_ESE
		/* Check if the inactivity interval is specified */
		if (pCurrentQosInfo && pCurrentQosInfo->inactivity_interval) {
			CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO,
				  "%s: Inactivity timer value = %d for AC=%d",
				  __func__,
				  pCurrentQosInfo->inactivity_interval, acType);
			hdd_wmm_enable_inactivity_timer(pQosContext,
							pCurrentQosInfo->
							inactivity_interval);
		}
#endif /* FEATURE_WLAN_ESE */

		/* notify TL to enable trigger frames if necessary */
		hdd_wmm_enable_tl_uapsd(pQosContext);

		break;

	case SME_QOS_STATUS_SETUP_SUCCESS_APSD_SET_ALREADY:
		CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO,
			  "%s: Setup is complete (U-APSD set previously)",
			  __func__);

		pAc->wmmAcAccessAllowed = true;
		pAc->wmmAcAccessGranted = true;
		pAc->wmmAcAccessPending = false;

		if (HDD_WMM_HANDLE_IMPLICIT != pQosContext->handle) {

			CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO,
				  "%s: Explicit Qos, notifying user space",
				  __func__);

			/* this was triggered by an application */
			pQosContext->lastStatus =
				HDD_WLAN_WMM_STATUS_SETUP_SUCCESS_NO_ACM_UAPSD_EXISTING;
			hdd_wmm_notify_app(pQosContext);
		}

		break;

	case SME_QOS_STATUS_SETUP_FAILURE_RSP:
		CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_ERROR,
			  "%s: Setup failed", __func__);
		/* QoS setup failed */

		pAc->wmmAcAccessPending = false;
		pAc->wmmAcAccessFailed = true;
		pAc->wmmAcAccessAllowed = false;
		if (HDD_WMM_HANDLE_IMPLICIT != pQosContext->handle) {

			CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO,
				  "%s: Explicit Qos, notifying user space",
				  __func__);

			/* this was triggered by an application */
			pQosContext->lastStatus =
				HDD_WLAN_WMM_STATUS_SETUP_FAILED;

			hdd_wmm_notify_app(pQosContext);
		}

		/* Setting up QoS Failed, QoS context can be released.
		 * SME is releasing this flow information and if HDD
		 * doesn't release this context, next time if
		 * application uses the same handle to set-up QoS, HDD
		 * (as it has QoS context for this handle) will issue
		 * Modify QoS request to SME but SME will reject as now
		 * it has no information for this flow.
		 */
		hdd_wmm_free_context(pQosContext);
		break;

	case SME_QOS_STATUS_SETUP_INVALID_PARAMS_RSP:
		CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_ERROR,
			  "%s: Setup Invalid Params, notify TL", __func__);
		/* QoS setup failed */
		pAc->wmmAcAccessAllowed = false;

		if (HDD_WMM_HANDLE_IMPLICIT == pQosContext->handle) {

			/* we note the failure, but we also mark
			 * access as allowed so that the packets will
			 * flow.  Note that the MAC will "do the right
			 * thing"
			 */
			pAc->wmmAcAccessPending = false;
			pAc->wmmAcAccessFailed = true;
			pAc->wmmAcAccessAllowed = true;

		} else {
			CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO,
				  "%s: Explicit Qos, notifying user space",
				  __func__);

			/* this was triggered by an application */
			pQosContext->lastStatus =
				HDD_WLAN_WMM_STATUS_SETUP_FAILED_BAD_PARAM;
			hdd_wmm_notify_app(pQosContext);
		}
		break;

	case SME_QOS_STATUS_SETUP_NOT_QOS_AP_RSP:
		CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_ERROR,
			  "%s: Setup failed, not a QoS AP", __func__);
		if (HDD_WMM_HANDLE_IMPLICIT != pQosContext->handle) {
			CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO,
				  "%s: Explicit Qos, notifying user space",
				  __func__);

			/* this was triggered by an application */
			pQosContext->lastStatus =
				HDD_WLAN_WMM_STATUS_SETUP_FAILED_NO_WMM;
			hdd_wmm_notify_app(pQosContext);
		}
		break;

	case SME_QOS_STATUS_SETUP_REQ_PENDING_RSP:
		CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO,
			  "%s: Setup pending", __func__);
		/* not a callback status -- ignore if we get it */
		break;

	case SME_QOS_STATUS_SETUP_MODIFIED_IND:
		CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO,
			  "%s: Setup modified", __func__);
		if (pCurrentQosInfo) {
			/* update the TSPEC */
			pAc->wmmAcTspecValid = true;
			memcpy(&pAc->wmmAcTspecInfo,
			       pCurrentQosInfo, sizeof(pAc->wmmAcTspecInfo));

			if (HDD_WMM_HANDLE_IMPLICIT != pQosContext->handle) {
				CDF_TRACE(CDF_MODULE_ID_HDD,
					  WMM_TRACE_LEVEL_INFO,
					  "%s: Explicit Qos, notifying user space",
					  __func__);

				/* this was triggered by an application */
				pQosContext->lastStatus =
					HDD_WLAN_WMM_STATUS_MODIFIED;
				hdd_wmm_notify_app(pQosContext);
			}
			/* need to tell TL to update its UAPSD handling */
			hdd_wmm_enable_tl_uapsd(pQosContext);
		}
		break;

	case SME_QOS_STATUS_SETUP_SUCCESS_NO_ACM_NO_APSD_RSP:
		if (HDD_WMM_HANDLE_IMPLICIT == pQosContext->handle) {

			/* this was triggered by implicit QoS so we
			 * know packets are pending
			 */
			pAc->wmmAcAccessPending = false;
			pAc->wmmAcAccessGranted = true;
			pAc->wmmAcAccessAllowed = true;

		} else {
			CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO,
				  "%s: Explicit Qos, notifying user space",
				  __func__);

			/* this was triggered by an application */
			pQosContext->lastStatus =
				HDD_WLAN_WMM_STATUS_SETUP_SUCCESS_NO_ACM_NO_UAPSD;
			hdd_wmm_notify_app(pQosContext);
		}
		break;

	case SME_QOS_STATUS_SETUP_SUCCESS_IND_APSD_PENDING:
		/* nothing to do for now */
		break;

	case SME_QOS_STATUS_SETUP_SUCCESS_IND_APSD_SET_FAILED:
		CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_ERROR,
			  "%s: Setup successful but U-APSD failed", __func__);

		if (HDD_WMM_HANDLE_IMPLICIT == pQosContext->handle) {

			/* QoS setup was successful but setting U=APSD
			 * failed.  Since the OTA part of the request
			 * was successful, we don't mark this as a
			 * failure.  the packets will flow.  Note that
			 * the MAC will "do the right thing" */
			pAc->wmmAcAccessGranted = true;
			pAc->wmmAcAccessAllowed = true;
			pAc->wmmAcAccessFailed = false;
			pAc->wmmAcAccessPending = false;

		} else {
			CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO,
				  "%s: Explicit Qos, notifying user space",
				  __func__);

			/* this was triggered by an application */
			pQosContext->lastStatus =
				HDD_WLAN_WMM_STATUS_SETUP_UAPSD_SET_FAILED;
			hdd_wmm_notify_app(pQosContext);
		}

		/* Since U-APSD portion failed disabled trigger frame
		 * generation
		 */
		hdd_wmm_disable_tl_uapsd(pQosContext);

		break;

	case SME_QOS_STATUS_RELEASE_SUCCESS_RSP:
		CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO,
			  "%s: Release is complete", __func__);

		if (pCurrentQosInfo) {
			CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO,
				  "%s: flows still active", __func__);

			/* there is still at least one flow active for
			 * this AC so update the AC state
			 */
			memcpy(&pAc->wmmAcTspecInfo,
			       pCurrentQosInfo, sizeof(pAc->wmmAcTspecInfo));

			/* need to tell TL to update its UAPSD handling */
			hdd_wmm_enable_tl_uapsd(pQosContext);
		} else {
			CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO,
				  "%s: last flow", __func__);

			/* this is the last flow active for this AC so
			 * update the AC state
			 */
			pAc->wmmAcTspecValid = false;

			/* DELTS is successful, do not allow */
			pAc->wmmAcAccessAllowed = false;

			/* need to tell TL to update its UAPSD handling */
			hdd_wmm_disable_tl_uapsd(pQosContext);
		}

		if (HDD_WMM_HANDLE_IMPLICIT != pQosContext->handle) {
			CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO,
				  "%s: Explicit Qos, notifying user space",
				  __func__);

			/* this was triggered by an application */
			pQosContext->lastStatus =
				HDD_WLAN_WMM_STATUS_RELEASE_SUCCESS;
			hdd_wmm_notify_app(pQosContext);
		}
		/* we are done with this flow */
		hdd_wmm_free_context(pQosContext);
		break;

	case SME_QOS_STATUS_RELEASE_FAILURE_RSP:
		CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO,
			  "%s: Release failure", __func__);

		/* we don't need to update our state or TL since
		 * nothing has changed
		 */
		if (HDD_WMM_HANDLE_IMPLICIT != pQosContext->handle) {
			CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO,
				  "%s: Explicit Qos, notifying user space",
				  __func__);

			/* this was triggered by an application */
			pQosContext->lastStatus =
				HDD_WLAN_WMM_STATUS_RELEASE_FAILED;
			hdd_wmm_notify_app(pQosContext);
		}

		break;

	case SME_QOS_STATUS_RELEASE_QOS_LOST_IND:
		CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO,
			  "%s: QOS Lost indication received", __func__);

		/* current TSPEC is no longer valid */
		pAc->wmmAcTspecValid = false;
		/* AP has sent DELTS, do not allow */
		pAc->wmmAcAccessAllowed = false;

		/* need to tell TL to update its UAPSD handling */
		hdd_wmm_disable_tl_uapsd(pQosContext);

		if (HDD_WMM_HANDLE_IMPLICIT == pQosContext->handle) {
			/* we no longer have implicit access granted */
			pAc->wmmAcAccessGranted = false;
			pAc->wmmAcAccessFailed = false;
		} else {
			CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO_LOW,
				  "%s: Explicit Qos, notifying user space",
				  __func__);

			/* this was triggered by an application */
			pQosContext->lastStatus = HDD_WLAN_WMM_STATUS_LOST;
			hdd_wmm_notify_app(pQosContext);
		}

		/* we are done with this flow */
		hdd_wmm_free_context(pQosContext);
		break;

	case SME_QOS_STATUS_RELEASE_REQ_PENDING_RSP:
		CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO_LOW,
			  "%s: Release pending", __func__);
		/* not a callback status -- ignore if we get it */
		break;

	case SME_QOS_STATUS_RELEASE_INVALID_PARAMS_RSP:
		CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_ERROR,
			  "%s: Release Invalid Params", __func__);
		if (HDD_WMM_HANDLE_IMPLICIT != pQosContext->handle) {
			/* this was triggered by an application */
			pQosContext->lastStatus =
				HDD_WLAN_WMM_STATUS_RELEASE_FAILED_BAD_PARAM;
			hdd_wmm_notify_app(pQosContext);
		}
		break;

	case SME_QOS_STATUS_MODIFY_SETUP_SUCCESS_IND:
		CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO_LOW,
			  "%s: Modification is complete, notify TL", __func__);

		/* there will always be a TSPEC returned with this
		 * status, even if a TSPEC is not exchanged OTA
		 */
		if (pCurrentQosInfo) {
			pAc->wmmAcTspecValid = true;
			memcpy(&pAc->wmmAcTspecInfo,
			       pCurrentQosInfo, sizeof(pAc->wmmAcTspecInfo));
		}

		if (HDD_WMM_HANDLE_IMPLICIT != pQosContext->handle) {
			/* this was triggered by an application */
			pQosContext->lastStatus =
				HDD_WLAN_WMM_STATUS_MODIFY_SUCCESS;
			hdd_wmm_notify_app(pQosContext);
		}
		/* notify TL to enable trigger frames if necessary */
		hdd_wmm_enable_tl_uapsd(pQosContext);

		break;

	case SME_QOS_STATUS_MODIFY_SETUP_SUCCESS_APSD_SET_ALREADY:
		if (HDD_WMM_HANDLE_IMPLICIT != pQosContext->handle) {
			/* this was triggered by an application */
			pQosContext->lastStatus =
				HDD_WLAN_WMM_STATUS_MODIFY_SUCCESS_NO_ACM_UAPSD_EXISTING;
			hdd_wmm_notify_app(pQosContext);
		}
		break;

	case SME_QOS_STATUS_MODIFY_SETUP_FAILURE_RSP:
		/* the flow modification failed so we'll leave in
		 * place whatever existed beforehand
		 */

		if (HDD_WMM_HANDLE_IMPLICIT != pQosContext->handle) {
			/* this was triggered by an application */
			pQosContext->lastStatus =
				HDD_WLAN_WMM_STATUS_MODIFY_FAILED;
			hdd_wmm_notify_app(pQosContext);
		}
		break;

	case SME_QOS_STATUS_MODIFY_SETUP_PENDING_RSP:
		CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO_LOW,
			  "%s: modification pending", __func__);
		/* not a callback status -- ignore if we get it */
		break;

	case SME_QOS_STATUS_MODIFY_SETUP_SUCCESS_NO_ACM_NO_APSD_RSP:
		/* the flow modification was successful but no QoS
		 * changes required
		 */

		if (HDD_WMM_HANDLE_IMPLICIT != pQosContext->handle) {
			/* this was triggered by an application */
			pQosContext->lastStatus =
				HDD_WLAN_WMM_STATUS_MODIFY_SUCCESS_NO_ACM_NO_UAPSD;
			hdd_wmm_notify_app(pQosContext);
		}
		break;

	case SME_QOS_STATUS_MODIFY_SETUP_INVALID_PARAMS_RSP:
		/* invalid params -- notify the application */
		if (HDD_WMM_HANDLE_IMPLICIT != pQosContext->handle) {
			/* this was triggered by an application */
			pQosContext->lastStatus =
				HDD_WLAN_WMM_STATUS_MODIFY_FAILED_BAD_PARAM;
			hdd_wmm_notify_app(pQosContext);
		}
		break;

	case SME_QOS_STATUS_MODIFY_SETUP_SUCCESS_IND_APSD_PENDING:
		/* nothing to do for now.  when APSD is established we'll have work to do */
		break;

	case SME_QOS_STATUS_MODIFY_SETUP_SUCCESS_IND_APSD_SET_FAILED:
		CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_ERROR,
			  "%s: Modify successful but U-APSD failed", __func__);

		/* QoS modification was successful but setting U=APSD
		 * failed.  This will always be an explicit QoS
		 * instance, so all we can do is notify the
		 * application and let it clean up.
		 */
		if (HDD_WMM_HANDLE_IMPLICIT != pQosContext->handle) {
			/* this was triggered by an application */
			pQosContext->lastStatus =
				HDD_WLAN_WMM_STATUS_MODIFY_UAPSD_SET_FAILED;
			hdd_wmm_notify_app(pQosContext);
		}
		/* Since U-APSD portion failed disabled trigger frame
		 * generation
		 */
		hdd_wmm_disable_tl_uapsd(pQosContext);

		break;

	case SME_QOS_STATUS_HANDING_OFF:
		/* no roaming so we won't see this */
		break;

	case SME_QOS_STATUS_OUT_OF_APSD_POWER_MODE_IND:
		/* need to tell TL to stop trigger frame generation */
		hdd_wmm_disable_tl_uapsd(pQosContext);
		break;

	case SME_QOS_STATUS_INTO_APSD_POWER_MODE_IND:
		/* need to tell TL to start sending trigger frames again */
		hdd_wmm_enable_tl_uapsd(pQosContext);
		break;

	default:
		CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_ERROR,
			  "%s: unexpected SME Status=%d", __func__, smeStatus);
		CDF_ASSERT(0);
	}

	/* if Tspec only allows downstream traffic then access is not
	 * allowed
	 */
	if (pAc->wmmAcTspecValid &&
	    (pAc->wmmAcTspecInfo.ts_info.direction ==
	     SME_QOS_WMM_TS_DIR_DOWNLINK)) {
		pAc->wmmAcAccessAllowed = false;
	}
	/* if we have valid Tpsec or if ACM bit is not set, allow access */
	if ((pAc->wmmAcTspecValid &&
	     (pAc->wmmAcTspecInfo.ts_info.direction !=
	      SME_QOS_WMM_TS_DIR_DOWNLINK)) || !pAc->wmmAcAccessRequired) {
		pAc->wmmAcAccessAllowed = true;
	}

	CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO,
		  "%s: complete, access for TL AC %d is%sallowed",
		  __func__, acType, pAc->wmmAcAccessAllowed ? " " : " not ");

	return CDF_STATUS_SUCCESS;
}
#endif

/**
 * hdd_wmmps_helper() - Function to set uapsd psb dynamically
 *
 * @pAdapter: [in] pointer to adapter structure
 * @ptr: [in] pointer to command buffer
 *
 * Return: Zero on success, appropriate error on failure.
 */
int hdd_wmmps_helper(hdd_adapter_t *pAdapter, uint8_t *ptr)
{
	if (NULL == pAdapter) {
		CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_ERROR,
			  "%s: pAdapter is NULL", __func__);
		return -EINVAL;
	}
	if (NULL == ptr) {
		CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_ERROR,
			  "%s: ptr is NULL", __func__);
		return -EINVAL;
	}
	/* convert ASCII to integer */
	pAdapter->configuredPsb = ptr[9] - '0';
	pAdapter->psbChanged = HDD_PSB_CHANGED;

	return 0;
}

/**
 * __hdd_wmm_do_implicit_qos() - Function which will attempt to setup
 *				QoS for any AC requiring it.
 * @work: [in] pointer to work structure.
 *
 * Return: none
 */
static void __hdd_wmm_do_implicit_qos(struct work_struct *work)
{
	hdd_wmm_qos_context_t *pQosContext =
		container_of(work, hdd_wmm_qos_context_t, wmmAcSetupImplicitQos);
	hdd_adapter_t *pAdapter;
	sme_ac_enum_type acType;
	hdd_wmm_ac_status_t *pAc;
#ifndef WLAN_MDM_CODE_REDUCTION_OPT
	sme_QosStatusType smeStatus;
#endif
	sme_QosWmmTspecInfo qosInfo;
	hdd_context_t *hdd_ctx;

	CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO_LOW,
		  "%s: Entered, context %p", __func__, pQosContext);

	if (unlikely(HDD_WMM_CTX_MAGIC != pQosContext->magic)) {
		CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_ERROR,
			  "%s: Invalid QoS Context", __func__);
		return;
	}

	pAdapter = pQosContext->pAdapter;

	hdd_ctx = WLAN_HDD_GET_CTX(pAdapter);
	if (0 != wlan_hdd_validate_context(hdd_ctx)) {
		hddLog(LOGE, FL("HDD context is not valid"));
		return;
	}

	acType = pQosContext->acType;
	pAc = &pAdapter->hddWmmStatus.wmmAcStatus[acType];

	CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO_LOW,
		  "%s: pAdapter %p acType %d", __func__, pAdapter, acType);

	if (!pAc->wmmAcAccessNeeded) {
		CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_ERROR,
			  "%s: AC %d doesn't need service", __func__, acType);
		pQosContext->magic = 0;
		kfree(pQosContext);
		return;
	}

	pAc->wmmAcAccessPending = true;
	pAc->wmmAcAccessNeeded = false;

	memset(&qosInfo, 0, sizeof(qosInfo));

	qosInfo.ts_info.psb = pAdapter->configuredPsb;

	switch (acType) {
	case SME_AC_VO:
		qosInfo.ts_info.up = SME_QOS_WMM_UP_VO;
		/* Check if there is any valid configuration from framework */
		if (HDD_PSB_CFG_INVALID == pAdapter->configuredPsb) {
			qosInfo.ts_info.psb =
				((WLAN_HDD_GET_CTX(pAdapter))->config->
				 UapsdMask & SME_QOS_UAPSD_VO) ? 1 : 0;
		}
		qosInfo.ts_info.direction =
			(WLAN_HDD_GET_CTX(pAdapter))->config->InfraDirAcVo;
		qosInfo.ts_info.tid = 255;
		qosInfo.mean_data_rate =
			(WLAN_HDD_GET_CTX(pAdapter))->config->
			InfraMeanDataRateAcVo;
		qosInfo.min_phy_rate =
			(WLAN_HDD_GET_CTX(pAdapter))->config->InfraMinPhyRateAcVo;
		qosInfo.min_service_interval =
			(WLAN_HDD_GET_CTX(pAdapter))->config->InfraUapsdVoSrvIntv;
		qosInfo.nominal_msdu_size =
			(WLAN_HDD_GET_CTX(pAdapter))->config->InfraNomMsduSizeAcVo;
		qosInfo.surplus_bw_allowance =
			(WLAN_HDD_GET_CTX(pAdapter))->config->InfraSbaAcVo;
		qosInfo.suspension_interval =
			(WLAN_HDD_GET_CTX(pAdapter))->config->InfraUapsdVoSuspIntv;
		break;
	case SME_AC_VI:
		qosInfo.ts_info.up = SME_QOS_WMM_UP_VI;
		/* Check if there is any valid configuration from framework */
		if (HDD_PSB_CFG_INVALID == pAdapter->configuredPsb) {
			qosInfo.ts_info.psb =
				((WLAN_HDD_GET_CTX(pAdapter))->config->
				 UapsdMask & SME_QOS_UAPSD_VI) ? 1 : 0;
		}
		qosInfo.ts_info.direction =
			(WLAN_HDD_GET_CTX(pAdapter))->config->InfraDirAcVi;
		qosInfo.ts_info.tid = 255;
		qosInfo.mean_data_rate =
			(WLAN_HDD_GET_CTX(pAdapter))->config->
			InfraMeanDataRateAcVi;
		qosInfo.min_phy_rate =
			(WLAN_HDD_GET_CTX(pAdapter))->config->InfraMinPhyRateAcVi;
		qosInfo.min_service_interval =
			(WLAN_HDD_GET_CTX(pAdapter))->config->InfraUapsdViSrvIntv;
		qosInfo.nominal_msdu_size =
			(WLAN_HDD_GET_CTX(pAdapter))->config->InfraNomMsduSizeAcVi;
		qosInfo.surplus_bw_allowance =
			(WLAN_HDD_GET_CTX(pAdapter))->config->InfraSbaAcVi;
		qosInfo.suspension_interval =
			(WLAN_HDD_GET_CTX(pAdapter))->config->InfraUapsdViSuspIntv;
		break;
	default:
	case SME_AC_BE:
		qosInfo.ts_info.up = SME_QOS_WMM_UP_BE;
		/* Check if there is any valid configuration from framework */
		if (HDD_PSB_CFG_INVALID == pAdapter->configuredPsb) {
			qosInfo.ts_info.psb =
				((WLAN_HDD_GET_CTX(pAdapter))->config->
				 UapsdMask & SME_QOS_UAPSD_BE) ? 1 : 0;
		}
		qosInfo.ts_info.direction =
			(WLAN_HDD_GET_CTX(pAdapter))->config->InfraDirAcBe;
		qosInfo.ts_info.tid = 255;
		qosInfo.mean_data_rate =
			(WLAN_HDD_GET_CTX(pAdapter))->config->
			InfraMeanDataRateAcBe;
		qosInfo.min_phy_rate =
			(WLAN_HDD_GET_CTX(pAdapter))->config->InfraMinPhyRateAcBe;
		qosInfo.min_service_interval =
			(WLAN_HDD_GET_CTX(pAdapter))->config->InfraUapsdBeSrvIntv;
		qosInfo.nominal_msdu_size =
			(WLAN_HDD_GET_CTX(pAdapter))->config->InfraNomMsduSizeAcBe;
		qosInfo.surplus_bw_allowance =
			(WLAN_HDD_GET_CTX(pAdapter))->config->InfraSbaAcBe;
		qosInfo.suspension_interval =
			(WLAN_HDD_GET_CTX(pAdapter))->config->InfraUapsdBeSuspIntv;
		break;
	case SME_AC_BK:
		qosInfo.ts_info.up = SME_QOS_WMM_UP_BK;
		/* Check if there is any valid configuration from framework */
		if (HDD_PSB_CFG_INVALID == pAdapter->configuredPsb) {
			qosInfo.ts_info.psb =
				((WLAN_HDD_GET_CTX(pAdapter))->config->
				 UapsdMask & SME_QOS_UAPSD_BK) ? 1 : 0;
		}
		qosInfo.ts_info.direction =
			(WLAN_HDD_GET_CTX(pAdapter))->config->InfraDirAcBk;
		qosInfo.ts_info.tid = 255;
		qosInfo.mean_data_rate =
			(WLAN_HDD_GET_CTX(pAdapter))->config->
			InfraMeanDataRateAcBk;
		qosInfo.min_phy_rate =
			(WLAN_HDD_GET_CTX(pAdapter))->config->InfraMinPhyRateAcBk;
		qosInfo.min_service_interval =
			(WLAN_HDD_GET_CTX(pAdapter))->config->InfraUapsdBkSrvIntv;
		qosInfo.nominal_msdu_size =
			(WLAN_HDD_GET_CTX(pAdapter))->config->InfraNomMsduSizeAcBk;
		qosInfo.surplus_bw_allowance =
			(WLAN_HDD_GET_CTX(pAdapter))->config->InfraSbaAcBk;
		qosInfo.suspension_interval =
			(WLAN_HDD_GET_CTX(pAdapter))->config->InfraUapsdBkSuspIntv;
		break;
	}
#ifdef FEATURE_WLAN_ESE
	qosInfo.inactivity_interval =
		(WLAN_HDD_GET_CTX(pAdapter))->config->InfraInactivityInterval;
#endif
	qosInfo.ts_info.burst_size_defn =
		(WLAN_HDD_GET_CTX(pAdapter))->config->burstSizeDefinition;

	switch ((WLAN_HDD_GET_CTX(pAdapter))->config->tsInfoAckPolicy) {
	case HDD_WLAN_WMM_TS_INFO_ACK_POLICY_NORMAL_ACK:
		qosInfo.ts_info.ack_policy =
			SME_QOS_WMM_TS_ACK_POLICY_NORMAL_ACK;
		break;

	case HDD_WLAN_WMM_TS_INFO_ACK_POLICY_HT_IMMEDIATE_BLOCK_ACK:
		qosInfo.ts_info.ack_policy =
			SME_QOS_WMM_TS_ACK_POLICY_HT_IMMEDIATE_BLOCK_ACK;
		break;

	default:
		/* unknown */
		qosInfo.ts_info.ack_policy =
			SME_QOS_WMM_TS_ACK_POLICY_NORMAL_ACK;
	}

	if (qosInfo.ts_info.ack_policy ==
	    SME_QOS_WMM_TS_ACK_POLICY_HT_IMMEDIATE_BLOCK_ACK) {
		if (!sme_qos_is_ts_info_ack_policy_valid
			    ((tpAniSirGlobal) WLAN_HDD_GET_HAL_CTX(pAdapter), &qosInfo,
			    pAdapter->sessionId)) {
			qosInfo.ts_info.ack_policy =
				SME_QOS_WMM_TS_ACK_POLICY_NORMAL_ACK;
		}
	}

	mutex_lock(&pAdapter->hddWmmStatus.wmmLock);
	list_add(&pQosContext->node, &pAdapter->hddWmmStatus.wmmContextList);
	mutex_unlock(&pAdapter->hddWmmStatus.wmmLock);

#ifndef WLAN_MDM_CODE_REDUCTION_OPT
	smeStatus = sme_qos_setup_req(WLAN_HDD_GET_HAL_CTX(pAdapter),
				      pAdapter->sessionId,
				      &qosInfo,
				      hdd_wmm_sme_callback,
				      pQosContext,
				      qosInfo.ts_info.up,
				      &pQosContext->qosFlowId);

	CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO,
		  "%s: sme_qos_setup_req returned %d flowid %d",
		  __func__, smeStatus, pQosContext->qosFlowId);

	/* need to check the return values and act appropriately */
	switch (smeStatus) {
	case SME_QOS_STATUS_SETUP_REQ_PENDING_RSP:
	case SME_QOS_STATUS_SETUP_SUCCESS_IND_APSD_PENDING:
		/* setup is pending, so no more work to do now.  all
		 * further work will be done in hdd_wmm_sme_callback()
		 */
		CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO_LOW,
			  "%s: Setup is pending, no further work", __func__);

		break;

	case SME_QOS_STATUS_SETUP_FAILURE_RSP:
		/* we can't tell the difference between when a request
		 * fails because AP rejected it versus when SME
		 * encountered an internal error.  in either case SME
		 * won't ever reference this context so free the
		 * record
		 */
		hdd_wmm_free_context(pQosContext);

		/* fall through and start packets flowing */
	case SME_QOS_STATUS_SETUP_SUCCESS_NO_ACM_NO_APSD_RSP:
		/* no ACM in effect, no need to setup U-APSD */
	case SME_QOS_STATUS_SETUP_SUCCESS_APSD_SET_ALREADY:
		/* no ACM in effect, U-APSD is desired but was already setup */

		/* for these cases everything is already setup so we
		 * can signal TL that it has work to do
		 */
		CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO_LOW,
			  "%s: Setup is complete, notify TL", __func__);

		pAc->wmmAcAccessAllowed = true;
		pAc->wmmAcAccessGranted = true;
		pAc->wmmAcAccessPending = false;

		break;

	default:
		CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_ERROR,
			  "%s: unexpected SME Status=%d", __func__, smeStatus);
		CDF_ASSERT(0);
	}
#endif

}

/**
 * hdd_wmm_do_implicit_qos() - SSR wraper function for hdd_wmm_do_implicit_qos
 * @work: pointer to work_struct
 *
 * Return: none
 */
static void hdd_wmm_do_implicit_qos(struct work_struct *work)
{
	cds_ssr_protect(__func__);
	__hdd_wmm_do_implicit_qos(work);
	cds_ssr_unprotect(__func__);
}

/**
 * hdd_wmm_init() - initialize the WMM DSCP configuation
 * @pAdapter : [in]  pointer to Adapter context
 *
 * This function will initialize the WMM DSCP configuation of an
 * adapter to an initial state.  The configuration can later be
 * overwritten via application APIs or via QoS Map sent OTA.
 *
 * Return: CDF_STATUS enumeration
 */
CDF_STATUS hdd_wmm_init(hdd_adapter_t *pAdapter)
{
	sme_QosWmmUpType *hddWmmDscpToUpMap = pAdapter->hddWmmDscpToUpMap;
	uint8_t dscp;

	CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO_LOW,
		  "%s: Entered", __func__);

	/* DSCP to User Priority Lookup Table
	 * By default use the 3 Precedence bits of DSCP as the User Priority
	 */
	for (dscp = 0; dscp <= WLAN_HDD_MAX_DSCP; dscp++) {
		hddWmmDscpToUpMap[dscp] = dscp >> 3;
	}

	/* Special case for Expedited Forwarding (DSCP 46) */
	hddWmmDscpToUpMap[46] = SME_QOS_WMM_UP_VO;

	return CDF_STATUS_SUCCESS;
}

/**
 * hdd_wmm_adapter_init() - initialize the WMM configuration of an adapter
 * @pAdapter: [in]  pointer to Adapter context
 *
 * This function will initialize the WMM configuation and status of an
 * adapter to an initial state.  The configuration can later be
 * overwritten via application APIs
 *
 * Return: CDF_STATUS enumeration
 */
CDF_STATUS hdd_wmm_adapter_init(hdd_adapter_t *pAdapter)
{
	hdd_wmm_ac_status_t *pAcStatus;
	sme_ac_enum_type acType;

	CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO_LOW,
		  "%s: Entered", __func__);

	pAdapter->hddWmmStatus.wmmQap = false;
	INIT_LIST_HEAD(&pAdapter->hddWmmStatus.wmmContextList);
	mutex_init(&pAdapter->hddWmmStatus.wmmLock);

	for (acType = 0; acType < WLAN_MAX_AC; acType++) {
		pAcStatus = &pAdapter->hddWmmStatus.wmmAcStatus[acType];
		pAcStatus->wmmAcAccessRequired = false;
		pAcStatus->wmmAcAccessNeeded = false;
		pAcStatus->wmmAcAccessPending = false;
		pAcStatus->wmmAcAccessFailed = false;
		pAcStatus->wmmAcAccessGranted = false;
		pAcStatus->wmmAcAccessAllowed = false;
		pAcStatus->wmmAcTspecValid = false;
		pAcStatus->wmmAcUapsdInfoValid = false;
	}
	/* Invalid value(0xff) to indicate psb not configured through
	 * framework initially.
	 */
	pAdapter->configuredPsb = HDD_PSB_CFG_INVALID;

	return CDF_STATUS_SUCCESS;
}

/**
 * hdd_wmm_adapter_clear() - Function which will clear the WMM status
 * for all the ACs
 *
 * @pAdapter: [in]  pointer to Adapter context
 *
 * Return: CDF_STATUS enumeration
 */
CDF_STATUS hdd_wmm_adapter_clear(hdd_adapter_t *pAdapter)
{
	hdd_wmm_ac_status_t *pAcStatus;
	sme_ac_enum_type acType;
	CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO_LOW,
		  "%s: Entered", __func__);
	for (acType = 0; acType < WLAN_MAX_AC; acType++) {
		pAcStatus = &pAdapter->hddWmmStatus.wmmAcStatus[acType];
		pAcStatus->wmmAcAccessRequired = false;
		pAcStatus->wmmAcAccessNeeded = false;
		pAcStatus->wmmAcAccessPending = false;
		pAcStatus->wmmAcAccessFailed = false;
		pAcStatus->wmmAcAccessGranted = false;
		pAcStatus->wmmAcAccessAllowed = false;
		pAcStatus->wmmAcTspecValid = false;
		pAcStatus->wmmAcUapsdInfoValid = false;
	}
	return CDF_STATUS_SUCCESS;
}

/**
 * hdd_wmm_close() - WMM close function
 * @pAdapter: [in]  pointer to adapter context
 *
 * Function which will perform any necessary work to to clean up the
 * WMM functionality prior to the kernel module unload.
 *
 * Return: CDF_STATUS enumeration
 */
CDF_STATUS hdd_wmm_adapter_close(hdd_adapter_t *pAdapter)
{
	hdd_wmm_qos_context_t *pQosContext;

	CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO_LOW,
		  "%s: Entered", __func__);

	/* free any context records that we still have linked */
	while (!list_empty(&pAdapter->hddWmmStatus.wmmContextList)) {
		pQosContext =
			list_first_entry(&pAdapter->hddWmmStatus.wmmContextList,
					 hdd_wmm_qos_context_t, node);
#ifdef FEATURE_WLAN_ESE
		hdd_wmm_disable_inactivity_timer(pQosContext);
#endif
		if (pQosContext->handle == HDD_WMM_HANDLE_IMPLICIT
			&& pQosContext->magic == HDD_WMM_CTX_MAGIC)
			cds_flush_work(&pQosContext->wmmAcSetupImplicitQos);

		hdd_wmm_free_context(pQosContext);
	}

	return CDF_STATUS_SUCCESS;
}

/**
 * hdd_wmm_classify_pkt() - Function which will classify an OS packet
 * into a WMM AC based on DSCP
 *
 * @adapter: adapter upon which the packet is being transmitted
 * @skb: pointer to network buffer
 * @user_pri: user priority of the OS packet
 * @is_eapol: eapol packet flag
 *
 * Return: None
 */
static
void hdd_wmm_classify_pkt(hdd_adapter_t *adapter,
			  struct sk_buff *skb,
			  sme_QosWmmUpType *user_pri,
			  bool *is_eapol)
{
	unsigned char dscp;
	unsigned char tos;
	union generic_ethhdr *eth_hdr;
	struct iphdr *ip_hdr;
	struct ipv6hdr *ipv6hdr;
	unsigned char *pkt;

	/* this code is executed for every packet therefore
	 * all debug code is kept conditional
	 */

#ifdef HDD_WMM_DEBUG
	CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO_LOW,
		  "%s: Entered", __func__);
#endif /* HDD_WMM_DEBUG */

	pkt = skb->data;
	eth_hdr = (union generic_ethhdr *)pkt;

#ifdef HDD_WMM_DEBUG
	CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO_LOW,
		  "%s: proto is 0x%04x", __func__, skb->protocol);
#endif /* HDD_WMM_DEBUG */

	if (eth_hdr->eth_II.h_proto == htons(ETH_P_IP)) {
		/* case 1: Ethernet II IP packet */
		ip_hdr = (struct iphdr *)&pkt[sizeof(eth_hdr->eth_II)];
		tos = ip_hdr->tos;
#ifdef HDD_WMM_DEBUG
		CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO_LOW,
				"%s: Ethernet II IP Packet, tos is %d",
				__func__, tos);
#endif /* HDD_WMM_DEBUG */

	} else if (eth_hdr->eth_II.h_proto == htons(ETH_P_IPV6)) {
		ipv6hdr = ipv6_hdr(skb);
		tos = ntohs(*(const __be16 *)ipv6hdr) >> 4;
#ifdef HDD_WMM_DEBUG
		CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO_LOW,
				"%s: Ethernet II IPv6 Packet, tos is %d",
				__func__, tos);
#endif /* HDD_WMM_DEBUG */
	} else if ((ntohs(eth_hdr->eth_II.h_proto) < WLAN_MIN_PROTO) &&
		  (eth_hdr->eth_8023.h_snap.dsap == WLAN_SNAP_DSAP) &&
		  (eth_hdr->eth_8023.h_snap.ssap == WLAN_SNAP_SSAP) &&
		  (eth_hdr->eth_8023.h_snap.ctrl == WLAN_SNAP_CTRL) &&
		  (eth_hdr->eth_8023.h_proto == htons(ETH_P_IP))) {
		/* case 2: 802.3 LLC/SNAP IP packet */
		ip_hdr = (struct iphdr *)&pkt[sizeof(eth_hdr->eth_8023)];
		tos = ip_hdr->tos;
#ifdef HDD_WMM_DEBUG
		CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO_LOW,
				"%s: 802.3 LLC/SNAP IP Packet, tos is %d",
				__func__, tos);
#endif /* HDD_WMM_DEBUG */
	} else if (eth_hdr->eth_II.h_proto == htons(ETH_P_8021Q)) {
		/* VLAN tagged */

		if (eth_hdr->eth_IIv.h_vlan_encapsulated_proto ==
			htons(ETH_P_IP)) {
			/* case 3: Ethernet II vlan-tagged IP packet */
			ip_hdr =
				(struct iphdr *)
				&pkt[sizeof(eth_hdr->eth_IIv)];
			tos = ip_hdr->tos;
#ifdef HDD_WMM_DEBUG
			CDF_TRACE(CDF_MODULE_ID_HDD,
				WMM_TRACE_LEVEL_INFO_LOW,
				"%s: Ethernet II VLAN tagged IP Packet, tos is %d",
				__func__, tos);
#endif /* HDD_WMM_DEBUG */
		} else
		if ((ntohs(eth_hdr->eth_IIv.h_vlan_encapsulated_proto)
			< WLAN_MIN_PROTO)
		    && (eth_hdr->eth_8023v.h_snap.dsap ==
			WLAN_SNAP_DSAP)
			&& (eth_hdr->eth_8023v.h_snap.ssap ==
			WLAN_SNAP_SSAP)
			&& (eth_hdr->eth_8023v.h_snap.ctrl ==
			WLAN_SNAP_CTRL)
			&& (eth_hdr->eth_8023v.h_proto ==
			htons(ETH_P_IP))) {
			/* case 4: 802.3 LLC/SNAP vlan-tagged IP packet */
			ip_hdr =
				(struct iphdr *)
				&pkt[sizeof(eth_hdr->eth_8023v)];
			tos = ip_hdr->tos;
#ifdef HDD_WMM_DEBUG
			CDF_TRACE(CDF_MODULE_ID_HDD,
				WMM_TRACE_LEVEL_INFO_LOW,
				"%s: 802.3 LLC/SNAP VLAN tagged IP Packet, tos is %d",
				__func__, tos);
#endif /* HDD_WMM_DEBUG */
		} else {
			/* default */
#ifdef HDD_WMM_DEBUG
			CDF_TRACE(CDF_MODULE_ID_HDD,
				WMM_TRACE_LEVEL_WARN,
				"%s: VLAN tagged Unhandled Protocol, using default tos",
				__func__);
#endif /* HDD_WMM_DEBUG */
			tos = 0;
		}
	} else {
		/* default */
#ifdef HDD_WMM_DEBUG
		CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_WARN,
			"%s: Unhandled Protocol, using default tos",
			__func__);
#endif /* HDD_WMM_DEBUG */
		/* Give the highest priority to 802.1x packet */
		if (eth_hdr->eth_II.h_proto ==
			htons(HDD_ETHERTYPE_802_1_X)) {
			tos = 0xC0;
			*is_eapol = true;
		} else
			tos = 0;
	}

	dscp = (tos >> 2) & 0x3f;
	*user_pri = adapter->hddWmmDscpToUpMap[dscp];

#ifdef HDD_WMM_DEBUG
	CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO,
		"%s: tos is %d, dscp is %d, up is %d",
		__func__, tos, dscp, *user_pri);
#endif /* HDD_WMM_DEBUG */

	return;
}

#ifdef QCA_LL_TX_FLOW_CONTROL_V2
/**
 * hdd_get_queue_index() - get queue index
 * @up: user priority
 * @is_eapol: is_eapol flag
 *
 * Return: queue_index
 */
static
uint16_t hdd_get_queue_index(uint16_t up, bool is_eapol)
{
	if (cdf_unlikely(is_eapol == true))
		return HDD_LINUX_AC_HI_PRIO;
	else
		return hdd_linux_up_to_ac_map[up];
}
#else
static
uint16_t hdd_get_queue_index(uint16_t up, bool is_eapol)
{
	return hdd_linux_up_to_ac_map[up];
}
#endif


/**
 * hdd_hostapd_select_queue() - Function which will classify the packet
 *       according to linux qdisc expectation.
 *
 * @dev: [in] pointer to net_device structure
 * @skb: [in] pointer to os packet
 *
 * Return: Qdisc queue index
 */
uint16_t hdd_hostapd_select_queue(struct net_device *dev, struct sk_buff *skb
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3, 13, 0))
				  , void *accel_priv
#endif
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3, 14, 0))
				  , select_queue_fallback_t fallback
#endif

)
{
	sme_QosWmmUpType up = SME_QOS_WMM_UP_BE;
	uint16_t queueIndex;
	hdd_adapter_t *adapter = (hdd_adapter_t *) netdev_priv(dev);
	hdd_context_t *hddctx = WLAN_HDD_GET_CTX(adapter);
	bool is_eapol = false;
	int status = 0;
	status = wlan_hdd_validate_context(hddctx);

	if (status != 0) {
		skb->priority = SME_QOS_WMM_UP_BE;
		return HDD_LINUX_AC_BE;
	}

	/* Get the user priority from IP header */
	hdd_wmm_classify_pkt(adapter, skb, &up, &is_eapol);
	skb->priority = up;
	queueIndex = hdd_get_queue_index(skb->priority, is_eapol);

	return queueIndex;
}

/**
 * hdd_wmm_select_queue() - Function which will classify the packet
 *       according to linux qdisc expectation.
 *
 * @dev: [in] pointer to net_device structure
 * @skb: [in] pointer to os packet
 *
 * Return: Qdisc queue index
 */
uint16_t hdd_wmm_select_queue(struct net_device *dev, struct sk_buff *skb)
{
	sme_QosWmmUpType up = SME_QOS_WMM_UP_BE;
	uint16_t queueIndex;
	hdd_adapter_t *pAdapter = WLAN_HDD_GET_PRIV_PTR(dev);
	bool is_eapol = false;
	hdd_context_t *hdd_ctx = WLAN_HDD_GET_CTX(pAdapter);
	int status;

	status = wlan_hdd_validate_context(hdd_ctx);
	if (status != 0) {
		skb->priority = SME_QOS_WMM_UP_BE;
		return HDD_LINUX_AC_BE;
	}

	/* Get the user priority from IP header */
	hdd_wmm_classify_pkt(pAdapter, skb, &up, &is_eapol);
	skb->priority = up;
	queueIndex = hdd_get_queue_index(skb->priority, is_eapol);

	return queueIndex;
}

/**
 * hdd_wmm_acquire_access_required() - Function which will determine
 * acquire admittance for a WMM AC is required or not based on psb configuration
 * done in framework
 *
 * @pAdapter: [in] pointer to adapter structure
 * @acType: [in] WMM AC type of OS packet
 *
 * Return: void
 */
void hdd_wmm_acquire_access_required(hdd_adapter_t *pAdapter,
				     sme_ac_enum_type acType)
{
	/* Each bit in the LSB nibble indicates 1 AC.
	 * Clearing the particular bit in LSB nibble to indicate
	 * access required
	 */
	switch (acType) {
	case SME_AC_BK:
		/* clear first bit */
		pAdapter->psbChanged &= ~SME_QOS_UAPSD_CFG_BK_CHANGED_MASK;
		break;
	case SME_AC_BE:
		/* clear second bit */
		pAdapter->psbChanged &= ~SME_QOS_UAPSD_CFG_BE_CHANGED_MASK;
		break;
	case SME_AC_VI:
		/* clear third bit */
		pAdapter->psbChanged &= ~SME_QOS_UAPSD_CFG_VI_CHANGED_MASK;
		break;
	case SME_AC_VO:
		/* clear fourth bit */
		pAdapter->psbChanged &= ~SME_QOS_UAPSD_CFG_VO_CHANGED_MASK;
		break;
	default:
		CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_ERROR,
			  "%s: Invalid AC Type", __func__);
		break;
	}
}

/**
 * hdd_wmm_acquire_access() - Function which will attempt to acquire
 * admittance for a WMM AC
 *
 * @pAdapter: [in]  pointer to adapter context
 * @acType: [in]  WMM AC type of OS packet
 * @pGranted: [out] pointer to bool flag when indicates if access
 *	      has been granted or not
 *
 * Return: CDF_STATUS enumeration
 */
CDF_STATUS hdd_wmm_acquire_access(hdd_adapter_t *pAdapter,
				  sme_ac_enum_type acType, bool *pGranted)
{
	hdd_wmm_qos_context_t *pQosContext;

	CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO_LOW,
		  "%s: Entered for AC %d", __func__, acType);

	if (!hdd_wmm_is_active(pAdapter) ||
	    !(WLAN_HDD_GET_CTX(pAdapter))->config->bImplicitQosEnabled ||
	    !pAdapter->hddWmmStatus.wmmAcStatus[acType].wmmAcAccessRequired) {
		/* either we don't want QoS or the AP doesn't support
		 * QoS or we don't want to do implicit QoS
		 */
		CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO_LOW,
			  "%s: QoS not configured on both ends ", __func__);

		*pGranted =
			pAdapter->hddWmmStatus.wmmAcStatus[acType].
			wmmAcAccessAllowed;

		return CDF_STATUS_SUCCESS;
	}
	/* do we already have an implicit QoS request pending for this AC? */
	if ((pAdapter->hddWmmStatus.wmmAcStatus[acType].wmmAcAccessNeeded) ||
	    (pAdapter->hddWmmStatus.wmmAcStatus[acType].wmmAcAccessPending)) {
		/* request already pending so we need to wait for that
		 * response
		 */
		CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO_LOW,
			  "%s: Implicit QoS for TL AC %d already scheduled",
			  __func__, acType);

		*pGranted = false;
		return CDF_STATUS_SUCCESS;
	}
	/* did we already fail to establish implicit QoS for this AC?
	 * (if so, access should have been granted when the failure
	 * was handled)
	 */
	if (pAdapter->hddWmmStatus.wmmAcStatus[acType].wmmAcAccessFailed) {
		/* request previously failed
		 * allow access, but we'll be downgraded
		 */
		CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO_LOW,
			  "%s: Implicit QoS for TL AC %d previously failed",
			  __func__, acType);

		if (!pAdapter->hddWmmStatus.wmmAcStatus[acType].
		    wmmAcAccessRequired) {
			pAdapter->hddWmmStatus.wmmAcStatus[acType].
			wmmAcAccessAllowed = true;
			*pGranted = true;
		} else {
			pAdapter->hddWmmStatus.wmmAcStatus[acType].
			wmmAcAccessAllowed = false;
			*pGranted = false;
		}

		return CDF_STATUS_SUCCESS;
	}
	/* we need to establish implicit QoS */
	CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO,
		  "%s: Need to schedule implicit QoS for TL AC %d, pAdapter is %p",
		  __func__, acType, pAdapter);

	pAdapter->hddWmmStatus.wmmAcStatus[acType].wmmAcAccessNeeded = true;

	pQosContext = kmalloc(sizeof(*pQosContext), GFP_ATOMIC);
	if (NULL == pQosContext) {
		/* no memory for QoS context.  Nothing we can do but
		 * let data flow
		 */
		CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_ERROR,
			  "%s: Unable to allocate context", __func__);
		pAdapter->hddWmmStatus.wmmAcStatus[acType].wmmAcAccessAllowed =
			true;
		*pGranted = true;
		return CDF_STATUS_SUCCESS;
	}

	pQosContext->acType = acType;
	pQosContext->pAdapter = pAdapter;
	pQosContext->qosFlowId = 0;
	pQosContext->handle = HDD_WMM_HANDLE_IMPLICIT;
	pQosContext->magic = HDD_WMM_CTX_MAGIC;
	pQosContext->is_inactivity_timer_running = false;

#ifdef CONFIG_CNSS
	cnss_init_work(&pQosContext->wmmAcSetupImplicitQos,
		       hdd_wmm_do_implicit_qos);
#else
	INIT_WORK(&pQosContext->wmmAcSetupImplicitQos, hdd_wmm_do_implicit_qos);
#endif

	CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO,
		  "%s: Scheduling work for AC %d, context %p",
		  __func__, acType, pQosContext);

	schedule_work(&pQosContext->wmmAcSetupImplicitQos);

	/* caller will need to wait until the work takes place and
	 * TSPEC negotiation completes
	 */
	*pGranted = false;
	return CDF_STATUS_SUCCESS;
}

/**
 * hdd_wmm_assoc() - Function which will handle the housekeeping
 * required by WMM when association takes place
 *
 * @pAdapter: [in]  pointer to adapter context
 * @pRoamInfo: [in]  pointer to roam information
 * @eBssType: [in]  type of BSS
 *
 * Return: CDF_STATUS enumeration
 */
CDF_STATUS hdd_wmm_assoc(hdd_adapter_t *pAdapter,
			 tCsrRoamInfo *pRoamInfo, eCsrRoamBssType eBssType)
{
	uint8_t uapsdMask;
	CDF_STATUS status;
	hdd_context_t *pHddCtx = WLAN_HDD_GET_CTX(pAdapter);

	/* when we associate we need to notify TL if it needs to
	 * enable UAPSD for any access categories
	 */

	CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO_LOW,
		  "%s: Entered", __func__);

	if (pRoamInfo->fReassocReq) {
		/* when we reassociate we should continue to use
		 * whatever parameters were previously established.
		 * if we are reassociating due to a U-APSD change for
		 * a particular Access Category, then the change will
		 * be communicated to HDD via the QoS callback
		 * associated with the given flow, and U-APSD
		 * parameters will be updated there
		 */

		CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO_LOW,
			  "%s: Reassoc so no work, Exiting", __func__);

		return CDF_STATUS_SUCCESS;
	}
	/* get the negotiated UAPSD Mask */
	uapsdMask =
		pRoamInfo->u.pConnectedProfile->modifyProfileFields.uapsd_mask;

	CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO_LOW,
		  "%s: U-APSD mask is 0x%02x", __func__, (int)uapsdMask);

	if (uapsdMask & HDD_AC_VO) {
		status =
			sme_enable_uapsd_for_ac((WLAN_HDD_GET_CTX(pAdapter))->
						   pcds_context,
						   (WLAN_HDD_GET_STATION_CTX_PTR
							    (pAdapter))->conn_info.staId[0],
						   SME_AC_VO, 7, 7,
						   pHddCtx->config->InfraUapsdVoSrvIntv,
						   pHddCtx->config->InfraUapsdVoSuspIntv,
						   SME_BI_DIR, 1,
						   pAdapter->sessionId,
						   pHddCtx->config->DelayedTriggerFrmInt);

		CDF_ASSERT(CDF_IS_STATUS_SUCCESS(status));
	}

	if (uapsdMask & HDD_AC_VI) {
		status =
			sme_enable_uapsd_for_ac((WLAN_HDD_GET_CTX(pAdapter))->
						   pcds_context,
						   (WLAN_HDD_GET_STATION_CTX_PTR
							    (pAdapter))->conn_info.staId[0],
						   SME_AC_VI, 5, 5,
						   pHddCtx->config->InfraUapsdViSrvIntv,
						   pHddCtx->config->InfraUapsdViSuspIntv,
						   SME_BI_DIR, 1,
						   pAdapter->sessionId,
						   pHddCtx->config->DelayedTriggerFrmInt);

		CDF_ASSERT(CDF_IS_STATUS_SUCCESS(status));
	}

	if (uapsdMask & HDD_AC_BK) {
		status =
			sme_enable_uapsd_for_ac((WLAN_HDD_GET_CTX(pAdapter))->
						   pcds_context,
						   (WLAN_HDD_GET_STATION_CTX_PTR
							    (pAdapter))->conn_info.staId[0],
						   SME_AC_BK, 2, 2,
						   pHddCtx->config->InfraUapsdBkSrvIntv,
						   pHddCtx->config->InfraUapsdBkSuspIntv,
						   SME_BI_DIR, 1,
						   pAdapter->sessionId,
						   pHddCtx->config->DelayedTriggerFrmInt);

		CDF_ASSERT(CDF_IS_STATUS_SUCCESS(status));
	}

	if (uapsdMask & HDD_AC_BE) {
		status =
			sme_enable_uapsd_for_ac((WLAN_HDD_GET_CTX(pAdapter))->
						   pcds_context,
						   (WLAN_HDD_GET_STATION_CTX_PTR
							    (pAdapter))->conn_info.staId[0],
						   SME_AC_BE, 3, 3,
						   pHddCtx->config->InfraUapsdBeSrvIntv,
						   pHddCtx->config->InfraUapsdBeSuspIntv,
						   SME_BI_DIR, 1,
						   pAdapter->sessionId,
						   pHddCtx->config->DelayedTriggerFrmInt);

		CDF_ASSERT(CDF_IS_STATUS_SUCCESS(status));
	}

	status = sme_update_dsc_pto_up_mapping(pHddCtx->hHal,
					       pAdapter->hddWmmDscpToUpMap,
					       pAdapter->sessionId);

	if (!CDF_IS_STATUS_SUCCESS(status)) {
		hdd_wmm_init(pAdapter);
	}

	CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO_LOW,
		  "%s: Exiting", __func__);

	return CDF_STATUS_SUCCESS;
}

static const uint8_t acm_mask_bit[WLAN_MAX_AC] = {
	0x4,                    /* SME_AC_BK */
	0x8,                    /* SME_AC_BE */
	0x2,                    /* SME_AC_VI */
	0x1                     /* SME_AC_VO */
};

/**
 * hdd_wmm_connect() - Function which will handle the housekeeping
 * required by WMM when a connection is established
 *
 * @pAdapter : [in]  pointer to adapter context
 * @pRoamInfo: [in]  pointer to roam information
 * @eBssType : [in]  type of BSS
 *
 * Return: CDF_STATUS enumeration
 */
CDF_STATUS hdd_wmm_connect(hdd_adapter_t *pAdapter,
			   tCsrRoamInfo *pRoamInfo, eCsrRoamBssType eBssType)
{
	int ac;
	bool qap;
	bool qosConnection;
	uint8_t acmMask;

	CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO_LOW,
		  "%s: Entered", __func__);

	if ((eCSR_BSS_TYPE_INFRASTRUCTURE == eBssType) &&
	    pRoamInfo && pRoamInfo->u.pConnectedProfile) {
		qap = pRoamInfo->u.pConnectedProfile->qap;
		qosConnection = pRoamInfo->u.pConnectedProfile->qosConnection;
		acmMask = pRoamInfo->u.pConnectedProfile->acm_mask;
	} else {
		qap = true;
		qosConnection = true;
		acmMask = 0x0;
	}

	CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO_LOW,
		  "%s: qap is %d, qosConnection is %d, acmMask is 0x%x",
		  __func__, qap, qosConnection, acmMask);

	pAdapter->hddWmmStatus.wmmQap = qap;
	pAdapter->hddWmmStatus.wmmQosConnection = qosConnection;

	for (ac = 0; ac < WLAN_MAX_AC; ac++) {
		if (qap && qosConnection && (acmMask & acm_mask_bit[ac])) {
			CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO_LOW,
				  "%s: ac %d on", __func__, ac);

			/* admission is required */
			pAdapter->hddWmmStatus.wmmAcStatus[ac].
			wmmAcAccessRequired = true;
			pAdapter->hddWmmStatus.wmmAcStatus[ac].
			wmmAcAccessAllowed = false;
			pAdapter->hddWmmStatus.wmmAcStatus[ac].
			wmmAcAccessGranted = false;
			/* after reassoc if we have valid tspec, allow access */
			if (pAdapter->hddWmmStatus.wmmAcStatus[ac].
			    wmmAcTspecValid
			    && (pAdapter->hddWmmStatus.wmmAcStatus[ac].
				wmmAcTspecInfo.ts_info.direction !=
				SME_QOS_WMM_TS_DIR_DOWNLINK)) {
				pAdapter->hddWmmStatus.wmmAcStatus[ac].
				wmmAcAccessAllowed = true;
			}
		} else {
			CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO_LOW,
				  "%s: ac %d off", __func__, ac);
			/* admission is not required so access is allowed */
			pAdapter->hddWmmStatus.wmmAcStatus[ac].
			wmmAcAccessRequired = false;
			pAdapter->hddWmmStatus.wmmAcStatus[ac].
			wmmAcAccessAllowed = true;
		}

	}

	CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO_LOW,
		  "%s: Exiting", __func__);

	return CDF_STATUS_SUCCESS;
}

/**
 * hdd_wmm_get_uapsd_mask() - Function which will calculate the
 * initial value of the UAPSD mask based upon the device configuration
 *
 * @pAdapter  : [in]  pointer to adapter context
 * @pUapsdMask: [out] pointer to where the UAPSD Mask is to be stored
 *
 * Return: CDF_STATUS enumeration
 */
CDF_STATUS hdd_wmm_get_uapsd_mask(hdd_adapter_t *pAdapter,
				  uint8_t *pUapsdMask)
{
	uint8_t uapsdMask;

	if (HDD_WMM_USER_MODE_NO_QOS ==
	    (WLAN_HDD_GET_CTX(pAdapter))->config->WmmMode) {
		/* no QOS then no UAPSD */
		uapsdMask = 0;
	} else {
		/* start with the default mask */
		uapsdMask = (WLAN_HDD_GET_CTX(pAdapter))->config->UapsdMask;

		/* disable UAPSD for any ACs with a 0 Service Interval */
		if ((WLAN_HDD_GET_CTX(pAdapter))->config->
		    InfraUapsdVoSrvIntv == 0) {
			uapsdMask &= ~HDD_AC_VO;
		}

		if ((WLAN_HDD_GET_CTX(pAdapter))->config->
		    InfraUapsdViSrvIntv == 0) {
			uapsdMask &= ~HDD_AC_VI;
		}

		if ((WLAN_HDD_GET_CTX(pAdapter))->config->
		    InfraUapsdBkSrvIntv == 0) {
			uapsdMask &= ~HDD_AC_BK;
		}

		if ((WLAN_HDD_GET_CTX(pAdapter))->config->
		    InfraUapsdBeSrvIntv == 0) {
			uapsdMask &= ~HDD_AC_BE;
		}
	}

	/* return calculated mask */
	*pUapsdMask = uapsdMask;
	return CDF_STATUS_SUCCESS;
}

/**
 * hdd_wmm_is_active() - Function which will determine if WMM is
 * active on the current connection
 *
 * @pAdapter: [in]  pointer to adapter context
 *
 * Return: true if WMM is enabled, false if WMM is not enabled
 */
bool hdd_wmm_is_active(hdd_adapter_t *pAdapter)
{
	if ((!pAdapter->hddWmmStatus.wmmQosConnection) ||
	    (!pAdapter->hddWmmStatus.wmmQap)) {
		return false;
	} else {
		return true;
	}
}

/**
 * hdd_wmm_addts() - Function which will add a traffic spec at the
 * request of an application
 *
 * @pAdapter  : [in]  pointer to adapter context
 * @handle    : [in]  handle to uniquely identify a TS
 * @pTspec    : [in]  pointer to the traffic spec
 *
 * Return: HDD_WLAN_WMM_STATUS_*
 */
hdd_wlan_wmm_status_e hdd_wmm_addts(hdd_adapter_t *pAdapter,
				    uint32_t handle,
				    sme_QosWmmTspecInfo *pTspec)
{
	hdd_wmm_qos_context_t *pQosContext;
	hdd_wlan_wmm_status_e status = HDD_WLAN_WMM_STATUS_SETUP_SUCCESS;
#ifndef WLAN_MDM_CODE_REDUCTION_OPT
	sme_QosStatusType smeStatus;
#endif
	bool found = false;

	CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO_LOW,
		  "%s: Entered with handle 0x%x", __func__, handle);

	/* see if a context already exists with the given handle */
	mutex_lock(&pAdapter->hddWmmStatus.wmmLock);
	list_for_each_entry(pQosContext,
			    &pAdapter->hddWmmStatus.wmmContextList, node) {
		if (pQosContext->handle == handle) {
			found = true;
			break;
		}
	}
	mutex_unlock(&pAdapter->hddWmmStatus.wmmLock);
	if (found) {
		/* record with that handle already exists */
		CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_ERROR,
			  "%s: Record already exists with handle 0x%x",
			  __func__, handle);

		/* Application is trying to modify some of the Tspec
		 * params. Allow it
		 */
		smeStatus = sme_qos_modify_req(WLAN_HDD_GET_HAL_CTX(pAdapter),
					       pTspec, pQosContext->qosFlowId);

		/* need to check the return value and act appropriately */
		switch (smeStatus) {
		case SME_QOS_STATUS_MODIFY_SETUP_PENDING_RSP:
			status = HDD_WLAN_WMM_STATUS_MODIFY_PENDING;
			break;
		case SME_QOS_STATUS_MODIFY_SETUP_SUCCESS_NO_ACM_NO_APSD_RSP:
			status =
				HDD_WLAN_WMM_STATUS_MODIFY_SUCCESS_NO_ACM_NO_UAPSD;
			break;
		case SME_QOS_STATUS_MODIFY_SETUP_SUCCESS_APSD_SET_ALREADY:
			status =
				HDD_WLAN_WMM_STATUS_MODIFY_SUCCESS_NO_ACM_UAPSD_EXISTING;
			break;
		case SME_QOS_STATUS_MODIFY_SETUP_INVALID_PARAMS_RSP:
			status = HDD_WLAN_WMM_STATUS_MODIFY_FAILED_BAD_PARAM;
			break;
		case SME_QOS_STATUS_MODIFY_SETUP_FAILURE_RSP:
			status = HDD_WLAN_WMM_STATUS_MODIFY_FAILED;
			break;
		case SME_QOS_STATUS_SETUP_NOT_QOS_AP_RSP:
			status = HDD_WLAN_WMM_STATUS_SETUP_FAILED_NO_WMM;
			break;
		default:
			/* we didn't get back one of the
			 * SME_QOS_STATUS_MODIFY_* status codes
			 */
			CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_ERROR,
				  "%s: unexpected SME Status=%d", __func__,
				  smeStatus);
			CDF_ASSERT(0);
			return HDD_WLAN_WMM_STATUS_MODIFY_FAILED;
		}

		/* we were successful, save the status */
		mutex_lock(&pAdapter->hddWmmStatus.wmmLock);
		if (pQosContext->magic == HDD_WMM_CTX_MAGIC)
			pQosContext->lastStatus = status;
		mutex_unlock(&pAdapter->hddWmmStatus.wmmLock);

		return status;
	}

	pQosContext = kmalloc(sizeof(*pQosContext), GFP_KERNEL);
	if (NULL == pQosContext) {
		/* no memory for QoS context.  Nothing we can do */
		CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_ERROR,
			  "%s: Unable to allocate QoS context", __func__);
		return HDD_WLAN_WMM_STATUS_INTERNAL_FAILURE;
	}
	/* we assume the tspec has already been validated by the caller */

	pQosContext->handle = handle;
	if (pTspec->ts_info.up < HDD_WMM_UP_TO_AC_MAP_SIZE)
		pQosContext->acType = hdd_wmm_up_to_ac_map[pTspec->ts_info.up];
	else {
		CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_ERROR,
			  "%s: ts_info.up (%d) larger than max value (%d), use default acType (%d)",
			  __func__, pTspec->ts_info.up,
			  HDD_WMM_UP_TO_AC_MAP_SIZE - 1, hdd_wmm_up_to_ac_map[0]);
		pQosContext->acType = hdd_wmm_up_to_ac_map[0];
	}
	pQosContext->pAdapter = pAdapter;
	pQosContext->qosFlowId = 0;
	pQosContext->magic = HDD_WMM_CTX_MAGIC;

	CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO,
		  "%s: Setting up QoS, context %p", __func__, pQosContext);

	mutex_lock(&pAdapter->hddWmmStatus.wmmLock);
	list_add(&pQosContext->node, &pAdapter->hddWmmStatus.wmmContextList);
	mutex_unlock(&pAdapter->hddWmmStatus.wmmLock);

#ifndef WLAN_MDM_CODE_REDUCTION_OPT
	smeStatus = sme_qos_setup_req(WLAN_HDD_GET_HAL_CTX(pAdapter),
				      pAdapter->sessionId,
				      pTspec,
				      hdd_wmm_sme_callback,
				      pQosContext,
				      pTspec->ts_info.up,
				      &pQosContext->qosFlowId);

	CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO,
		  "%s: sme_qos_setup_req returned %d flowid %d",
		  __func__, smeStatus, pQosContext->qosFlowId);

	/* need to check the return value and act appropriately */
	switch (smeStatus) {
	case SME_QOS_STATUS_SETUP_REQ_PENDING_RSP:
		status = HDD_WLAN_WMM_STATUS_SETUP_PENDING;
		break;
	case SME_QOS_STATUS_SETUP_SUCCESS_NO_ACM_NO_APSD_RSP:
		status = HDD_WLAN_WMM_STATUS_SETUP_SUCCESS_NO_ACM_NO_UAPSD;
		break;
	case SME_QOS_STATUS_SETUP_SUCCESS_APSD_SET_ALREADY:
		status =
			HDD_WLAN_WMM_STATUS_SETUP_SUCCESS_NO_ACM_UAPSD_EXISTING;
		break;
	case SME_QOS_STATUS_SETUP_SUCCESS_IND_APSD_PENDING:
		status = HDD_WLAN_WMM_STATUS_SETUP_PENDING;
		break;
	case SME_QOS_STATUS_SETUP_INVALID_PARAMS_RSP:
		hdd_wmm_free_context(pQosContext);
		return HDD_WLAN_WMM_STATUS_SETUP_FAILED_BAD_PARAM;
	case SME_QOS_STATUS_SETUP_FAILURE_RSP:
		/* we can't tell the difference between when a request
		 * fails because AP rejected it versus when SME
		 * encounterd an internal error
		 */
		hdd_wmm_free_context(pQosContext);
		return HDD_WLAN_WMM_STATUS_SETUP_FAILED;
	case SME_QOS_STATUS_SETUP_NOT_QOS_AP_RSP:
		hdd_wmm_free_context(pQosContext);
		return HDD_WLAN_WMM_STATUS_SETUP_FAILED_NO_WMM;
	default:
		/* we didn't get back one of the
		 * SME_QOS_STATUS_SETUP_* status codes
		 */
		hdd_wmm_free_context(pQosContext);
		CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_ERROR,
			  "%s: unexpected SME Status=%d", __func__, smeStatus);
		CDF_ASSERT(0);
		return HDD_WLAN_WMM_STATUS_SETUP_FAILED;
	}
#endif

	/* we were successful, save the status */
	mutex_lock(&pAdapter->hddWmmStatus.wmmLock);
	if (pQosContext->magic == HDD_WMM_CTX_MAGIC)
		pQosContext->lastStatus = status;
	mutex_unlock(&pAdapter->hddWmmStatus.wmmLock);

	return status;
}

/**
 * hdd_wmm_delts() - Function which will delete a traffic spec at the
 * request of an application
 *
 * @pAdapter: [in]  pointer to adapter context
 * @handle: [in]  handle to uniquely identify a TS
 *
 * Return: HDD_WLAN_WMM_STATUS_*
 */
hdd_wlan_wmm_status_e hdd_wmm_delts(hdd_adapter_t *pAdapter, uint32_t handle)
{
	hdd_wmm_qos_context_t *pQosContext;
	bool found = false;
	sme_ac_enum_type acType = 0;
	uint32_t qosFlowId = 0;
	hdd_wlan_wmm_status_e status = HDD_WLAN_WMM_STATUS_SETUP_SUCCESS;
#ifndef WLAN_MDM_CODE_REDUCTION_OPT
	sme_QosStatusType smeStatus;
#endif

	CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO_LOW,
		  "%s: Entered with handle 0x%x", __func__, handle);

	/* locate the context with the given handle */
	mutex_lock(&pAdapter->hddWmmStatus.wmmLock);
	list_for_each_entry(pQosContext,
			    &pAdapter->hddWmmStatus.wmmContextList, node) {
		if (pQosContext->handle == handle) {
			found = true;
			acType = pQosContext->acType;
			qosFlowId = pQosContext->qosFlowId;
			break;
		}
	}
	mutex_unlock(&pAdapter->hddWmmStatus.wmmLock);

	if (false == found) {
		/* we didn't find the handle */
		CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO_LOW,
			  "%s: handle 0x%x not found", __func__, handle);
		return HDD_WLAN_WMM_STATUS_RELEASE_FAILED_BAD_PARAM;
	}

	CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO_LOW,
		  "%s: found handle 0x%x, flow %d, AC %d, context %p",
		  __func__, handle, qosFlowId, acType, pQosContext);

#ifndef WLAN_MDM_CODE_REDUCTION_OPT
	smeStatus =
		sme_qos_release_req(WLAN_HDD_GET_HAL_CTX(pAdapter), qosFlowId);

	CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO_LOW,
		  "%s: SME flow %d released, SME status %d",
		  __func__, qosFlowId, smeStatus);

	switch (smeStatus) {
	case SME_QOS_STATUS_RELEASE_SUCCESS_RSP:
		/* this flow is the only one on that AC, so go ahead
		 * and update our TSPEC state for the AC
		 */
		pAdapter->hddWmmStatus.wmmAcStatus[acType].wmmAcTspecValid =
			false;

		/* need to tell TL to stop trigger timer, etc */
		hdd_wmm_disable_tl_uapsd(pQosContext);

#ifdef FEATURE_WLAN_ESE
		/* disable the inactivity timer */
		hdd_wmm_disable_inactivity_timer(pQosContext);
#endif
		/* we are done with this context */
		hdd_wmm_free_context(pQosContext);

		/* SME must not fire any more callbacks for this flow
		 * since the context is no longer valid
		 */

		return HDD_WLAN_WMM_STATUS_RELEASE_SUCCESS;

	case SME_QOS_STATUS_RELEASE_REQ_PENDING_RSP:
		/* do nothing as we will get a response from SME */
		status = HDD_WLAN_WMM_STATUS_RELEASE_PENDING;
		break;

	case SME_QOS_STATUS_RELEASE_INVALID_PARAMS_RSP:
		/* nothing we can do with the existing flow except leave it */
		status = HDD_WLAN_WMM_STATUS_RELEASE_FAILED_BAD_PARAM;
		break;

	case SME_QOS_STATUS_RELEASE_FAILURE_RSP:
		/* nothing we can do with the existing flow except leave it */
		status = HDD_WLAN_WMM_STATUS_RELEASE_FAILED;

	default:
		/* we didn't get back one of the
		 * SME_QOS_STATUS_RELEASE_* status codes
		 */
		CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_ERROR,
			  "%s: unexpected SME Status=%d", __func__, smeStatus);
		CDF_ASSERT(0);
		status = HDD_WLAN_WMM_STATUS_RELEASE_FAILED;
	}

#endif
	mutex_lock(&pAdapter->hddWmmStatus.wmmLock);
	if (pQosContext->magic == HDD_WMM_CTX_MAGIC)
		pQosContext->lastStatus = status;
	mutex_unlock(&pAdapter->hddWmmStatus.wmmLock);

	return status;
}

/**
 * hdd_wmm_checkts() - Function which will return the status of a traffic
 * spec at the request of an application
 *
 * @pAdapter: [in]  pointer to adapter context
 * @handle: [in]  handle to uniquely identify a TS
 *
 * Return: HDD_WLAN_WMM_STATUS_*
 */
hdd_wlan_wmm_status_e hdd_wmm_checkts(hdd_adapter_t *pAdapter, uint32_t handle)
{
	hdd_wmm_qos_context_t *pQosContext;
	hdd_wlan_wmm_status_e status = HDD_WLAN_WMM_STATUS_LOST;

	CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO_LOW,
		  "%s: Entered with handle 0x%x", __func__, handle);

	/* locate the context with the given handle */
	mutex_lock(&pAdapter->hddWmmStatus.wmmLock);
	list_for_each_entry(pQosContext,
			    &pAdapter->hddWmmStatus.wmmContextList, node) {
		if (pQosContext->handle == handle) {
			CDF_TRACE(CDF_MODULE_ID_HDD, WMM_TRACE_LEVEL_INFO_LOW,
				  "%s: found handle 0x%x, context %p",
				  __func__, handle, pQosContext);

			status = pQosContext->lastStatus;
			break;
		}
	}
	mutex_unlock(&pAdapter->hddWmmStatus.wmmLock);
	return status;
}
