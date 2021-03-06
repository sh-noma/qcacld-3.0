/*
 * Copyright (c) 2015 The Linux Foundation. All rights reserved.
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

#include <linux/platform_device.h>
#include <linux/pci.h>
#ifdef HIF_PCI
#ifdef CONFIG_CNSS
#include <net/cnss.h>
#endif /* CONFIG_CNSS */
#else
#include <soc/qcom/icnss.h>
#endif /* HIF_PCI */
#include "cds_api.h"
#include "cdf_status.h"
#include "cdf_lock.h"
#include "cds_sched.h"
#include "osdep.h"
#include "hif.h"
#include "epping_main.h"
#include "wlan_hdd_main.h"
#include "wlan_hdd_power.h"
#include "wlan_logging_sock_svc.h"
#include "wma_api.h"
#include "wlan_hdd_napi.h"
#include "cds_concurrency.h"

#ifdef MODULE
#define WLAN_MODULE_NAME  module_name(THIS_MODULE)
#else
#define WLAN_MODULE_NAME  "wlan"
#endif

#ifdef HIF_PCI
#ifdef CONFIG_CNSS
#define WLAN_HDD_REGISTER_DRIVER(wlan_drv_ops) \
	cnss_wlan_register_driver(wlan_drv_ops)
#define WLAN_HDD_UNREGISTER_DRIVER(wlan_drv_ops) \
	cnss_wlan_unregister_driver(wlan_drv_ops)
#else
#define WLAN_HDD_REGISTER_DRIVER(wlan_drv_ops) \
	pci_register_driver(wlan_drv_ops)
#define WLAN_HDD_UNREGISTER_DRIVER(wlan_drv_ops) \
	pci_unregister_driver(wlan_drv_ops)
#endif /* CONFIG_CNSS */
#else
#define WLAN_HDD_REGISTER_DRIVER(wlan_drv_ops) \
	icnss_register_driver(wlan_drv_ops)
#define WLAN_HDD_UNREGISTER_DRIVER(wlan_drv_ops) \
	icnss_unregister_driver(wlan_drv_ops)
#endif /* HIF_PCI */

/**
 * wlan_hdd_probe() - handles probe request
 *
 * This function is called to probe the wlan driver
 *
 * @dev: wlan device structure
 * @bdev: bus device structure
 * @bid: bus identifier for shared busses
 * @bus_type: underlying bus type
 * @reinit: true if we are reinitiallizing the driver after a subsystem restart
 *
 * Return: 0 on successfull probe
 */
static int wlan_hdd_probe(struct device *dev, void *bdev, const hif_bus_id *bid,
	enum ath_hal_bus_type bus_type, bool reinit)
{
	void *hif_ctx;
	CDF_STATUS status;
	int ret;

	if (WLAN_IS_EPPING_ENABLED(cds_get_conparam())) {
		status = epping_open();
		if (status != CDF_STATUS_SUCCESS)
			return status;
	}

	status = hif_open();
	if (status != CDF_STATUS_SUCCESS) {
		hddLog(LOGE, FL("hif_open error = %d"), status);
		return -EFAULT;
	}
	hif_ctx = cds_get_context(CDF_MODULE_ID_HIF);
	if (reinit)
		hdd_napi_destroy(true);
	status = hdd_napi_create();
	if (hdd_napi_enabled(HDD_NAPI_ANY)) {
		hdd_info("hdd_napi_create returned: %d\n", status);
		if (status <= 0) {
			hdd_err("NAPI creation error, rc: 0x%x, reinit = %d",
			status, reinit);
			ret = -EFAULT;
			goto end;
		}
	}

	status = hif_enable(hif_ctx, dev, bdev, bid,
				bus_type, (reinit == true) ?
				HIF_ENABLE_TYPE_REINIT : HIF_ENABLE_TYPE_PROBE);
	if (status != CDF_STATUS_SUCCESS) {
		hdd_err("hif_enable error = %d, reinit = %d",
			status, reinit);
		ret = -EIO;
		goto end;
	}

