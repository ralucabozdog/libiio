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

#include <zephyr/kernel.h>
#include <zephyr/sys/slist.h>
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

struct zephyr_trigger_state;

struct iio_block_pdata {
	sys_snode_t node;
	struct iio_buffer_pdata *buf_pdata;
	void *data;
	size_t size;
	size_t bytes_used;
	bool cyclic;
	struct k_sem ready_sem;
};

struct iio_buffer_pdata {
	sys_snode_t node;
	const struct device *zephyr_dev;
	const struct iio_device *iio_dev;
	struct iio_channels_mask *mask;
	bool enabled;
	struct k_mutex lock;
	sys_slist_t pending_blocks;
	struct zephyr_trigger_state *trig;
	size_t sample_size;
};

struct zephyr_trigger_state {
	struct k_timer timer;
	struct k_work work;
	struct k_mutex lock;
	sys_slist_t subscribers;
	uint32_t period_ms;
	uint32_t refcnt;
};

static struct zephyr_trigger_state zephyr_trig0_state;
static const struct iio_device *zephyr_trig0_iio_dev;

static void zephyr_trig0_timer_handler(struct k_timer *t);
static void zephyr_trig0_work_handler(struct k_work *w);

static void zephyr_trig0_init_once(void)
{
	static bool inited;

	if (inited) {
		return;
	}
	inited = true;

	k_mutex_init(&zephyr_trig0_state.lock);
	sys_slist_init(&zephyr_trig0_state.subscribers);

	zephyr_trig0_state.period_ms = 100;
	zephyr_trig0_state.refcnt = 0;

	k_work_init(&zephyr_trig0_state.work, zephyr_trig0_work_handler);
	k_timer_init(&zephyr_trig0_state.timer, zephyr_trig0_timer_handler, NULL);
}

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

int zephyr_set_trigger(const struct iio_device *dev,
			const struct iio_device *trigger)
{
	iio_device_set_data((struct iio_device *)dev, (void *)trigger);
	return 0;
}

static const char *const buf_name = "buffer0";

