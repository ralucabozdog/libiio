/*
 * Copyright (c) 2026 Analog Devices, Inc.
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>
#include <iio/iio-backend.h>
#include <iio_device.h>
#include <iio_trigger.h>
#include <iio-private.h>

LOG_MODULE_REGISTER(iio_device_trigger_timer, CONFIG_LIBIIO_LOG_LEVEL);

/* Period is 4294967295 which is 10 digits + null terminator */
#define IIO_DEVICE_SAMPLING_PERIOD_LEN 11

struct iio_device_trigger_timer_data {
	struct k_timer timer;
	struct k_work work;
	struct k_mutex lock;
	struct iio_trigger_list subscribers;
	uint32_t period_ms;
	uint32_t refcnt;
	bool inited;
};

static const char *const sampling_period_name = "sampling_period";

static void iio_device_trigger_timer_handler(struct k_timer *t);
static void iio_device_trigger_timer_work_handler(struct k_work *w);

static void iio_device_trigger_timer_init(const struct device *dev)
{
	struct iio_device_trigger_timer_data *trig_data =
		(struct iio_device_trigger_timer_data *)dev->data;

	if (trig_data->inited) {
		return;
	}

	iio_trigger_list_init(&trig_data->subscribers);
	trig_data->refcnt = 0;
	trig_data->inited = true;
	k_mutex_init(&trig_data->lock);
	k_work_init(&trig_data->work, iio_device_trigger_timer_work_handler);
	k_timer_init(&trig_data->timer, iio_device_trigger_timer_handler, NULL);
}

static struct iio_device * iio_device_trigger_timer_create(struct iio_context *ctx,
							const struct device *dev)
{
	struct iio_device *trig;
	const struct iio_device_trigger_config *trig_config = 
			(const struct iio_device_trigger_config *)dev->config;

	trig = iio_context_add_device(ctx, trig_config->trigger_id, trig_config->name, NULL);
	if (!trig) {
		LOG_ERR("Could not add trigger device.");
		return NULL;
	}

	iio_device_set_pdata(trig, (struct iio_device_pdata *) dev);

	if (iio_device_add_attr(trig, sampling_period_name, IIO_ATTR_TYPE_DEVICE)) {
		LOG_ERR("Could not add trigger device %s attribute %s",
				trig_config->name, sampling_period_name);
		return NULL;
	}

	return trig;
}

static int iio_device_trigger_timer_subscribe(struct iio_trigger_node *node)
{
	struct iio_buffer_pdata *buf = CONTAINER_OF(node, struct iio_buffer_pdata, node);
	const struct iio_device *iio_dev = buf->iio_dev;
	const struct iio_device *trigger = iio_device_get_trigger(iio_dev);
	const struct device *trig_dev = (const struct device *) iio_device_get_pdata(trigger);
	struct iio_device_trigger_timer_data *trig_data =
		(struct iio_device_trigger_timer_data *)trig_dev->data;
	int ret = 0;

	k_mutex_lock(&trig_data->lock, K_FOREVER);

	if (iio_trigger_node_find(&trig_data->subscribers, node)) {
		k_mutex_unlock(&trig_data->lock);
		return ret;
	}

	iio_trigger_node_add(&trig_data->subscribers, node);
	trig_data->refcnt++;

	if (trig_data->refcnt == 1) {
		k_timer_start(&trig_data->timer, K_MSEC(trig_data->period_ms),
				K_MSEC(trig_data->period_ms));
	}

	k_mutex_unlock(&trig_data->lock);
	return ret;
}

static void iio_device_trigger_timer_unsubscribe(struct iio_trigger_node *node)
{
	struct iio_buffer_pdata *buf = CONTAINER_OF(node, struct iio_buffer_pdata, node);
	const struct iio_device *iio_dev = buf->iio_dev;
	const struct iio_device *trigger = iio_device_get_trigger(iio_dev);
	const struct device *trig_dev = (const struct device *) iio_device_get_pdata(trigger);
	struct iio_device_trigger_timer_data *trig_data =
		(struct iio_device_trigger_timer_data *)trig_dev->data;
	
	k_mutex_lock(&trig_data->lock, K_FOREVER);

	bool removed = iio_trigger_node_remove(&trig_data->subscribers, node);
	if (removed && trig_data->refcnt > 0) {
		trig_data->refcnt--;
		if (trig_data->refcnt == 0) {
			k_timer_stop(&trig_data->timer);
		}
	}

	k_mutex_unlock(&trig_data->lock);
}

static void iio_device_trigger_timer_handler(struct k_timer *t)
{
	struct iio_device_trigger_timer_data *trig_data =
		CONTAINER_OF(t, struct iio_device_trigger_timer_data, timer);
	k_work_submit(&trig_data->work);
}

static int iio_device_trigger_timer_work_handler_cb(struct iio_trigger_node *node, void *user_data)
{
	struct iio_buffer_pdata *buf = CONTAINER_OF(node, struct iio_buffer_pdata, node);
	const struct iio_backend_ops *ops = buf->iio_dev->ctx->ops;
	(void)user_data;

	if (!buf->enabled || !ops->readbuf) {
		return 0;
	}

	k_mutex_lock(&buf->lock, K_FOREVER);

	struct iio_trigger_node *n = iio_trigger_node_get(&buf->pending_blocks);
	if (!n) {
		k_mutex_unlock(&buf->lock);
		return 0;
	}

	struct iio_block_pdata *blk = CONTAINER_OF(n, struct iio_block_pdata, node);
	size_t to_write = MIN(blk->bytes_used, blk->size);

	k_mutex_unlock(&buf->lock);

	(void)ops->readbuf(buf, blk->data, to_write);

	k_sem_give(&blk->ready_sem);

	return 0;
}