	if (reinit)
		ret = hdd_wlan_re_init(hif_ctx);
	else
		ret = hdd_wlan_startup(dev, hif_ctx);

end:
	if (ret) {
		if (WLAN_IS_EPPING_ENABLED(cds_get_conparam())) {
			hif_pktlogmod_exit(hif_ctx);
			epping_disable();
			htc_destroy(cds_get_context(CDF_MODULE_ID_HTC));
			hif_disable(hif_ctx, (reinit == true) ?
				HIF_ENABLE_TYPE_REINIT : HIF_ENABLE_TYPE_PROBE);
			cds_free_context(NULL, CDF_MODULE_ID_HTC, NULL);
			epping_close();
		} else {
			hif_pktlogmod_exit(hif_ctx);
			__hdd_wlan_exit();
			hif_disable(hif_ctx, HIF_DISABLE_TYPE_REMOVE);
		}
		hif_close(hif_ctx);
	}

	if (reinit)
		cds_set_logp_in_progress(false);

	return ret;
}

/**
 * wlan_hdd_remove() - wlan_hdd_remove
 *
 * This function is called by the platform driver to remove the
 * driver
 *
 * Return: void
 */
static void wlan_hdd_remove(void)
{
	void *hif_ctx;
	v_CONTEXT_t p_cds_context = NULL;

	/* Get the global cds context */
	p_cds_context = cds_get_global_context();

	hif_ctx = cds_get_context(CDF_MODULE_ID_HIF);

	if (WLAN_IS_EPPING_ENABLED(cds_get_conparam())) {
		hif_pktlogmod_exit(hif_ctx);
		epping_disable();
		htc_destroy(cds_get_context(CDF_MODULE_ID_HTC));
		hif_disable(hif_ctx, HIF_DISABLE_TYPE_REMOVE);
		cds_free_context(NULL, CDF_MODULE_ID_HTC, NULL);
		epping_close();
	} else {
		hif_pktlogmod_exit(hif_ctx);
		__hdd_wlan_exit();
		hif_disable(hif_ctx, HIF_DISABLE_TYPE_REMOVE);
	}

	hdd_napi_destroy(true);
	hif_close(hif_ctx);

	cds_free_global_context(&p_cds_context);

#ifdef WLAN_LOGGING_SOCK_SVC_ENABLE
	wlan_logging_sock_deinit_svc();
#endif

	cdf_wake_lock_destroy(hdd_wlan_get_wake_lock_ptr());

	pr_info("%s: driver unloaded\n", WLAN_MODULE_NAME);
}

/**
 * wlan_hdd_shutdown() - wlan_hdd_shutdown
 *
 * This is routine is called by platform driver to shutdown the
 * driver
 *
 * Return: void
 */
static void wlan_hdd_shutdown(void)
{
	void *hif_ctx = cds_get_context(CDF_MODULE_ID_HIF);

	if (cds_is_load_unload_in_progress()) {
		hddLog(LOGE,
			FL("Load/unload in progress, ignore SSR shutdown"));
		return;
	}
	/* this is for cases, where shutdown invoked from CNSS */
	cds_set_logp_in_progress(true);

	if (cds_get_conparam() != CDF_FTM_MODE &&
	    !WLAN_IS_EPPING_ENABLED(cds_get_conparam()))
		hif_pktlogmod_exit(hif_ctx);

	if (!cds_is_ssr_ready(__func__))
		hddLog(LOGE,
			FL("Host is not ready for SSR, attempting anyway"));

	if (!WLAN_IS_EPPING_ENABLED(cds_get_conparam())) {
		hif_disable_isr(hif_ctx);
		hdd_wlan_shutdown();
	}

	hif_disable(hif_ctx, HIF_DISABLE_TYPE_SHUTDOWN);
	hif_close(hif_ctx);
}

/**
 * wlan_hdd_crash_shutdown() - wlan_hdd_crash_shutdown
 *
 * HDD crash shutdown funtion: This function is called by
 * platfrom driver's crash shutdown routine
 *
 * Return: void
 */
