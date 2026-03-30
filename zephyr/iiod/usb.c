/*
 * Copyright (c) 2025 Analog Devices, Inc.
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <tinyiiod/tinyiiod.h>
#include <zephyr/device.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/bos.h>

LOG_MODULE_REGISTER(iiod_usb, CONFIG_LIBIIO_LOG_LEVEL);

/*
 * Instantiate a context named sample_usbd using the default USB device
 * controller, the Zephyr project vendor ID, and the sample product ID.
 * Zephyr project vendor ID must not be used outside of Zephyr samples.
 */
USBD_DEVICE_DEFINE(sample_usbd,
		   DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
		   CONFIG_IIOD_USBD_VID, CONFIG_IIOD_USBD_PID);
/* doc device instantiation end */

/* doc string instantiation start */
USBD_DESC_LANG_DEFINE(sample_lang);
USBD_DESC_MANUFACTURER_DEFINE(sample_mfr, CONFIG_IIOD_USBD_MANUFACTURER);
USBD_DESC_PRODUCT_DEFINE(sample_product, CONFIG_IIOD_USBD_PRODUCT);
IF_ENABLED(CONFIG_HWINFO, (USBD_DESC_SERIAL_NUMBER_DEFINE(sample_sn)));

/* doc string instantiation end */

USBD_DESC_CONFIG_DEFINE(fs_cfg_desc, "FS Configuration");
USBD_DESC_CONFIG_DEFINE(hs_cfg_desc, "HS Configuration");

/* doc configuration instantiation start */
static const uint8_t attributes = (IS_ENABLED(CONFIG_IIOD_USBD_SELF_POWERED) ?
				   USB_SCD_SELF_POWERED : 0) |
				  (IS_ENABLED(CONFIG_IIOD_USBD_REMOTE_WAKEUP) ?
				   USB_SCD_REMOTE_WAKEUP : 0);

/* Full speed configuration */
USBD_CONFIGURATION_DEFINE(sample_fs_config,
			  attributes,
			  CONFIG_IIOD_USBD_MAX_POWER, &fs_cfg_desc);

/* High speed configuration */
USBD_CONFIGURATION_DEFINE(sample_hs_config,
			  attributes,
			  CONFIG_IIOD_USBD_MAX_POWER, &hs_cfg_desc);

/* Pointer to USB device context - for future IIO device integration */
static struct usbd_context *const usbd_ctx = &sample_usbd;

static ssize_t iiod_usb_read(struct iiod_pdata *pdata, void *buf, size_t size)
{
    return 0;
}

static ssize_t iiod_usb_write(struct iiod_pdata *pdata, const void *buf, size_t size)
{
    return 0;
}

static void msg_cb(struct usbd_context *const usbd_ctx,
		   const struct usbd_msg *const msg)
{
	LOG_INF("USBD message: %s", usbd_msg_type_string(msg->type));

	if (msg->type == USBD_MSG_CONFIGURATION) {
		LOG_INF("\tConfiguration value %d", msg->status);
	}

	if (usbd_can_detect_vbus(usbd_ctx)) {
		if (msg->type == USBD_MSG_VBUS_READY) {
			if (usbd_enable(usbd_ctx)) {
				LOG_ERR("Failed to enable device support");
			}
		}

		if (msg->type == USBD_MSG_VBUS_REMOVED) {
			if (usbd_disable(usbd_ctx)) {
				LOG_ERR("Failed to disable device support");
			}
		}
	}
}

static void iiod_usb_thread(void *p1, void *p2, void *p3)
{
	struct usbd_context *usbd_thread = (struct usbd_context *)p1;
	int err;

	LOG_INF("Entered USB thread");

    err = usbd_add_descriptor(usbd_thread, &sample_lang);
    if (err) {
        LOG_ERR("Failed to initialize language descriptor (%d)", err);
        return;
    }
    LOG_INF("Added language descriptor: 0x%04x", *(const uint16_t *)sample_lang.ptr);

    err = usbd_add_descriptor(usbd_thread, &sample_mfr);
    if (err) {
        LOG_ERR("Failed to initialize manufacturer descriptor (%d)", err);
        return;
    }
    LOG_INF("Added manufacturer descriptor: %s", (const char *)sample_mfr.ptr);

    err = usbd_add_descriptor(usbd_thread, &sample_product);
    if (err) {
        LOG_ERR("Failed to initialize product descriptor (%d)", err);
        return;
    }
    LOG_INF("Added product descriptor: %s", (const char *)sample_product.ptr);

    IF_ENABLED(CONFIG_HWINFO, (
        err = usbd_add_descriptor(usbd_thread, &sample_sn);
        if (err) {
            LOG_ERR("Failed to initialize SN descriptor (%d)", err);
            return;
        }
        if (sample_sn.ptr != NULL) {
            LOG_INF("Added serial number descriptor: %s", (const char *)sample_sn.ptr);
        } else {
            LOG_INF("Added serial number descriptor (from hwinfo)");
        }
    ))
    
	err = usbd_add_configuration(usbd_thread, USBD_SPEED_FS,
				     &sample_fs_config);

	if (err) {
		LOG_ERR("Failed to add Full-Speed configuration");
		return;
	}
    LOG_INF("Added Full-Speed configuration");

	err = usbd_add_configuration(usbd_thread, USBD_SPEED_HS,
				     &sample_hs_config);

	if (err) {
		LOG_ERR("Failed to add High-Speed configuration");
		return;
	}
    LOG_INF("Added High-Speed configuration");

    err = usbd_init(usbd_thread);
    if (err) {
        LOG_ERR("Failed to initialize device support");
        return;
    }
    LOG_INF("Initialized USB device");

    err = usbd_enable(usbd_thread);
    if (err) {
        LOG_ERR("Failed to enable device support");
        return;
    }
    LOG_INF("Enabled  USB device");

    err = usbd_msg_register_cb(usbd_thread, msg_cb);
    if (err) {
        LOG_ERR("Failed to register message callback");
        return;
    }
    LOG_INF("Callback registered");
}

K_THREAD_DEFINE(iiod_usb, CONFIG_LIBIIO_IIOD_USB_THREAD_STACK_SIZE,
		iiod_usb_thread, usbd_ctx, NULL, NULL,
		CONFIG_LIBIIO_IIOD_USB_THREAD_PRIORITY, 0, 1);
