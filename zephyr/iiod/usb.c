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
#include <string.h>
#include <stdio.h>

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

/* Maximum buffer size for USB bulk transfers */
#define IIO_USB_RX_BUF_SIZE 512
/* Use smaller TX chunks to avoid buffer pool exhaustion */
#define IIO_USB_TX_CHUNK_SIZE 64

struct iio_usb_data {
    struct iio_usb_desc *const desc;
    const struct usb_desc_header **const fs_desc;
    const struct usb_desc_header **const hs_desc;
    atomic_t state;
    struct usbd_class_data *c_data;
    struct k_sem rx_sem;
    struct k_sem tx_sem;
    struct k_sem enabled_sem;
    struct k_mutex rx_mutex;
    uint8_t rx_buffer[IIO_USB_RX_BUF_SIZE];
    size_t rx_len;
    size_t rx_offset;
    int rx_err;
    int tx_err;
    bool enabled;
};

static struct iio_usb_data iio_usb_data_0 = {
    .desc = &iio_usb_desc_0,
    .fs_desc = iio_fs_desc_0,
    .hs_desc = iio_hs_desc_0,
    .c_data = NULL,
    .enabled = false,
};

static uint8_t iio_usb_get_bulk_in(struct usbd_class_data *const c_data)
{
    struct usbd_context *uds_ctx = usbd_class_get_ctx(c_data);
    struct iio_usb_data *data = usbd_class_get_private(c_data);
    struct iio_usb_desc *desc = data->desc;

    if (usbd_bus_speed(uds_ctx) == USBD_SPEED_HS) {
        return desc->if0_hs_in_ep.bEndpointAddress;
    }

    return desc->if0_in_ep.bEndpointAddress;
}

static uint8_t iio_usb_get_bulk_out(struct usbd_class_data *const c_data)
{
    struct usbd_context *uds_ctx = usbd_class_get_ctx(c_data);
    struct iio_usb_data *data = usbd_class_get_private(c_data);
    struct iio_usb_desc *desc = data->desc;

    if (usbd_bus_speed(uds_ctx) == USBD_SPEED_HS) {
        return desc->if0_hs_out_ep.bEndpointAddress;
    }

    return desc->if0_out_ep.bEndpointAddress;
}

/* Queue a receive buffer on the OUT endpoint */
static int iio_usb_queue_rx(struct usbd_class_data *const c_data)
{
    struct iio_usb_data *data = usbd_class_get_private(c_data);
    struct net_buf *buf;
    int err;

    if (!data->enabled) {
        return -ENODEV;
    }

    buf = usbd_ep_buf_alloc(c_data, iio_usb_get_bulk_out(c_data), IIO_USB_RX_BUF_SIZE);
    if (buf == NULL) {
        LOG_ERR("Failed to allocate RX buffer");
        return -ENOMEM;
    }

    err = usbd_ep_enqueue(c_data, buf);
    if (err) {
        LOG_ERR("Failed to enqueue RX buffer: %d", err);
        net_buf_unref(buf);
        return err;
    }

    LOG_DBG("RX buffer queued");
    return 0;
}

static void *iio_usb_get_desc(struct usbd_class_data *const c_data,
                                const enum usbd_speed speed)
{
    struct iio_usb_data *data = usbd_class_get_private(c_data);

    if (USBD_SUPPORTS_HIGH_SPEED && speed == USBD_SPEED_HS) {
        return data->hs_desc;
    }

    return data->fs_desc;
}

static int iio_usb_request_handler(struct usbd_class_data *const c_data,
                                    struct net_buf *const buf, const int err)
{
    struct udc_buf_info *bi = (struct udc_buf_info *)net_buf_user_data(buf);
    struct iio_usb_data *data = usbd_class_get_private(c_data);
    const uint8_t ep = bi->ep;

    if (ep == iio_usb_get_bulk_out(c_data)) {
        /* Received data from host (RX) */
        LOG_DBG("RX complete: err=%d, len=%u", err, buf->len);

        if (err == 0 && buf->len > 0) {
            /* Copy received data to internal buffer */
            data->rx_len = MIN(buf->len, IIO_USB_RX_BUF_SIZE);
            memcpy(data->rx_buffer, buf->data, data->rx_len);
            data->rx_offset = 0;
            data->rx_err = 0;

            /* Hex dump of received data */
            LOG_INF("RX [%zu bytes]:", data->rx_len);
            for (size_t i = 0; i < data->rx_len; i += 16) {
                char hex_str[64] = {0};
                char ascii_str[17] = {0};
                for (size_t j = 0; j < 16 && (i + j) < data->rx_len; j++) {
                    sprintf(hex_str + j*3, "%02x ", data->rx_buffer[i + j]);
                    ascii_str[j] = (data->rx_buffer[i + j] >= 32 && data->rx_buffer[i + j] < 127)
                                   ? data->rx_buffer[i + j] : '.';
                }
                LOG_INF("  %04zx: %-48s %s", i, hex_str, ascii_str);
            }
        } else {
            data->rx_len = 0;
            data->rx_err = err ? err : -EIO;
        }

        net_buf_unref(buf);

        /* Signal that data is available */
        k_sem_give(&data->rx_sem);

        /* Automatically re-queue another receive buffer */
        iio_usb_queue_rx(c_data);

    } else if (ep == iio_usb_get_bulk_in(c_data)) {
        /* Sent data to host (TX) */
        LOG_DBG("TX complete: err=%d", err);
        data->tx_err = err;
        net_buf_unref(buf);
        k_sem_give(&data->tx_sem);
    }