void wlan_hdd_crash_shutdown(void)
{
	hif_crash_shutdown(cds_get_context(CDF_MODULE_ID_HIF));
}

/**
 * wlan_hdd_notify_handler() - wlan_hdd_notify_handler
 *
 * This function is called by the platform driver to notify the
 * COEX
 *
 * @state: state
 *
 * Return: void
 */
void wlan_hdd_notify_handler(int state)
{
	if (!WLAN_IS_EPPING_ENABLED(cds_get_conparam())) {
		int ret = 0;
		ret = hdd_wlan_notify_modem_power_state(state);
		if (ret < 0)
			hddLog(LOGE, FL("Fail to send notify"));
	}
}

/**
 * __wlan_hdd_bus_suspend() - handles platform supsend
 * @state: suspend message from the kernel
 *
 * Does precondtion validation. Ensures that a subsystem restart isn't in
 * progress.  Ensures that no load or unload is in progress.
 * Calls ol_txrx_bus_suspend to ensure the layer is ready for a bus suspend.
 * Calls wma_suspend to configure offloads.
 * Calls hif_suspend to suspend the bus.
 *
 * Return: 0 for success, -EFAULT for null pointers,
 *     -EBUSY or -EAGAIN if another opperation is in progress and
 *     wlan will not be ready to suspend in time.
 */
static int __wlan_hdd_bus_suspend(pm_message_t state)
{
	void *hdd_ctx = cds_get_context(CDF_MODULE_ID_HDD);
	int err = wlan_hdd_validate_context(hdd_ctx);
	int status;

	hdd_info("event %d", state.event);

	if (err)
		goto done;

	err = cdf_status_to_os_return(
			ol_txrx_bus_suspend());
	if (err)
		goto done;

	err = wma_bus_suspend();
	if (err)
		goto resume_oltxrx;

	err = hif_bus_suspend();
	if (err)
		goto resume_wma;

	hdd_info("suspend done, status = %d", err);
	return err;

resume_wma:
	status = wma_bus_resume();
	CDF_BUG(!status);
resume_oltxrx:
	status = ol_txrx_bus_resume();
	CDF_BUG(!status);
done:
	hdd_err("suspend done, status = %d", err);
	return err;
}

/**
 * wlan_hdd_bus_suspend() - suspend the wlan bus
 *
 * This function is called by the platform driver to suspend the
 * wlan bus
 *
 * @state: state
 *
 * Return: CDF_STATUS
 */
int wlan_hdd_bus_suspend(pm_message_t state)
{
	int ret;

	cds_ssr_protect(__func__);
	ret = __wlan_hdd_bus_suspend(state);
	cds_ssr_unprotect(__func__);

	return ret;
}

/**
 * __wlan_hdd_bus_resume() - handles platform resume
 *
 * Does precondtion validation. Ensures that a subsystem restart isn't in
 * progress.  Ensures that no load or unload is in progress.  Ensures that
 * it has valid pointers for the required contexts.
 * Calls into hif to resume the bus opperation.
 * Calls into wma to handshake with firmware and notify it that the bus is up.
 * Calls into ol_txrx for symetry.
 * Failures are treated as catastrophic.
 *
 * return: error code or 0 for success
 */
static int __wlan_hdd_bus_resume(void)
{
	void *hdd_ctx = cds_get_context(CDF_MODULE_ID_HDD);
	int status = wlan_hdd_validate_context(hdd_ctx);

	if (0 != status) {
		hddLog(LOGE, FL("HDD context is not valid"));
		return status;
	}

	status = hif_bus_resume();
	CDF_BUG(!status);

	status = wma_bus_resume();
	CDF_BUG(!status);

	status = ol_txrx_bus_resume();
	CDF_BUG(!status);

	hdd_info("resume done");
	return status;
}

/**
 * wlan_hdd_bus_resume(): wake up the bus
 *
 * This function is called by the platform driver to resume wlan
 * bus
 *
 * Return: void
 */
