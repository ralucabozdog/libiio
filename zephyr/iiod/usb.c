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
#include <zephyr/sys/ring_buffer.h>
#include <string.h>
#include <stdio.h>

LOG_MODULE_REGISTER(iiod_usb, CONFIG_LIBIIO_LOG_LEVEL);

#define IIO_USB_NUM_PIPES CONFIG_LIBIIO_IIOD_USB_NUM_PIPES

/* Vendor control request commands (must match host-side libiio usb.c) */
#define IIO_USD_CMD_RESET_PIPES 0
#define IIO_USD_CMD_OPEN_PIPE   1
#define IIO_USD_CMD_CLOSE_PIPE  2

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
 * Single interface with IIO_USB_NUM_PIPES bulk IN/OUT endpoint pairs.
 * The host-side libiio (usb.c) identifies IIO devices by scanning
 * for interfaces whose iInterface string descriptor is "IIO".
 * Endpoint ordering: IN before OUT — the host's usb_verify_eps()
 * expects even-indexed endpoints to be IN, odd-indexed to be OUT.
 */
struct iio_usb_desc {
	struct usb_if_descriptor if0;
	struct usb_ep_descriptor if0_ep[IIO_USB_NUM_PIPES * 2];    /* FS endpoints */
	struct usb_ep_descriptor if0_hs_ep[IIO_USB_NUM_PIPES * 2]; /* HS endpoints */
	struct usb_desc_header nil_desc;
};

/* Build the descriptor initializer with endpoint pairs for each pipe.
 * Each pipe N gets: IN endpoint 0x81+N, OUT endpoint 0x01+N */
#define EP_FS_IN(n)  \
	[n * 2] = { \
		.bLength = sizeof(struct usb_ep_descriptor), \
		.bDescriptorType = USB_DESC_ENDPOINT, \
		.bEndpointAddress = 0x81 + n, \
		.bmAttributes = USB_EP_TYPE_BULK, \
		.wMaxPacketSize = sys_cpu_to_le16(64U), \
		.bInterval = 0x00, \
	}

#define EP_FS_OUT(n) \
	[n * 2 + 1] = { \
		.bLength = sizeof(struct usb_ep_descriptor), \
		.bDescriptorType = USB_DESC_ENDPOINT, \
		.bEndpointAddress = 0x01 + n, \
		.bmAttributes = USB_EP_TYPE_BULK, \
		.wMaxPacketSize = sys_cpu_to_le16(64U), \
		.bInterval = 0x00, \
	}

#define EP_HS_IN(n)  \
	[n * 2] = { \
		.bLength = sizeof(struct usb_ep_descriptor), \
		.bDescriptorType = USB_DESC_ENDPOINT, \
		.bEndpointAddress = 0x81 + n, \
		.bmAttributes = USB_EP_TYPE_BULK, \
		.wMaxPacketSize = sys_cpu_to_le16(512U), \
		.bInterval = 0x00, \
	}

#define EP_HS_OUT(n) \
	[n * 2 + 1] = { \
		.bLength = sizeof(struct usb_ep_descriptor), \
		.bDescriptorType = USB_DESC_ENDPOINT, \
		.bEndpointAddress = 0x01 + n, \
		.bmAttributes = USB_EP_TYPE_BULK, \
		.wMaxPacketSize = sys_cpu_to_le16(512U), \
		.bInterval = 0x00, \
	}

#define EP_FS_PAIR(n) EP_FS_IN(n), EP_FS_OUT(n)
#define EP_HS_PAIR(n) EP_HS_IN(n), EP_HS_OUT(n)

