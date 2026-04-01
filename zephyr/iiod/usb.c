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
#include <zephyr/drivers/usb/udc.h>

LOG_MODULE_REGISTER(iiod_usb, CONFIG_LIBIIO_LOG_LEVEL);

/*
 * Instantiate a context named sample_usbd using the default USB device
 * controller, the Zephyr project vendor ID, and the sample product ID.
 * Zephyr project vendor ID must not be used outside of Zephyr samples.
 */
USBD_DEVICE_DEFINE(sample_usbd,
		   DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
		   CONFIG_IIOD_USBD_VID, CONFIG_IIOD_USBD_PID);

USBD_DESC_LANG_DEFINE(sample_lang);
USBD_DESC_MANUFACTURER_DEFINE(sample_mfr, CONFIG_IIOD_USBD_MANUFACTURER);
USBD_DESC_PRODUCT_DEFINE(sample_product, CONFIG_IIOD_USBD_PRODUCT);
IF_ENABLED(CONFIG_HWINFO, (USBD_DESC_SERIAL_NUMBER_DEFINE(sample_sn)));

USBD_DESC_STRING_DEFINE(iio_iface_str_desc, "IIO", USBD_DUT_STRING_INTERFACE);

USBD_DESC_CONFIG_DEFINE(fs_cfg_desc, "FS Configuration");
USBD_DESC_CONFIG_DEFINE(hs_cfg_desc, "HS Configuration");

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

/*
 * IIO USB class descriptor structure.
 *
 * Single interface with one bulk IN + one bulk OUT endpoint pair.
 * The host-side libiio (usb.c) identifies IIO devices by scanning
 * for interfaces whose iInterface string descriptor is "IIO".
 * Endpoint ordering: IN before OUT — the host's usb_verify_eps()
 * expects even-indexed endpoints to be IN, odd-indexed to be OUT.
 */
struct iio_usb_desc {
	struct usb_if_descriptor if0;
	struct usb_ep_descriptor if0_in_ep;
	struct usb_ep_descriptor if0_out_ep;
	struct usb_ep_descriptor if0_hs_in_ep;
	struct usb_ep_descriptor if0_hs_out_ep;
	struct usb_desc_header nil_desc;
};

static struct iio_usb_desc iio_usb_desc_0 = {
	.if0 = {
		.bLength = sizeof(struct usb_if_descriptor),
		.bDescriptorType = USB_DESC_INTERFACE,
		.bInterfaceNumber = 0,
		.bAlternateSetting = 0,
		.bNumEndpoints = 2,
		.bInterfaceClass = USB_BCC_CDC_CONTROL,
		.bInterfaceSubClass = 0,
		.bInterfaceProtocol = 0,
		.iInterface = 0, /* Set dynamically in init to "IIO" string index */
	},
	.if0_in_ep = {
		.bLength = sizeof(struct usb_ep_descriptor),
		.bDescriptorType = USB_DESC_ENDPOINT,
		.bEndpointAddress = 0x81,
		.bmAttributes = USB_EP_TYPE_BULK,
		.wMaxPacketSize = sys_cpu_to_le16(64U),
		.bInterval = 0x00,
	},
	.if0_out_ep = {
		.bLength = sizeof(struct usb_ep_descriptor),
		.bDescriptorType = USB_DESC_ENDPOINT,
		.bEndpointAddress = 0x01,
		.bmAttributes = USB_EP_TYPE_BULK,
		.wMaxPacketSize = sys_cpu_to_le16(64U),
		.bInterval = 0x00,
	},
	.if0_hs_in_ep = {
		.bLength = sizeof(struct usb_ep_descriptor),
		.bDescriptorType = USB_DESC_ENDPOINT,
		.bEndpointAddress = 0x81,
		.bmAttributes = USB_EP_TYPE_BULK,
		.wMaxPacketSize = sys_cpu_to_le16(512U),
		.bInterval = 0x00,
	},
	.if0_hs_out_ep = {
		.bLength = sizeof(struct usb_ep_descriptor),
		.bDescriptorType = USB_DESC_ENDPOINT,
		.bEndpointAddress = 0x01,
		.bmAttributes = USB_EP_TYPE_BULK,
		.wMaxPacketSize = sys_cpu_to_le16(512U),
		.bInterval = 0x00,
	},
	.nil_desc = {
		.bLength = 0,
		.bDescriptorType = 0,
	},
};

