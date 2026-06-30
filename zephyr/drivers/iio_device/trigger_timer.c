/*
 * Copyright (c) 2026 Analog Devices, Inc.
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>
#include <iio/iio-backend.h>
#include <iio_device.h>
#include <iio-private.h>

LOG_MODULE_REGISTER(iio_device_trigger, CONFIG_LIBIIO_LOG_LEVEL);

#define IIO_DEVICE_SAMPLING_PERIOD_LEN 11 /* Period is 4294967295 which is 10 digits + null terminator */

struct iio_device_trigger_timer_config {
};

struct iio_device_trigger_timer_data {
	struct k_timer timer;
	struct k_work work;
	struct k_mutex lock;
	sys_slist_t subscribers;
	uint32_t period_ms;
	uint32_t refcnt;
	bool inited;
};

/* Dedicated work queue for trigger/readbuf — keeps the system work queue
 * free for RTIO processing, allowing rtio_cqe_consume_block() in readbuf. */
static K_KERNEL_STACK_DEFINE(trigger_wq_stack, 4096);
struct k_work_q trigger_wq;
static bool trigger_wq_started;

static unsigned iio_trigger_timer_count = 0;
static const char *const sampling_period_name = "sampling_period";

static void iio_device_trigger_timer_handler(struct k_timer *t);
static void iio_device_trigger_timer_work_handler(struct k_work *w);

void iio_device_trigger_timer_init(const struct device *dev)
{
	struct iio_device_trigger_timer_data *trig_data = (struct iio_device_trigger_timer_data *)dev->data;

	if (trig_data->inited) {
		return;
	}

	sys_slist_init(&trig_data->subscribers);
	trig_data->refcnt = 0;
	trig_data->inited = true;
	k_mutex_init(&trig_data->lock);
	k_work_init(&trig_data->work, iio_device_trigger_timer_work_handler);
	k_timer_init(&trig_data->timer, iio_device_trigger_timer_handler, NULL);

	if (!trigger_wq_started) {
		k_work_queue_init(&trigger_wq);
		k_work_queue_start(&trigger_wq, trigger_wq_stack,
				   K_KERNEL_STACK_SIZEOF(trigger_wq_stack),
				   K_PRIO_PREEMPT(5), NULL);
		k_thread_name_set(&trigger_wq.thread, "trig_wq");
		trigger_wq_started = true;
	}
}

struct iio_device * iio_device_trigger_timer_create(struct iio_context *ctx, const struct device *dev,
										enum iio_device_trigger_type type, const char *id)
{
	struct iio_device *trig;
	char name[32];

	snprintk(name, sizeof(name), "timer%zu", iio_trigger_timer_count++);

	trig = iio_context_add_device(ctx, id, name, NULL);
	if (!trig) {
		LOG_ERR("Could not add trigger device.");
		return NULL;
	}

	iio_device_set_pdata(trig, (struct iio_device_pdata *) dev);

	if (iio_device_add_attr(trig, sampling_period_name, IIO_ATTR_TYPE_DEVICE)) {
		LOG_ERR("Could not add trigger device %s attribute %s", name, sampling_period_name);
		return NULL;
	}

	return trig;
}

int iio_device_trigger_timer_subscribe(sys_snode_t *node)
{
	struct iio_buffer_pdata *buf = CONTAINER_OF(node, struct iio_buffer_pdata, node);
	const struct iio_device *iio_dev = buf->iio_dev;
	const struct iio_device *trigger = iio_device_get_trigger(iio_dev);
	const struct device *trig_dev = (const struct device *) iio_device_get_pdata(trigger);
	struct iio_device_trigger_timer_data *trig_data = (struct iio_device_trigger_timer_data *)trig_dev->data;
	int ret = 0;

	k_mutex_lock(&trig_data->lock, K_FOREVER);

	sys_snode_t *n;
	SYS_SLIST_FOR_EACH_NODE(&trig_data->subscribers, n) {
		if (n == node) {
			k_mutex_unlock(&trig_data->lock);
			return ret;
		}
	}

	sys_slist_append(&trig_data->subscribers, node);
	trig_data->refcnt++;
	buf->trig = &trig_data->work;

	if (trig_data->refcnt == 1) {
		k_timer_start(&trig_data->timer, K_MSEC(trig_data->period_ms),
				K_MSEC(trig_data->period_ms));
	}

	k_mutex_unlock(&trig_data->lock);
	return ret;
}