static struct iio_usb_desc iio_usb_desc_0 = {
	.if0 = {
		.bLength = sizeof(struct usb_if_descriptor),
		.bDescriptorType = USB_DESC_INTERFACE,
		.bInterfaceNumber = 0,
		.bAlternateSetting = 0,
		.bNumEndpoints = IIO_USB_NUM_PIPES * 2,
		.bInterfaceClass = USB_BCC_CDC_CONTROL,
		.bInterfaceSubClass = 0,
		.bInterfaceProtocol = 0,
		.iInterface = 0, /* Set dynamically in init to "IIO" string index */
	},
	.if0_ep = {
		EP_FS_PAIR(0),
#if IIO_USB_NUM_PIPES > 1
		EP_FS_PAIR(1),
#endif
#if IIO_USB_NUM_PIPES > 2
		EP_FS_PAIR(2),
#endif
#if IIO_USB_NUM_PIPES > 3
		EP_FS_PAIR(3),
#endif
#if IIO_USB_NUM_PIPES > 4
		EP_FS_PAIR(4),
#endif
#if IIO_USB_NUM_PIPES > 5
		EP_FS_PAIR(5),
#endif
	},
	.if0_hs_ep = {
		EP_HS_PAIR(0),
#if IIO_USB_NUM_PIPES > 1
		EP_HS_PAIR(1),
#endif
#if IIO_USB_NUM_PIPES > 2
		EP_HS_PAIR(2),
#endif
#if IIO_USB_NUM_PIPES > 3
		EP_HS_PAIR(3),
#endif
#if IIO_USB_NUM_PIPES > 4
		EP_HS_PAIR(4),
#endif
#if IIO_USB_NUM_PIPES > 5
		EP_HS_PAIR(5),
#endif
	},
	.nil_desc = {
		.bLength = 0,
		.bDescriptorType = 0,
	},
};

/*
 * Build FS and HS descriptor pointer arrays.
 * Format: interface descriptor, then all FS (or HS) endpoint descriptors,
 * then nil terminator.
 */
#define FS_EP_PTR(n) \
	(struct usb_desc_header *) &iio_usb_desc_0.if0_ep[n * 2], \
	(struct usb_desc_header *) &iio_usb_desc_0.if0_ep[n * 2 + 1]

#define HS_EP_PTR(n) \
	(struct usb_desc_header *) &iio_usb_desc_0.if0_hs_ep[n * 2], \
	(struct usb_desc_header *) &iio_usb_desc_0.if0_hs_ep[n * 2 + 1]

static const struct usb_desc_header *iio_fs_desc_0[] = {
	(struct usb_desc_header *) &iio_usb_desc_0.if0,
	FS_EP_PTR(0),
#if IIO_USB_NUM_PIPES > 1
	FS_EP_PTR(1),
#endif
#if IIO_USB_NUM_PIPES > 2
	FS_EP_PTR(2),
#endif
#if IIO_USB_NUM_PIPES > 3
	FS_EP_PTR(3),
#endif
#if IIO_USB_NUM_PIPES > 4
	FS_EP_PTR(4),
#endif
#if IIO_USB_NUM_PIPES > 5
	FS_EP_PTR(5),
#endif
	(struct usb_desc_header *) &iio_usb_desc_0.nil_desc,
};

static const struct usb_desc_header *iio_hs_desc_0[] = {
	(struct usb_desc_header *) &iio_usb_desc_0.if0,
	HS_EP_PTR(0),
#if IIO_USB_NUM_PIPES > 1
	HS_EP_PTR(1),
#endif
#if IIO_USB_NUM_PIPES > 2
	HS_EP_PTR(2),
#endif
#if IIO_USB_NUM_PIPES > 3
	HS_EP_PTR(3),
#endif
#if IIO_USB_NUM_PIPES > 4
	HS_EP_PTR(4),
#endif
#if IIO_USB_NUM_PIPES > 5
	HS_EP_PTR(5),
#endif
	(struct usb_desc_header *) &iio_usb_desc_0.nil_desc,
};

/* Maximum buffer size for a single USB bulk transfer */
#define IIO_USB_RX_BUF_SIZE 512
/* Use smaller TX chunks to avoid buffer pool exhaustion */
#define IIO_USB_TX_CHUNK_SIZE 64
/* Size of the per-pipe RX FIFO — must hold multiple in-flight packets */
#define IIO_USB_RX_FIFO_SIZE (IIO_USB_RX_BUF_SIZE * 8)

/*
 * Per-pipe state. Each pipe has its own RX FIFO and TX semaphore,
 * and its own endpoint addresses.
 *
 * The RX path uses a k_pipe (byte-stream FIFO) so that multiple USB
 * packets arriving while the interpreter is busy are queued rather
 * than overwritten.
 */
struct iio_usb_pipe {
    uint8_t ep_in;       /* IN endpoint address (e.g., 0x81) */
    uint8_t ep_out;      /* OUT endpoint address (e.g., 0x01) */
    struct ring_buf rx_ringbuf;
    uint8_t rx_ringbuf_data[IIO_USB_RX_FIFO_SIZE];
    struct k_sem rx_sem; /* signaled when data is added to ring buffer */
    struct k_sem tx_sem;
    int rx_err;
    int tx_err;
    bool open;           /* Whether this pipe has been opened by host */
};