static struct iio_context *
zephyr_create_context(const struct iio_context_params *params, const char *args)
{
	struct iio_context *ctx;
	const struct device *dev;
	struct iio_buffer *buffer;
	struct iio_device *iio_device;
	struct iio_device *trig0;
	const char *name;
	const char *label = NULL;
	char id[32];
	int i = 0;
	int ret;

	zephyr_trig0_init_once();

	const char *description = "Zephyr " BACKEND_VERSION(BACKEND_VERSION_BUILD);

	ctx = iio_context_create_from_backend(params, &iio_external_backend,
			description, KERNEL_VERSION_MAJOR, KERNEL_VERSION_MINOR,
			BACKEND_VERSION_BUILD);
	if (iio_err(ctx)) {
		return iio_err_cast(ctx);
	}

	trig0 = iio_context_add_device(ctx, "trigger0", "timer0", NULL);
	if (!trig0) {
		return iio_ptr(-ENOMEM);
	}
	zephyr_trig0_iio_dev = trig0;

	STRUCT_SECTION_FOREACH(iio_device_info, iio_device_info) {
		dev = iio_device_info->dev;
		name = iio_device_info->name;
		if (!name) {
			name = dev->name;
		}

		snprintk(id, sizeof(id), "iio:device%zu", i);

		iio_device = iio_context_add_device(ctx, id, name, label);

		iio_device_set_pdata(iio_device, (struct iio_device_pdata *) dev);
		iio_device_add_channels(dev, iio_device);

		buffer = iio_device_add_buffer(iio_device, i);
		const char *buffer_name = iio_device_get_buffer_name(dev);
		iio_buffer_add_attr(buffer, buffer_name);

		i++;

		ret = iio_device_set_trigger(iio_device, zephyr_trig0_iio_dev);
		if (ret < 0) {
			return iio_ptr(ret);
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
	sys_slist_init(&pdata->pending_blocks);
	pdata->trig = NULL;

	return pdata;
}

static void zephyr_close_buffer(struct iio_buffer_pdata *pdata)
{
	free(pdata);
}

static int zephyr_trig0_subscribe(struct iio_buffer_pdata *pdata)
{
	k_mutex_lock(&zephyr_trig0_state.lock, K_FOREVER);

	sys_snode_t *n;
	SYS_SLIST_FOR_EACH_NODE(&zephyr_trig0_state.subscribers, n) {
		if (n == &pdata->node) {
			k_mutex_unlock(&zephyr_trig0_state.lock);
			return 0;
		}
	}

	sys_slist_append(&zephyr_trig0_state.subscribers, &pdata->node);
	zephyr_trig0_state.refcnt++;

	if (zephyr_trig0_state.refcnt == 1) {
		k_timer_start(&zephyr_trig0_state.timer, K_MSEC(zephyr_trig0_state.period_ms),
				K_MSEC(zephyr_trig0_state.period_ms));
	}

	k_mutex_unlock(&zephyr_trig0_state.lock);
	return 0;
}

static void zephyr_trig0_unsubscribe(struct iio_buffer_pdata *pdata)
{
	k_mutex_lock(&zephyr_trig0_state.lock, K_FOREVER);

	bool removed = sys_slist_find_and_remove(&zephyr_trig0_state.subscribers, &pdata->node);
	if (removed && zephyr_trig0_state.refcnt > 0) {
		zephyr_trig0_state.refcnt--;
		if (zephyr_trig0_state.refcnt == 0) {
			k_timer_stop(&zephyr_trig0_state.timer);
		}
	}

	k_mutex_unlock(&zephyr_trig0_state.lock);
}

static int zephyr_enable_buffer(struct iio_buffer_pdata *pdata,
				size_t nb_samples, bool enable, bool cyclic)
{
	if (enable) {
		const struct iio_device *trig = (const struct iio_device *) iio_device_get_data(pdata->iio_dev);
		const char *trig_id;

		if (!trig) {
			return -ENODEV;
		}

		if (!iio_device_is_trigger(trig)) {
			return -EINVAL;
		}

		trig_id = iio_device_get_id(trig);
		if (!trig_id || strcmp(trig_id, "trigger0") != 0) {
			return -ENOSYS;
		}

		pdata->enabled = true;
		pdata->trig = &zephyr_trig0_state;

		return zephyr_trig0_subscribe(pdata);
	}

	pdata->enabled = false;
	if (pdata->trig == &zephyr_trig0_state) {
		zephyr_trig0_unsubscribe(pdata);
	}
	pdata->trig = NULL;

	k_mutex_lock(&pdata->lock, K_FOREVER);
	sys_snode_t *n;
	SYS_SLIST_FOR_EACH_NODE(&pdata->pending_blocks, n) {
		struct iio_block_pdata *b = CONTAINER_OF(n, struct iio_block_pdata, node);
		k_sem_give(&b->ready_sem);
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

	block->data = malloc(size);
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
		if (block->data) {
			free(block->data);
		}
		free(block);
	}
}

static int zephyr_enqueue_block(struct iio_block_pdata *pdata, size_t bytes_used, bool cyclic)
{
	struct iio_buffer_pdata *buf = pdata->buf_pdata;

	pdata->bytes_used = bytes_used ? bytes_used : pdata->size;
	pdata->cyclic = cyclic;

	while (k_sem_take(&pdata->ready_sem, K_NO_WAIT) == 0) {
	}

	k_mutex_lock(&buf->lock, K_FOREVER);
	sys_slist_append(&buf->pending_blocks, &pdata->node);
	k_mutex_unlock(&buf->lock);

	return 0;
}

static int zephyr_dequeue_block(struct iio_block_pdata *block, bool nonblock)
{
	struct iio_buffer_pdata *buf_pdata = block->buf_pdata;

	if (nonblock) {
		if (k_sem_take(&block->ready_sem, K_NO_WAIT) != 0) {
			return -EBUSY;
		}
	} else {
		k_sem_take(&block->ready_sem, K_FOREVER);
	}

	if (!buf_pdata->enabled) {
		return -EBADF;
	}
	
	return 0;
}

static ssize_t zephyr_fill_samples(struct iio_buffer_pdata *pdata, void *dst, size_t len)
{
	return zephyr_readbuf(pdata, dst, len);
}

static void zephyr_trig0_timer_handler(struct k_timer *t)
{
	ARG_UNUSED(t);
	k_work_submit(&zephyr_trig0_state.work);
}

static void zephyr_trig0_work_handler(struct k_work *w)
{
	ARG_UNUSED(w);

	k_mutex_lock(&zephyr_trig0_state.lock, K_FOREVER);

	sys_snode_t *bn;
	SYS_SLIST_FOR_EACH_NODE(&zephyr_trig0_state.subscribers, bn) {
		struct iio_buffer_pdata *buf = CONTAINER_OF(bn, struct iio_buffer_pdata, node);

		if (!buf->enabled) {
			continue;
		}

		k_mutex_lock(&buf->lock, K_FOREVER);

		sys_snode_t *n = sys_slist_get(&buf->pending_blocks);
		if (!n) {
			k_mutex_unlock(&buf->lock);
			continue;
		}

		struct iio_block_pdata *blk = CONTAINER_OF(n, struct iio_block_pdata, node);
		size_t to_write = MIN(blk->bytes_used, blk->size);

		k_mutex_unlock(&buf->lock);

		ssize_t written = zephyr_fill_samples(buf, blk->data, to_write);
		(void)written;

		k_sem_give(&blk->ready_sem);
	}

	k_mutex_unlock(&zephyr_trig0_state.lock);
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
