/*
 * Copyright (c) 2025 Analog Devices, Inc.
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/sys/iterable_sections.h>
#include <zephyr/version.h>
#include <iio/iio-backend.h>
#include <iio-private.h>
#include <errno.h>
#include <string.h>
#include <iio_device.h>
#include <iio_trigger.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <stdlib.h>

#if defined(__DATE__) && defined(__TIME__)
#define BACKEND_VERSION(_ver) _ver " " __DATE__ " " __TIME__
#else
#define BACKEND_VERSION(_ver) _ver
#endif

#if defined(BUILD_VERSION) && !IS_EMPTY(BUILD_VERSION)
#define BACKEND_VERSION_BUILD STRINGIFY(BUILD_VERSION)
#else
#define BACKEND_VERSION_BUILD KERNEL_VERSION_STRING
#endif

static ssize_t
zephyr_read_attr(const struct iio_attr *attr, char *dst, size_t len)
{
	const struct iio_device *iio_device = iio_attr_get_device(attr);
	const struct device *dev = (const struct device *) iio_device_get_pdata(iio_device);

	return iio_device_read_attr(dev, iio_device, attr, dst, len);
}

static ssize_t
zephyr_write_attr(const struct iio_attr *attr, const char *src, size_t len)
{
	const struct iio_device *iio_device = iio_attr_get_device(attr);
	const struct device *dev = (const struct device *) iio_device_get_pdata(iio_device);

	return iio_device_write_attr(dev, iio_device, attr, src, len);
}

static const struct iio_device *
zephyr_get_trigger(const struct iio_device *dev)
{
	const struct iio_device * trigger;

	if (iio_device_is_trigger(dev)) {
		return iio_ptr(-ENOENT);
	} else {
		trigger = (const struct iio_device *) iio_device_get_data(dev);
		
		if (!trigger) {
			return iio_ptr(-ENODEV);
		} else {
			return trigger;
		}
	}
}

static int zephyr_set_trigger(const struct iio_device *dev,
			const struct iio_device *trigger)
{
	iio_device_set_data((struct iio_device *)dev, (void *)trigger);
	return 0;
}

static struct iio_context *
zephyr_create_context(const struct iio_context_params *params, const char *args)
{
	struct iio_context *ctx;
	const struct device *dev;
	struct iio_buffer *buffer;
	struct iio_device *iio_device;
	const char *name;
	const char *label = NULL;
	char id[32];
	int i = 0;
	int ret;

	const char *description = "Zephyr " BACKEND_VERSION(BACKEND_VERSION_BUILD);

	ctx = iio_context_create_from_backend(params, &iio_external_backend,
			description, KERNEL_VERSION_MAJOR, KERNEL_VERSION_MINOR,
			KERNEL_PATCHLEVEL, BACKEND_VERSION_BUILD);
	if (iio_err(ctx)) {
		return iio_err_cast(ctx);
	}

	STRUCT_SECTION_FOREACH(iio_device_info, iio_device_info) {
		dev = iio_device_info->dev;
		name = iio_device_info->name;
		if (!name) {
			name = dev->name;
		}

		if (strncmp(name, "trigger", 7) == 0) {
			/* This is a trigger device — skip */
			continue;
		}

		snprintk(id, sizeof(id), "iio:device%d", i);

		iio_device = iio_context_add_device(ctx, id, name, label);

		iio_device_set_pdata(iio_device, (struct iio_device_pdata *) dev);
		iio_device_add_channels(dev, iio_device);

		const char *buffer_name = iio_device_get_buffer_name(dev);
		if (buffer_name) {
			buffer = iio_device_add_buffer(iio_device, 0);

			if (!buffer) {
				return iio_ptr(-ENOMEM);
			}

			iio_buffer_add_attr(buffer, buffer_name);

			unsigned int nb_ch = iio_device_get_channels_count(iio_device);

			for (unsigned int ci = 0; ci < nb_ch; ci++) {
				const struct iio_channel *chn =
					iio_device_get_channel(iio_device, ci);

				if (iio_channel_is_scan_element(chn)) {
					iio_buffer_add_scan_element(buffer, chn, NULL);
				}
			}

			i++;

			ret = iio_device_add_trigger(ctx, iio_device);
			if ((ret < 0) && (ret != -ENODEV)) {
				return iio_ptr(ret);
			}
		}
	}

	return ctx;
}

static struct iio_buffer_pdata *
zephyr_open_buffer(const struct iio_device *dev, unsigned int idx,
			struct iio_channels_mask *mask)
{
	struct iio_buffer_pdata *pdata;
	const struct device *zephyr_dev;

	pdata = zalloc(sizeof(*pdata));
	if (!pdata) {
		return iio_ptr(-ENOMEM);
	}

	zephyr_dev = (const struct device *)iio_device_get_pdata(dev);

	pdata->enabled = false;
	pdata->zephyr_dev = zephyr_dev;
	pdata->iio_dev = dev;
	pdata->mask = mask;