struct iio_usb_data {
    struct iio_usb_desc *const desc;
    const struct usb_desc_header **const fs_desc;
    const struct usb_desc_header **const hs_desc;
    atomic_t state;
    struct usbd_class_data *c_data;
    struct k_sem enabled_sem;
    struct iio_usb_pipe pipes[IIO_USB_NUM_PIPES];
    bool enabled;

    /* Shared IIO context and XML for spawning pipe interpreters */
    struct iio_context *ctx;
    const void *xml;
    size_t xml_len;
};

static struct iio_usb_data iio_usb_data_0 = {
    .desc = &iio_usb_desc_0,
    .fs_desc = iio_fs_desc_0,
    .hs_desc = iio_hs_desc_0,
    .c_data = NULL,
    .enabled = false,
};

/* Pipe thread stacks for data pipes (pipe 0 uses the main USB thread) */
#if IIO_USB_NUM_PIPES > 1
static K_THREAD_STACK_ARRAY_DEFINE(pipe_stacks, IIO_USB_NUM_PIPES - 1,
				   CONFIG_LIBIIO_IIOD_USB_PIPE_THREAD_STACK_SIZE);
static struct k_thread pipe_threads[IIO_USB_NUM_PIPES - 1];
#endif

/*
 * Find the pipe that owns a given endpoint address.
 * Returns the pipe index or -1 if not found.
 */
static int find_pipe_by_ep(struct iio_usb_data *data, uint8_t ep_addr)
{
    for (int i = 0; i < IIO_USB_NUM_PIPES; i++) {
        if (data->pipes[i].ep_in == ep_addr ||
            data->pipes[i].ep_out == ep_addr) {
            return i;
        }
    }
    return -1;
}

/*
 * Get the correct IN/OUT endpoint address for a pipe, accounting for
 * bus speed (FS vs HS). Since both FS and HS descriptors use the same
 * endpoint addresses (just different max packet sizes), we can just
 * return the pipe's stored address directly.
 */
static uint8_t pipe_get_bulk_in(struct iio_usb_pipe *pipe)
{
    return pipe->ep_in;
}

static uint8_t pipe_get_bulk_out(struct iio_usb_pipe *pipe)
{
    return pipe->ep_out;
}

/* Queue a receive buffer on a specific pipe's OUT endpoint */
static int iio_usb_queue_rx_pipe(struct usbd_class_data *const c_data,
				 struct iio_usb_pipe *pipe)
{
    struct iio_usb_data *data = usbd_class_get_private(c_data);
    struct net_buf *buf;
    int err;

    if (!data->enabled) {
        return -ENODEV;
    }

    buf = usbd_ep_buf_alloc(c_data, pipe_get_bulk_out(pipe), IIO_USB_RX_BUF_SIZE);
    if (buf == NULL) {
        LOG_ERR("Pipe 0x%02x: Failed to allocate RX buffer", pipe->ep_out);
        return -ENOMEM;
    }

    err = usbd_ep_enqueue(c_data, buf);
    if (err) {
        LOG_ERR("Pipe 0x%02x: Failed to enqueue RX buffer: %d", pipe->ep_out, err);
        net_buf_unref(buf);
        return err;
    }

    LOG_DBG("Pipe 0x%02x: RX buffer queued", pipe->ep_out);
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
    int pipe_idx;
    struct iio_usb_pipe *pipe;

    pipe_idx = find_pipe_by_ep(data, ep);
    if (pipe_idx < 0) {
        LOG_ERR("Unknown endpoint 0x%02x", ep);
        net_buf_unref(buf);
        return -EINVAL;
    }
    pipe = &data->pipes[pipe_idx];