    return 0;
}

static int iio_usb_init(struct usbd_class_data *const c_data)
{
    struct usbd_context *usbd_ctx = usbd_class_get_ctx(c_data);
    struct iio_usb_data *data = usbd_class_get_private(c_data);
    struct iio_usb_desc *desc = data->desc;

    /* Store class data pointer for use in read/write functions */
    data->c_data = c_data;

    /* Initialize synchronization primitives */
    k_sem_init(&data->rx_sem, 0, 1);
    k_sem_init(&data->tx_sem, 0, 1);
    k_sem_init(&data->enabled_sem, 0, 1);
    k_mutex_init(&data->rx_mutex);

    /* Initialize RX buffer state */
    data->rx_len = 0;
    data->rx_offset = 0;
    data->rx_err = 0;
    data->tx_err = 0;

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

static int iio_usb_control_to_host(struct usbd_class_data *c_data,
                                    const struct usb_setup_packet *const setup,
                                    struct net_buf *const buf)
{
    LOG_DBG("Control to host: bRequest=0x%02x, wValue=0x%04x, wIndex=0x%04x, wLength=%u",
            setup->bRequest, setup->wValue, setup->wIndex, setup->wLength);

    /* For now, return empty response for vendor requests */
    /* The host might be probing capabilities */
    return 0;
}

static int iio_usb_control_to_dev(struct usbd_class_data *c_data,
                                   const struct usb_setup_packet *const setup,
                                   const struct net_buf *const buf)
{
    LOG_DBG("Control to dev: bRequest=0x%02x, wValue=0x%04x, wIndex=0x%04x, wLength=%u",
            setup->bRequest, setup->wValue, setup->wIndex, setup->wLength);

    /* Accept vendor requests without error */
    return 0;
}

static void iio_usb_enable(struct usbd_class_data *const c_data)
{
    struct iio_usb_data *data = usbd_class_get_private(c_data);

    LOG_INF("IIO USB class enabled");
    data->enabled = true;

    /* Queue initial receive buffer */
    if (iio_usb_queue_rx(c_data)) {
        LOG_ERR("Failed to queue initial RX buffer");
    }

    /* Signal that USB is ready for IIOD interpreter */
    k_sem_give(&data->enabled_sem);
    LOG_INF("USB ready - signaled iiod_interpreter to start");
}

static void iio_usb_disable(struct usbd_class_data *const c_data)
{
    struct iio_usb_data *data = usbd_class_get_private(c_data);

    LOG_INF("IIO USB class disabled");
    data->enabled = false;
}

static struct usbd_class_api iio_usb_api = {
    .get_desc = iio_usb_get_desc,
    .control_to_host = iio_usb_control_to_host,
    .control_to_dev = iio_usb_control_to_dev,
    .request = iio_usb_request_handler,
    .enable = iio_usb_enable,
    .disable = iio_usb_disable,
    .init = iio_usb_init,
};

USBD_DEFINE_CLASS(iio_usb_0, &iio_usb_api, &iio_usb_data_0, NULL);

static ssize_t iiod_usb_read(struct iiod_pdata *pdata, void *buf, size_t size)
{
    struct iio_usb_data *data = &iio_usb_data_0;
    size_t bytes_read = 0;
    uint8_t *dest = (uint8_t *)buf;
    int err;

    if (data->c_data == NULL || !data->enabled) {
        LOG_ERR("USB not initialized or not enabled");
        return -ENODEV;
    }

    LOG_INF(">>> iiod_usb_read called: %zu bytes requested", size);

    k_mutex_lock(&data->rx_mutex, K_FOREVER);

    while (bytes_read < size) {
        /* If buffer is empty, wait for new data */
        if (data->rx_offset >= data->rx_len) {
            k_mutex_unlock(&data->rx_mutex);

            LOG_DBG("Waiting for RX data...");
            err = k_sem_take(&data->rx_sem, K_FOREVER);
            if (err) {
                LOG_ERR("RX semaphore error: %d", err);
                return err;
            }

            k_mutex_lock(&data->rx_mutex, K_FOREVER);

            /* Check for receive error */
            if (data->rx_err) {
                LOG_ERR("USB RX error: %d", data->rx_err);
                k_mutex_unlock(&data->rx_mutex);
                return data->rx_err;
            }
        }

        /* Copy available data from internal buffer */
        size_t available = data->rx_len - data->rx_offset;
        size_t to_copy = MIN(available, size - bytes_read);

        memcpy(dest + bytes_read, data->rx_buffer + data->rx_offset, to_copy);
        data->rx_offset += to_copy;
        bytes_read += to_copy;

        LOG_DBG("Read %zu bytes, total %zu/%zu", to_copy, bytes_read, size);
    }

    k_mutex_unlock(&data->rx_mutex);
    LOG_INF(">>> iiod_usb_read returning: %zd bytes", bytes_read);
    return bytes_read;
}

static ssize_t iiod_usb_write(struct iiod_pdata *pdata, const void *buf, size_t size)
{
    struct iio_usb_data *data = &iio_usb_data_0;
    struct usbd_class_data *c_data = data->c_data;
    const uint8_t *src = (const uint8_t *)buf;
    struct net_buf *net_buf;
    int err;

    if (c_data == NULL || !data->enabled) {
        LOG_ERR("USB not initialized or not enabled");
        return -ENODEV;
    }

    LOG_INF("TX [%zu bytes total]:", size);

    /* Send data in chunks to avoid buffer allocation failures */
    size_t bytes_sent = 0;
    while (bytes_sent < size) {
        size_t chunk_size = MIN(size - bytes_sent, IIO_USB_TX_CHUNK_SIZE);

        /* Hex dump of first chunk only to avoid spam */
        if (bytes_sent == 0 && chunk_size <= 64) {
            for (size_t i = 0; i < chunk_size; i += 16) {
                char hex_str[64] = {0};
                char ascii_str[17] = {0};
                for (size_t j = 0; j < 16 && (i + j) < chunk_size; j++) {
                    sprintf(hex_str + j*3, "%02x ", src[i + j]);
                    ascii_str[j] = (src[i + j] >= 32 && src[i + j] < 127) ? src[i + j] : '.';
                }
                LOG_INF("  %04zx: %-48s %s", i, hex_str, ascii_str);
            }
        }

        /* Allocate buffer for IN endpoint (device to host) */
        net_buf = usbd_ep_buf_alloc(c_data, iio_usb_get_bulk_in(c_data), chunk_size);
        if (net_buf == NULL) {
            LOG_ERR("Failed to allocate TX buffer for %zu bytes", chunk_size);
            return bytes_sent > 0 ? bytes_sent : -ENOMEM;
        }

        /* Copy chunk to buffer */
        net_buf_add_mem(net_buf, src + bytes_sent, chunk_size);

        /* Reset TX error */
        data->tx_err = 0;

        /* Queue the buffer to send data */
        err = usbd_ep_enqueue(c_data, net_buf);
        if (err) {
            LOG_ERR("Failed to enqueue TX buffer: %d", err);
            net_buf_unref(net_buf);
            return bytes_sent > 0 ? bytes_sent : err;
        }

        /* Wait for transfer to complete */
        err = k_sem_take(&data->tx_sem, K_FOREVER);
        if (err) {
            LOG_ERR("TX semaphore error: %d", err);
            return bytes_sent > 0 ? bytes_sent : err;
        }

        /* Check if transfer had an error */
        if (data->tx_err) {
            LOG_ERR("USB TX transfer failed: %d", data->tx_err);
            return bytes_sent > 0 ? bytes_sent : data->tx_err;
        }

        bytes_sent += chunk_size;
        LOG_DBG("TX chunk %zu/%zu bytes sent", bytes_sent, size);
    }

    LOG_INF(">>> iiod_usb_write complete: %zu bytes", bytes_sent);
    return bytes_sent;
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
	struct iio_context_params ctx_params = {0};
	static struct iio_context *ctx = NULL;
	static const void *xml = NULL;
	static size_t xml_len = 0;

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

	LOG_INF("Initializing tinyiiod resources...");
	if (iiod_init() < 0) {
		LOG_ERR("Failed to initialize tinyiiod resources");
		return;
	}

	LOG_INF("Creating shared IIO context...");
	ctx = iio_create_context(&ctx_params, "zephyr:");
	if (iio_err(ctx)) {
		LOG_ERR("Context creation failed");
		iiod_cleanup();
		return;
	}

	LOG_INF("Getting xml data");
	xml = iio_context_get_xml(ctx);
	if (!xml) {
		LOG_ERR("Error getting context XML");
		iio_context_destroy(ctx);
		iiod_cleanup();
		return;
	}

	xml_len = strlen(xml) + 1;
	LOG_INF("XML ready, length: %zu bytes", xml_len);

	/* Wait for USB to be enabled before starting interpreter */
	LOG_INF("Waiting for USB enumeration and configuration...");
	k_sem_take(&iio_usb_data_0.enabled_sem, K_FOREVER);
	LOG_INF("USB is ready!");

	LOG_INF("Starting IIOD interpreter");

	iiod_interpreter(ctx, (struct iiod_pdata *)usbd_ctx,
		iiod_usb_read, iiod_usb_write,
		xml, xml_len);

	LOG_INF("Exited IIOD interpreter");

	iio_context_destroy(ctx);
	iiod_cleanup();

	LOG_DBG("USB thread exiting");
}

K_THREAD_DEFINE(iiod_usb, CONFIG_LIBIIO_IIOD_USB_THREAD_STACK_SIZE,
		iiod_usb_thread, usbd_ctx, NULL, NULL,
		CONFIG_LIBIIO_IIOD_USB_THREAD_PRIORITY, 0, 1);