	k_mutex_init(&pdata->lock);
	iio_trigger_list_init(&pdata->pending_blocks);

	return pdata;
}

static void zephyr_close_buffer(struct iio_buffer_pdata *pdata)
{
	free(pdata);
}

static int zephyr_release_block(struct iio_trigger_node *node, void *user_data)
{
	struct iio_block_pdata *b = CONTAINER_OF(node, struct iio_block_pdata, node);

	k_sem_give(&b->ready_sem);
	(void)user_data;

	return 0;
}

static int zephyr_enable_buffer(struct iio_buffer_pdata *pdata,
			size_t nb_samples, bool enable, bool cyclic)
{
	const struct iio_device *trig = (const struct iio_device *) iio_device_get_data(pdata->iio_dev);
	const struct device *trig_dev = NULL;

	if (cyclic) {
		return -ENOSYS;   /* not implemented */
	}

	if (enable) {
		if (!trig) {
			pdata->enabled = true;

			return 0;
		}
		else {
			if (!iio_device_is_trigger(trig)) {
				return -EINVAL;
			}

			trig_dev = (const struct device *) iio_device_get_pdata(trig);

			int ret = iio_trigger_subscribe(trig_dev, &pdata->node);

			if (ret == 0) {
				pdata->enabled = true;
			}

			return ret;
		}
	}

	pdata->enabled = false;

	if (trig) {
		trig_dev = (const struct device *) iio_device_get_pdata(trig);
		iio_trigger_unsubscribe(trig_dev, &pdata->node);
	}

	k_mutex_lock(&pdata->lock, K_FOREVER);

	struct iio_trigger_node *n;
	while ((n = iio_trigger_node_get(&pdata->pending_blocks)) != NULL) {
		zephyr_release_block(n, NULL);
	}

	k_mutex_unlock(&pdata->lock);

	return 0;
}

static void zephyr_cancel_buffer(struct iio_buffer_pdata *pdata)
{
	(void)zephyr_enable_buffer(pdata, 0, false, false);
}

static int iio_device_read_channel_raw(const struct device *zephyr_dev,
											const struct iio_channel *chn,
											const struct iio_device *iio_dev,
											char *buf, size_t len)
{
	const struct iio_attr *raw_attr = NULL;
	unsigned int nb_attrs = iio_channel_get_attrs_count(chn);

	for (unsigned int i = 0; i < nb_attrs; i++) {
		const struct iio_attr *attr = iio_channel_get_attr(chn, i);
		if (!strcmp(iio_attr_get_name(attr), "raw")) {
			raw_attr = attr;
			break;
		}
	}

	if (!raw_attr)
		return -ENOENT;

	return iio_device_read_attr(zephyr_dev, iio_dev, raw_attr, buf, len);
}

static ssize_t zephyr_readbuf(struct iio_buffer_pdata *pdata,
					void *dst, size_t len)
{
	const struct device *zephyr_dev = pdata->zephyr_dev;
	const struct iio_device *iio_dev = pdata->iio_dev;
	uint8_t *out = (uint8_t *)dst;
	unsigned int nb_channels = iio_device_get_channels_count(iio_dev);
	char raw_buf[32];
	int ret;

	unsigned int enabled_channels = 0;
	for (unsigned int ch_idx = 0; ch_idx < nb_channels; ch_idx++) {
		const struct iio_channel *chn = iio_device_get_channel(iio_dev, ch_idx);
		if (iio_channel_is_scan_element(chn) &&
			iio_channel_is_enabled(chn, pdata->mask)) {
			enabled_channels++;
		}
	}

	if (enabled_channels == 0)
		return -EINVAL;

	const struct iio_channel *first_chn = NULL;
	for (unsigned int ch_idx = 0; ch_idx < nb_channels; ch_idx++) {
		const struct iio_channel *chn = iio_device_get_channel(iio_dev, ch_idx);
		if (iio_channel_is_scan_element(chn) &&
			iio_channel_is_enabled(chn, pdata->mask)) {
			first_chn = chn;
			break;
		}
	}

	if (!first_chn)
		return -EINVAL;

	const struct iio_data_format *fmt = iio_channel_get_data_format(first_chn);

	size_t bytes_per_channel = DIV_ROUND_UP(fmt->length, BITS_PER_BYTE);
	size_t bytes_per_sample = enabled_channels * bytes_per_channel;
	size_t num_samples = len / bytes_per_sample;
	size_t samples_written = 0;

	for (size_t sample_idx = 0; sample_idx < num_samples; sample_idx++) {
		for (unsigned int ch_idx = 0; ch_idx < nb_channels; ch_idx++) {
			const struct iio_channel *chn = iio_device_get_channel(iio_dev, ch_idx);

			if (!iio_channel_is_scan_element(chn))
				continue;
			if (!iio_channel_is_enabled(chn, pdata->mask))
				continue;

			ret = iio_device_read_channel_raw(zephyr_dev, chn, iio_dev,
										raw_buf, sizeof(raw_buf));

			int32_t raw_value;
			if (ret < 0) {
				raw_value = 0;
			} else {
				if (fmt->is_signed) {
					raw_value = (int32_t)strtol(raw_buf, NULL, 10);
				} else {
					raw_value = (int32_t)strtoul(raw_buf, NULL, 10);
				}
			}

			raw_value <<= fmt->shift;

			if (fmt->is_be) {
				for (int i = bytes_per_channel - 1; i >= 0; i--) {
					*out++ = (uint8_t)((raw_value >> (i * 8)) & 0xFF);
				}
			} else {
				for (size_t i = 0; i < bytes_per_channel; i++) {
					*out++ = (uint8_t)((raw_value >> (i * 8)) & 0xFF);
				}
			}
		}
		samples_written++;
	}

	return (ssize_t)(samples_written * bytes_per_sample);
}