static void iio_device_trigger_timer_work_handler(struct k_work *w)
{
	struct iio_device_trigger_timer_data *trig_data =
		CONTAINER_OF(w, struct iio_device_trigger_timer_data, work);

	k_mutex_lock(&trig_data->lock, K_FOREVER);

	iio_trigger_node_foreach(&trig_data->subscribers,
		iio_device_trigger_timer_work_handler_cb, NULL);

	k_mutex_unlock(&trig_data->lock);
}

static int iio_device_trigger_sampling_period_read(const struct device *dev,
		char *dst, size_t len)
{
	struct iio_device_trigger_timer_data *trig_data =
		(struct iio_device_trigger_timer_data *)dev->data;

	if (len < IIO_DEVICE_SAMPLING_PERIOD_LEN) {
		LOG_ERR("Buffer size %zu is too small for sampling period value, need %u",
			len, IIO_DEVICE_SAMPLING_PERIOD_LEN);
		return -ENOMEM;
	}

	return snprintk(dst, len, "%u", trig_data->period_ms) + 1;
}

static int iio_device_trigger_timer_read_attr(const struct device *dev,
		const struct iio_device *iio_device, const struct iio_attr *attr,
		char *dst, size_t len)
{

	switch (attr->type) {
	case IIO_ATTR_TYPE_DEVICE:
		if (!strcmp(attr->name, sampling_period_name)) {
			return iio_device_trigger_sampling_period_read(dev, dst, len);
		}
		break;

	default:
		break;
	}

	LOG_ERR("Invalid attr type: %d, name: %s", attr->type, attr->name);
	return -EINVAL;
}

static int iio_device_trigger_sampling_period_write(const struct device *dev,
	const char *src, size_t len)
{
	struct iio_device_trigger_timer_data *trig_data =
		(struct iio_device_trigger_timer_data *)dev->data;
	char tmp[16];
 	size_t n = MIN(len, sizeof(tmp) - 1);
	char *end;
	unsigned long val;

	strncpy(tmp, src, n);
 	tmp[n] = '\0';
 	while (n > 0 && (tmp[n - 1] == '\n' || tmp[n - 1] == '\r' || tmp[n - 1] == ' '
				|| tmp[n - 1] == '\t')) {
 		tmp[--n] = '\0';
 	}
 	val = strtoul(tmp, &end, 10);

 	if (end == tmp || *end != '\0' || val == 0 || val > UINT32_MAX) {
		LOG_ERR("Invalid sampling period value");
		return -EINVAL;
	}

	k_mutex_lock(&trig_data->lock, K_FOREVER);

 	trig_data->period_ms = (uint32_t)val;
 	
	if (trig_data->refcnt > 0) {
 		k_timer_stop(&trig_data->timer);
 		k_timer_start(&trig_data->timer, K_MSEC(trig_data->period_ms),
 			     K_MSEC(trig_data->period_ms));
 	}

	k_mutex_unlock(&trig_data->lock);

	return len;
}

static int iio_device_trigger_timer_write_attr(const struct device *dev,
		const struct iio_device *iio_device, const struct iio_attr *attr,
		const char *src, size_t len)
{
	switch (attr->type) {
	case IIO_ATTR_TYPE_DEVICE:
		if (!strcmp(attr->name, sampling_period_name)) {
			return iio_device_trigger_sampling_period_write(dev, src, len);
		}		
		break;

	default:
		break;
	}

	LOG_ERR("Invalid attr type: %d, name: %s", attr->type, attr->name);
	return -EINVAL;
}

static DEVICE_API(iio_trigger, iio_device_trigger_timer_driver_api) = {
	.attr_api.read_attr = iio_device_trigger_timer_read_attr,
	.attr_api.write_attr = iio_device_trigger_timer_write_attr,
	.create = iio_device_trigger_timer_create,
	.init = iio_device_trigger_timer_init,
	.subscribe = iio_device_trigger_timer_subscribe,
	.unsubscribe = iio_device_trigger_timer_unsubscribe,
};

#define DT_DRV_COMPAT iio_trigger_timer

#define IIO_DEVICE_TRIGGER_TIMER_INIT(inst)							\
static struct iio_device_trigger_timer_data iio_device_trigger_timer_data_##inst = {		\
	.inited = false, 									\
	.period_ms = 100, 									\
	.refcnt = 0, 										\
};												\
												\
static const struct iio_device_trigger_config iio_device_trigger_timer_config_##inst = {	\
	.trigger_id = DT_INST_PROP_OR(inst, trigger_id, "trigger" STRINGIFY(inst)),		\
	.name = DT_INST_PROP_OR(inst, io_name, ""),						\
};												\
												\
IIO_DEVICE_DT_INST_DEFINE(inst,									\
	NULL, NULL,	&iio_device_trigger_timer_data_##inst,					\
	&iio_device_trigger_timer_config_##inst,						\
	POST_KERNEL, CONFIG_LIBIIO_IIO_DEVICE_TRIGGER_TIMER_INIT_PRIORITY,			\
	&iio_device_trigger_timer_driver_api);

DT_INST_FOREACH_STATUS_OKAY(IIO_DEVICE_TRIGGER_TIMER_INIT)