void iio_device_trigger_timer_unsubscribe(sys_snode_t *node)
{
	struct iio_buffer_pdata *buf = CONTAINER_OF(node, struct iio_buffer_pdata, node);
	const struct iio_device *iio_dev = buf->iio_dev;
	const struct iio_device *trigger = iio_device_get_trigger(iio_dev);
	const struct device *trig_dev = (const struct device *) iio_device_get_pdata(trigger);
	struct iio_device_trigger_timer_data *trig_data = (struct iio_device_trigger_timer_data *)trig_dev->data;
	
	k_mutex_lock(&trig_data->lock, K_FOREVER);

	bool removed = sys_slist_find_and_remove(&trig_data->subscribers, node);
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
	struct iio_device_trigger_timer_data *trig_data = CONTAINER_OF(t, struct iio_device_trigger_timer_data, timer);
	k_work_submit_to_queue(&trigger_wq, &trig_data->work);
}

static void iio_device_trigger_timer_work_handler(struct k_work *w)
{
	struct iio_device_trigger_timer_data *trig_data = CONTAINER_OF(w, struct iio_device_trigger_timer_data, work);

	k_mutex_lock(&trig_data->lock, K_FOREVER);

	sys_snode_t *bn;
	SYS_SLIST_FOR_EACH_NODE(&trig_data->subscribers, bn) {
		struct iio_buffer_pdata *buf = CONTAINER_OF(bn, struct iio_buffer_pdata, node);
		const struct iio_backend_ops *ops = buf->iio_dev->ctx->ops;

		if (!buf->enabled || !ops->readbuf) {
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

		ssize_t written = ops->readbuf(buf, blk->data, to_write);
		(void)written;

		k_sem_give(&blk->ready_sem);
	}

	k_mutex_unlock(&trig_data->lock);
}

static int iio_device_trigger_sampling_period_read(const struct device *dev,
		char *dst, size_t len)
{
	struct iio_device_trigger_timer_data *trig_data = (struct iio_device_trigger_timer_data *)dev->data;

	if (len < IIO_DEVICE_SAMPLING_PERIOD_LEN) {
		LOG_ERR("Buffer size %u is too small for sampling period value, need %u",
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

	LOG_ERR("Invalid attr");
	return -EINVAL;
}

static int iio_device_trigger_sampling_period_write(const struct device *dev,
	const char *src, size_t len)
{
	struct iio_device_trigger_timer_data *trig_data = (struct iio_device_trigger_timer_data *)dev->data;
	char *end;
	unsigned long val = strtoul(src, &end, 10);

	if (end == src || val > UINT32_MAX) {
		LOG_ERR("Invalid sampling period value");
		return -EINVAL;
	}

	trig_data->period_ms = (uint32_t)val;

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

	LOG_ERR("Invalid attr");
	return -EINVAL;
}

static DEVICE_API(iio_device, iio_device_trigger_timer_driver_api) = {
	.read_attr = iio_device_trigger_timer_read_attr,
	.write_attr = iio_device_trigger_timer_write_attr,
};

static const struct iio_trigger_ops iio_trigger_timer_ops = {
	.create = iio_device_trigger_timer_create,
	.init = iio_device_trigger_timer_init,
	.subscribe = iio_device_trigger_timer_subscribe,
	.unsubscribe = iio_device_trigger_timer_unsubscribe,
};

static int iio_device_trigger_timer_driver_init(const struct device *dev)
{
	return 0;
}

#define DT_DRV_COMPAT iio_trigger_timer

#define IIO_DEVICE_TRIGGER_TIMER_INIT(inst)							\
static struct iio_device_trigger_timer_data iio_device_trigger_timer_data_##inst = {			\
	.inited = false, \
	.period_ms = 1, \
	.refcnt = 0, \
};												\
												\
static const struct iio_device_trigger_config iio_device_trigger_timer_config_##inst = {	\
	.trigger_type = COND_CODE_1(DT_INST_NODE_HAS_PROP(inst, trigger_type), \
    ((enum iio_device_trigger_type)DT_INST_ENUM_IDX(inst, trigger_type)), \
    (IIO_DEVICE_TRIGGER_TIMER)), \
	.trigger_id = DT_INST_PROP_OR(inst, trigger_id, "trigger" STRINGIFY(inst)),					\
	.ops = &iio_trigger_timer_ops, \
};											\
												\
IIO_DEVICE_DT_INST_DEFINE(inst, DT_INST_PROP_OR(inst, io_name, NULL),				\
	iio_device_trigger_timer_driver_init, NULL,							\
	&iio_device_trigger_timer_data_##inst, &iio_device_trigger_timer_config_##inst,		\
	POST_KERNEL, CONFIG_LIBIIO_IIO_DEVICE_TRIGGER_INIT_PRIORITY,				\
	&iio_device_trigger_timer_driver_api);

DT_INST_FOREACH_STATUS_OKAY(IIO_DEVICE_TRIGGER_TIMER_INIT)