static struct iio_block_pdata *
zephyr_create_block(struct iio_buffer_pdata *pdata, size_t size, void **data)
{
	struct iio_block_pdata *block;

	block = zalloc(sizeof(*block));
	if (!block) {
		return iio_ptr(-ENOMEM);
	}

	block->data = zalloc(size);
	if (!block->data) {
		free(block);
		return iio_ptr(-ENOMEM);
	}

	block->buf_pdata = pdata;
	block->size = size;
	block->bytes_used = 0;
	block->cyclic = false;
	k_sem_init(&block->ready_sem, 0, 1);

	*data = block->data;
	return block;
}

static void zephyr_free_block(struct iio_block_pdata *block)
{
	if (block) {
		/* Ensure node is not linked before freeing */
		iio_trigger_node_remove(&block->buf_pdata->pending_blocks, &block->node);
		free(block->data);
		free(block);
	}
}

static int zephyr_enqueue_block(struct iio_block_pdata *pdata, size_t bytes_used, bool cyclic)
{
	struct iio_buffer_pdata *buf = pdata->buf_pdata;
	const struct iio_device *iio_dev = buf->iio_dev;
	const struct iio_device *trigger = iio_device_get_trigger(iio_dev);
	const struct device *trig_dev;

	if (iio_trigger_node_find(&buf->pending_blocks, &pdata->node)) {
		return -EPERM;
	}

	if (!iio_err(trigger)) {
		trig_dev = (const struct device *) iio_device_get_pdata(trigger);

		while (k_sem_take(&pdata->ready_sem, K_NO_WAIT) == 0) {
		}

		pdata->bytes_used = bytes_used ? bytes_used : pdata->size;
		pdata->cyclic = cyclic;

		k_mutex_lock(&buf->lock, K_FOREVER);
		iio_trigger_node_add(&buf->pending_blocks, &pdata->node);
		k_mutex_unlock(&buf->lock);

		iio_trigger_submit_work(trig_dev, &buf->node);
	}

	return 0;
}

static int zephyr_dequeue_block(struct iio_block_pdata *block, bool nonblock)
{
	struct iio_buffer_pdata *buf = block->buf_pdata;
	const struct iio_device *iio_dev = buf->iio_dev;
	const struct iio_device *trigger = iio_device_get_trigger(iio_dev);
	int ret;

	if (!block->buf_pdata->enabled) {
		return -EBADF;
	}

	if (!iio_err(trigger)) {
		k_timeout_t timeout = nonblock ? K_NO_WAIT : K_MSEC(5000);
		ret = k_sem_take(&block->ready_sem, timeout);

		if (ret == -EAGAIN) {
			return nonblock ? -EBUSY : -ETIMEDOUT;
		}
	}
	else
	{
		 ret = zephyr_readbuf(buf, block->data, block->size);
		if (ret < 0)
			return (int)ret;
	}

	return 0;
}

static const struct iio_backend_ops zephyr_ops = {
	.create = zephyr_create_context,
	.read_attr = zephyr_read_attr,
	.write_attr = zephyr_write_attr,
	.get_trigger = zephyr_get_trigger,
	.set_trigger = zephyr_set_trigger,
	.open_buffer = zephyr_open_buffer,
	.close_buffer = zephyr_close_buffer,
	.enable_buffer = zephyr_enable_buffer,
	.cancel_buffer = zephyr_cancel_buffer,
	.readbuf = zephyr_readbuf,
	//.writebuf = zephyr_writebuf,
	.create_block = zephyr_create_block,
	.free_block = zephyr_free_block,
	.enqueue_block = zephyr_enqueue_block,
	.dequeue_block = zephyr_dequeue_block,
};

const struct iio_backend iio_external_backend = {
	.name = "zephyr",
	.api_version = IIO_BACKEND_API_V1,
	.default_timeout_ms = 5000,
	.uri_prefix = "zephyr:",
	.ops = &zephyr_ops,
};