static int wlan_hdd_bus_resume(void)
{
	int ret;

	cds_ssr_protect(__func__);
	ret = __wlan_hdd_bus_resume();
	cds_ssr_unprotect(__func__);

	return ret;
}

#ifdef HIF_PCI
/**
 * wlan_hdd_pci_probe() - probe callback for pci platform driver
 * @pdev: bus dev
 *
 * Return: void
 */
static int wlan_hdd_pci_probe(struct pci_dev *pdev,
	const struct pci_device_id *id)
{
	return wlan_hdd_probe(&pdev->dev, pdev, (void *)id,
			HAL_BUS_TYPE_PCI, false);
}

/**
 * wlan_hdd_pci_remove() - wlan_hdd_pci_remove
 *
 * Return: void
 */
void wlan_hdd_pci_remove(struct pci_dev *pdev)
{
	wlan_hdd_remove();
}

/**
 * wlan_hdd_pci_reinit() - wlan_hdd_pci_reinit
 * @pdev: bus dev
 * @id: bus id
 *
 * Return: int
 */
int wlan_hdd_pci_reinit(struct pci_dev *pdev,
	const struct pci_device_id *id)
{
	return wlan_hdd_probe(&pdev->dev, pdev, id,
			HAL_BUS_TYPE_PCI, true);
}

/**
 * wlan_hdd_pci_shutdown() - wlan_hdd_pci_shutdown
 * @pdev: pdev
 *
 * Return: void
 */
void wlan_hdd_pci_shutdown(struct pci_dev *pdev)
{
	wlan_hdd_shutdown();
}

/**
 * wlan_hdd_pci_crash_shutdown() - wlan_hdd_pci_crash_shutdown
 * @pdev: pdev
 *
 * Return: void
 */
void wlan_hdd_pci_crash_shutdown(struct pci_dev *pdev)
{
	wlan_hdd_crash_shutdown();
}

/**
 * wlan_hdd_pci_notify_handler() - wlan_hdd_pci_notify_handler
 * @pdev: pdev
 * @state: state
 *
 * Return: void
 */
void wlan_hdd_pci_notify_handler(struct pci_dev *pdev, int state)
{
	wlan_hdd_notify_handler(state);
}

/**
 * wlan_hdd_pci_suspend() - wlan_hdd_pci_suspend
 * @pdev: pdev
 * @state: state
 *
 * Return: void
 */
static int wlan_hdd_pci_suspend(struct pci_dev *pdev, pm_message_t state)
{
	return wlan_hdd_bus_suspend(state);
}

/**
 * wlan_hdd_pci_resume() - wlan_hdd_pci_resume
 * @pdev: pdev
 *
 * Return: void
 */
static int wlan_hdd_pci_resume(struct pci_dev *pdev)
{
	return wlan_hdd_bus_resume();
}
#else
/**
 * wlan_hdd_snoc_probe() - wlan_hdd_snoc_probe
 * @dev: dev
 *
 * Return: int
 */
static int wlan_hdd_snoc_probe(struct device *dev)
{
	return wlan_hdd_probe(dev, NULL, NULL, HAL_BUS_TYPE_SNOC, false);
}

/**
 * wlan_hdd_snoc_remove() - wlan_hdd_snoc_remove
 * @dev: dev
 *
 * Return: void
 */
void wlan_hdd_snoc_remove(struct device *dev)
{
	wlan_hdd_remove();
}

/**
 * wlan_hdd_snoc_shutdown() - wlan_hdd_snoc_shutdown
 * @dev: dev
 *
 * Return: void
 */
void wlan_hdd_snoc_shutdown(struct device *dev)
{
	wlan_hdd_shutdown();
}

/**
 * wlan_hdd_snoc_reinit() - wlan_hdd_snoc_reinit
 * @dev: dev
 *
 * Return: int
 */
int wlan_hdd_snoc_reinit(struct device *dev)
{
	return wlan_hdd_probe(dev, NULL, NULL, HAL_BUS_TYPE_SNOC, true);
}