/* FS descriptor array: interface + FS endpoints + terminator */
static const struct usb_desc_header *iio_fs_desc_0[] = {
	(struct usb_desc_header *) &iio_usb_desc_0.if0,
	(struct usb_desc_header *) &iio_usb_desc_0.if0_in_ep,
	(struct usb_desc_header *) &iio_usb_desc_0.if0_out_ep,
	(struct usb_desc_header *) &iio_usb_desc_0.nil_desc,
};

/* HS descriptor array: interface + HS endpoints + terminator */
static const struct usb_desc_header *iio_hs_desc_0[] = {
	(struct usb_desc_header *) &iio_usb_desc_0.if0,
	(struct usb_desc_header *) &iio_usb_desc_0.if0_hs_in_ep,
	(struct usb_desc_header *) &iio_usb_desc_0.if0_hs_out_ep,
	(struct usb_desc_header *) &iio_usb_desc_0.nil_desc,
};

struct iio_usb_data {
    struct iio_usb_desc *const desc;
    const struct usb_desc_header **const fs_desc;
    const struct usb_desc_header **const hs_desc;
    atomic_t state;    
};

static struct iio_usb_data iio_usb_data_0 = {
    .desc = &iio_usb_desc_0,
    .fs_desc = iio_fs_desc_0,
    .hs_desc = iio_hs_desc_0,
};

static void *iio_usb_get_desc(struct usbd_class_data *const c_data,
                                const enum usbd_speed speed)
{
    struct iio_usb_data *data = usbd_class_get_private(c_data);

    if (USBD_SUPPORTS_HIGH_SPEED && speed == USBD_SPEED_HS) {
        return data->hs_desc;
    }

    return data->fs_desc;
}

static int iio_usb_init(struct usbd_class_data *const c_data)
{
    struct usbd_context *usbd_ctx = usbd_class_get_ctx(c_data);
    struct iio_usb_data *data = usbd_class_get_private(c_data);
    struct iio_usb_desc *desc = data->desc;

    if (desc->if0.iInterface == 0) {
        if (usbd_add_descriptor(usbd_ctx, &iio_iface_str_desc)) {
            LOG_ERR("Failed to add IIO interface string descriptor");
        }
        else {
            desc->if0.iInterface = usbd_str_desc_get_idx(&iio_iface_str_desc);
            LOG_INF("IIO inteface string at index %u", desc->if0.iInterface);
        }
    }

    return 0;
}

static void iio_usb_enable(struct usbd_class_data *const c_data)
{
    LOG_INF("IIO USB class enabled");
}

static void iio_usb_disable(struct usbd_class_data *const c_data)
{
    LOG_INF("IIO USB class disabled");
}

static struct usbd_class_api iio_usb_api = {
    .get_desc = iio_usb_get_desc,
    .enable = iio_usb_enable,
    .disable = iio_usb_disable,
    .init = iio_usb_init,
};

USBD_DEFINE_CLASS(iio_usb_0, &iio_usb_api, &iio_usb_data_0, NULL);

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

    err = usbd_register_all_classes(usbd_thread, USBD_SPEED_FS, 1, NULL);
    if (err) {
        LOG_ERR("Failed to register FS classes (%d)", err);
        return;
    }
    LOG_INF("Registered FS classes");

    err = usbd_register_all_classes(usbd_thread, USBD_SPEED_HS, 1, NULL);
    if (err) {
        LOG_ERR("Failed to register HS classes (%d)", err);
        return;
    }
    LOG_INF("Registered HS classes");

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