    if (ep == pipe->ep_out) {
        /* Received data from host (RX) — append to FIFO */
        LOG_DBG("Pipe %d RX complete: err=%d, len=%u", pipe_idx, err, buf->len);

        if (err == 0 && buf->len > 0) {
            uint32_t written = ring_buf_put(&pipe->rx_ringbuf,
                                            buf->data, buf->len);
            if (written < buf->len) {
                LOG_ERR("Pipe %d: RX FIFO overflow, lost %u bytes",
                        pipe_idx, buf->len - written);
            }
            pipe->rx_err = 0;
            k_sem_give(&pipe->rx_sem);
        } else {
            pipe->rx_err = err ? err : -EIO;
            k_sem_give(&pipe->rx_sem);
        }

        net_buf_unref(buf);

        /* Re-queue another receive buffer */
        iio_usb_queue_rx_pipe(c_data, pipe);

    } else if (ep == pipe->ep_in) {
        /* Sent data to host (TX) */
        LOG_DBG("Pipe %d TX complete: err=%d", pipe_idx, err);
        pipe->tx_err = err;
        net_buf_unref(buf);
        k_sem_give(&pipe->tx_sem);
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

    /*
     * Initialize per-pipe state.
     * Read the actual endpoint addresses from the descriptor struct —
     * the Zephyr USB stack remaps bEndpointAddress during class
     * registration (usbd_register_all_classes), so the addresses in
     * the struct now reflect the real UDC endpoint addresses, not
     * the placeholder values we put in the initializer.
     */
    for (int i = 0; i < IIO_USB_NUM_PIPES; i++) {
        struct iio_usb_pipe *pipe = &data->pipes[i];

        pipe->ep_in = desc->if0_ep[i * 2].bEndpointAddress;
        pipe->ep_out = desc->if0_ep[i * 2 + 1].bEndpointAddress;
        ring_buf_init(&pipe->rx_ringbuf, sizeof(pipe->rx_ringbuf_data),
                      pipe->rx_ringbuf_data);
        k_sem_init(&pipe->rx_sem, 0, K_SEM_MAX_LIMIT);
        k_sem_init(&pipe->tx_sem, 0, 1);
        pipe->rx_err = 0;
        pipe->tx_err = 0;
        pipe->open = false;

        LOG_INF("Pipe %d: IN=0x%02x OUT=0x%02x", i, pipe->ep_in, pipe->ep_out);
    }

    k_sem_init(&data->enabled_sem, 0, 1);

    if (desc->if0.iInterface == 0) {
        if (usbd_add_descriptor(usbd_ctx, &iio_iface_str_desc)) {
            LOG_ERR("Failed to add IIO interface string descriptor");
        }
        else {
            desc->if0.iInterface = usbd_str_desc_get_idx(&iio_iface_str_desc);
            LOG_INF("IIO interface string at index %u", desc->if0.iInterface);
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
    return 0;
}

/*
 * Read/write callbacks for pipe interpreter threads.
 * The pdata pointer is the iio_usb_pipe struct for the pipe.
 */
static ssize_t iiod_usb_pipe_read(struct iiod_pdata *pdata, void *buf, size_t size)
{
    struct iio_usb_pipe *pipe = (struct iio_usb_pipe *)pdata;
    size_t bytes_read = 0;
    uint8_t *dest = (uint8_t *)buf;

    LOG_DBG("Pipe 0x%02x: read %zu bytes requested", pipe->ep_out, size);

    while (bytes_read < size) {
        uint32_t got = ring_buf_get(&pipe->rx_ringbuf,
                                     dest + bytes_read,
                                     size - bytes_read);
        bytes_read += got;

        if (bytes_read < size) {
            /* Ring buffer empty — wait for more data */
            k_sem_take(&pipe->rx_sem, K_FOREVER);

            if (pipe->rx_err) {
                /* Only log as error if pipe is still open.
                 * During pipe closure, pending USB transfers are cancelled
                 * and return -ETIMEDOUT, which is expected behavior. */
                if (pipe->open) {
                    LOG_ERR("Pipe 0x%02x: USB RX error: %d",
                            pipe->ep_out, pipe->rx_err);
                } else {
                    LOG_DBG("Pipe 0x%02x: RX error during shutdown: %d",
                            pipe->ep_out, pipe->rx_err);
                }
                return pipe->rx_err;
            }
        }
    }

    return bytes_read;
}

static ssize_t iiod_usb_pipe_write(struct iiod_pdata *pdata, const void *buf, size_t size)
{
    struct iio_usb_pipe *pipe = (struct iio_usb_pipe *)pdata;
    struct iio_usb_data *data = &iio_usb_data_0;
    struct usbd_class_data *c_data = data->c_data;
    const uint8_t *src = (const uint8_t *)buf;
    struct net_buf *net_buf;
    int err;

    if (c_data == NULL || !data->enabled) {
        return -ENODEV;
    }

    LOG_DBG("Pipe 0x%02x: TX %zu bytes", pipe->ep_in, size);

    size_t bytes_sent = 0;
    while (bytes_sent < size) {
        size_t chunk_size = MIN(size - bytes_sent, IIO_USB_TX_CHUNK_SIZE);

        net_buf = usbd_ep_buf_alloc(c_data, pipe_get_bulk_in(pipe), chunk_size);
        if (net_buf == NULL) {
            LOG_ERR("Pipe 0x%02x: Failed to allocate TX buffer for %zu bytes",
                    pipe->ep_in, chunk_size);
            return bytes_sent > 0 ? bytes_sent : -ENOMEM;
        }

        net_buf_add_mem(net_buf, src + bytes_sent, chunk_size);
        pipe->tx_err = 0;

        err = usbd_ep_enqueue(c_data, net_buf);
        if (err) {
            LOG_ERR("Pipe 0x%02x: Failed to enqueue TX buffer: %d", pipe->ep_in, err);
            net_buf_unref(net_buf);
            return bytes_sent > 0 ? bytes_sent : err;
        }

        err = k_sem_take(&pipe->tx_sem, K_FOREVER);
        if (err) {
            LOG_ERR("Pipe 0x%02x: TX semaphore error: %d", pipe->ep_in, err);
            return bytes_sent > 0 ? bytes_sent : err;
        }

        if (pipe->tx_err) {
            LOG_ERR("Pipe 0x%02x: USB TX transfer failed: %d", pipe->ep_in, pipe->tx_err);
            return bytes_sent > 0 ? bytes_sent : pipe->tx_err;
        }

        bytes_sent += chunk_size;
    }

    return bytes_sent;
}

/*
 * Thread entry for data pipe interpreters (pipes 1..N-1).
 * Each runs its own iiod_interpreter using the pipe's endpoints.
 */
#if IIO_USB_NUM_PIPES > 1
static void pipe_interpreter_thread(void *p1, void *p2, void *p3)
{
    struct iio_usb_pipe *pipe = (struct iio_usb_pipe *)p1;
    struct iio_usb_data *data = (struct iio_usb_data *)p2;
    int pipe_idx = (int)(intptr_t)p3;

    LOG_INF("Pipe %d: interpreter thread started", pipe_idx);

    iiod_interpreter(data->ctx, (struct iiod_pdata *)pipe,
                     iiod_usb_pipe_read, iiod_usb_pipe_write,
                     data->xml, data->xml_len);

    LOG_INF("Pipe %d: interpreter thread exiting", pipe_idx);
    pipe->open = false;
}
#endif

/*
 * Handle IIO vendor control requests for pipe management.
 * The host-side libiio sends these as VENDOR|RECIPIENT_INTERFACE requests:
 *   bRequest = command (OPEN/CLOSE/RESET)
 *   wValue   = pipe_id
 */
static int iio_usb_control_to_dev(struct usbd_class_data *c_data,
                                   const struct usb_setup_packet *const setup,
                                   const struct net_buf *const buf)
{
    struct iio_usb_data *data = usbd_class_get_private(c_data);
    uint8_t request = setup->bRequest;
    uint16_t pipe_id = setup->wValue;

    LOG_INF("Control to dev: bRequest=0x%02x, wValue=0x%04x, wIndex=0x%04x",
            request, pipe_id, setup->wIndex);

    switch (request) {
    case IIO_USD_CMD_RESET_PIPES:
        LOG_INF("RESET_PIPES");
#if IIO_USB_NUM_PIPES > 1
        for (int i = 1; i < IIO_USB_NUM_PIPES; i++) {
            if (data->pipes[i].open) {
                data->pipes[i].open = false;
                data->pipes[i].rx_err = -ESHUTDOWN;
                k_sem_give(&data->pipes[i].rx_sem);
            }
        }
#endif
        break;

    case IIO_USD_CMD_OPEN_PIPE:
        LOG_INF("OPEN_PIPE %u", pipe_id);
        if (pipe_id >= IIO_USB_NUM_PIPES) {
            LOG_ERR("Invalid pipe_id %u (max %d)", pipe_id, IIO_USB_NUM_PIPES - 1);
            return -EINVAL;
        }
        if (data->pipes[pipe_id].open) {
            LOG_WRN("Pipe %u already open", pipe_id);
            return 0;
        }
        data->pipes[pipe_id].open = true;

        /* Queue initial RX buffer for this pipe */
        iio_usb_queue_rx_pipe(c_data, &data->pipes[pipe_id]);

#if IIO_USB_NUM_PIPES > 1
        /* Spawn interpreter thread for data pipes (not pipe 0) */
        if (pipe_id > 0 && data->ctx != NULL) {
            struct iio_usb_pipe *pipe = &data->pipes[pipe_id];
            int idx = pipe_id - 1;

            /* Reset pipe state for clean start */
            ring_buf_reset(&pipe->rx_ringbuf);
            k_sem_reset(&pipe->rx_sem);
            pipe->rx_err = 0;
            pipe->tx_err = 0;
            k_sem_reset(&pipe->tx_sem);

            k_thread_create(&pipe_threads[idx],
                            pipe_stacks[idx],
                            CONFIG_LIBIIO_IIOD_USB_PIPE_THREAD_STACK_SIZE,
                            pipe_interpreter_thread,
                            pipe, data, (void *)(intptr_t)pipe_id,
                            CONFIG_LIBIIO_IIOD_USB_THREAD_PRIORITY, 0, K_NO_WAIT);

            LOG_INF("Pipe %u: interpreter thread spawned", pipe_id);
        }
#endif
        break;

    case IIO_USD_CMD_CLOSE_PIPE:
        LOG_INF("CLOSE_PIPE %u", pipe_id);
        if (pipe_id >= IIO_USB_NUM_PIPES) {
            return -EINVAL;
        }
        if (data->pipes[pipe_id].open) {
            data->pipes[pipe_id].open = false;
            data->pipes[pipe_id].rx_err = -ESHUTDOWN;
            k_sem_give(&data->pipes[pipe_id].rx_sem);
        }
        break;

    default:
        LOG_DBG("Unknown vendor request 0x%02x", request);
        break;
    }

    return 0;
}

static void iio_usb_enable(struct usbd_class_data *const c_data)
{
    struct iio_usb_data *data = usbd_class_get_private(c_data);

    LOG_INF("IIO USB class enabled");
    data->enabled = true;

    /* Queue initial receive buffer for pipe 0 (command channel) */
    data->pipes[0].open = true;
    if (iio_usb_queue_rx_pipe(c_data, &data->pipes[0])) {
        LOG_ERR("Failed to queue initial RX buffer for pipe 0");
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
	iio_usb_data_0.ctx = iio_create_context(&ctx_params, "zephyr:");
	if (iio_err(iio_usb_data_0.ctx)) {
		LOG_ERR("Context creation failed");
		iiod_cleanup();
		return;
	}

	LOG_INF("Getting xml data");
	iio_usb_data_0.xml = iio_context_get_xml(iio_usb_data_0.ctx);
	if (!iio_usb_data_0.xml) {
		LOG_ERR("Error getting context XML");
		iio_context_destroy(iio_usb_data_0.ctx);
		iiod_cleanup();
		return;
	}

	iio_usb_data_0.xml_len = strlen(iio_usb_data_0.xml) + 1;
	LOG_INF("XML ready, length: %zu bytes", iio_usb_data_0.xml_len);

	/* Wait for USB to be enabled before starting interpreter */
	LOG_INF("Waiting for USB enumeration and configuration...");
	k_sem_take(&iio_usb_data_0.enabled_sem, K_FOREVER);
	LOG_INF("USB is ready!");

	LOG_INF("Starting IIOD interpreter on pipe 0");

	/* Pipe 0 runs the command-channel interpreter in this thread */
	iiod_interpreter(iio_usb_data_0.ctx,
			 (struct iiod_pdata *)&iio_usb_data_0.pipes[0],
			 iiod_usb_pipe_read, iiod_usb_pipe_write,
			 iio_usb_data_0.xml, iio_usb_data_0.xml_len);

	LOG_INF("Exited IIOD interpreter");

	iio_context_destroy(iio_usb_data_0.ctx);
	iio_usb_data_0.ctx = NULL;
	iiod_cleanup();

	LOG_DBG("USB thread exiting");
}

K_THREAD_DEFINE(iiod_usb, CONFIG_LIBIIO_IIOD_USB_THREAD_STACK_SIZE,
		iiod_usb_thread, usbd_ctx, NULL, NULL,
		CONFIG_LIBIIO_IIOD_USB_THREAD_PRIORITY, 0, 1);