/**
 * wlan_hdd_snoc_crash_shutdown() - wlan_hdd_snoc_crash_shutdown
 * @dev: dev
 *
 * Return: void
 */
void wlan_hdd_snoc_crash_shutdown(void *pdev)
{
	wlan_hdd_crash_shutdown();
}

/**
 * wlan_hdd_snoc_suspend() - wlan_hdd_snoc_suspend
 * @dev: dev
 * @state: state
 *
 * Return: int
 */
static int wlan_hdd_snoc_suspend(struct device *dev, pm_message_t state)
{
	return wlan_hdd_bus_suspend(state);
}

/**
 * wlan_hdd_snoc_resume() - wlan_hdd_snoc_resume
 * @dev: dev
 *
 * Return: int
 */
static int wlan_hdd_snoc_resume(struct device *dev)
{
	return wlan_hdd_bus_resume();
}
#endif /* HIF_PCI */

#ifdef HIF_PCI
static struct pci_device_id wlan_hdd_pci_id_table[] = {
	{ 0x168c, 0x003c, PCI_ANY_ID, PCI_ANY_ID },
	{ 0x168c, 0x003e, PCI_ANY_ID, PCI_ANY_ID },
	{ 0x168c, 0x0041, PCI_ANY_ID, PCI_ANY_ID },
	{ 0x168c, 0xabcd, PCI_ANY_ID, PCI_ANY_ID },
	{ 0x168c, 0x7021, PCI_ANY_ID, PCI_ANY_ID },
	{ 0 }
};

#ifdef CONFIG_CNSS
struct cnss_wlan_driver wlan_drv_ops = {
	.name       = "wlan_hdd_pci",
	.id_table   = wlan_hdd_pci_id_table,
	.probe      = wlan_hdd_pci_probe,
	.remove     = wlan_hdd_pci_remove,
	.reinit     = wlan_hdd_pci_reinit,
	.shutdown   = wlan_hdd_pci_shutdown,
	.crash_shutdown = wlan_hdd_pci_crash_shutdown,
	.modem_status   = wlan_hdd_pci_notify_handler,
#ifdef ATH_BUS_PM
	.suspend    = wlan_hdd_pci_suspend,
	.resume     = wlan_hdd_pci_resume,
#endif /* ATH_BUS_PM */
};
#else
MODULE_DEVICE_TABLE(pci, wlan_hdd_pci_id_table);
struct pci_driver wlan_drv_ops = {
	.name       = "wlan_hdd_pci",
	.id_table   = wlan_hdd_pci_id_table,
	.probe      = wlan_hdd_pci_probe,
	.remove     = wlan_hdd_pci_remove,
#ifdef ATH_BUS_PM
	.suspend    = wlan_hdd_pci_suspend,
	.resume     = wlan_hdd_pci_resume,
#endif /* ATH_BUS_PM */
};
#endif /* CONFIG_CNSS */
#else
struct icnss_driver_ops wlan_drv_ops = {
	.name       = "wlan_hdd_drv",
	.probe      = wlan_hdd_snoc_probe,
	.remove     = wlan_hdd_snoc_remove,
	.shutdown   = wlan_hdd_snoc_shutdown,
	.reinit     = wlan_hdd_snoc_reinit,
	.crash_shutdown = wlan_hdd_snoc_crash_shutdown,
	.suspend    = wlan_hdd_snoc_suspend,
	.resume     = wlan_hdd_snoc_resume,
};
#endif

/**
 * wlan_hdd_register_driver() - wlan_hdd_register_driver
 *
 * Return: int
 */
int wlan_hdd_register_driver(void)
{
	return WLAN_HDD_REGISTER_DRIVER(&wlan_drv_ops);
}

/**
 * wlan_hdd_unregister_driver() - wlan_hdd_unregister_driver
 *
 * Return: void
 */
void wlan_hdd_unregister_driver(void)
{
	WLAN_HDD_UNREGISTER_DRIVER(&wlan_drv_ops);
}
