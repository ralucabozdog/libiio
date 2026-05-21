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
#define IIO_DEVICE_COMMON_TRIGGER_TYPE_LEN 17
#define IIO_DEVICE_COMMON_TRIGGER_CHANNEL_LEN 17

struct iio_device_trigger_config {
	struct k_timer timer;
	struct k_work work;
	struct k_mutex lock;
	sys_slist_t subscribers;
	uint32_t period_ms;
	uint32_t refcnt;
	enum iio_device_trigger_type type;
	bool inited;
	enum sensor_trigger_type common_trigger_type;
	enum sensor_channel common_trigger_channel;
	const struct device *common_zephyr_dev;
	sys_snode_t common_node;
};

static sys_slist_t active_common_triggers = SYS_SLIST_STATIC_INIT(&active_common_triggers);
static unsigned iio_trigger_timer_count = 0;
static unsigned iio_trigger_zephyr_common_count = 0;
static unsigned iio_trigger_zephyr_stream_count = 0;
static const char *const sampling_period_name = "sampling_period";
static const char *const common_trigger_type_name = "common_trigger_type";
static const char *const common_trigger_channel_name = "common_trigger_channel";

void iio_device_trigger_set_period(const struct iio_device *iio_device, uint32_t period_ms)
{
	struct iio_device_trigger_config *config = (struct iio_device_trigger_config *)iio_device_get_pdata(iio_device);
	config->period_ms = period_ms;
}

static void iio_device_trigger_timer_handler(struct k_timer *t);
static void iio_device_trigger_work_handler(struct k_work *w);
static void iio_device_trigger_common_handler(const struct device *dev,
			    const struct sensor_trigger *trig);

static int iio_device_trigger_set_common_trigger(const struct device *zephyr_dev, 
	struct iio_device_trigger_config *trig_config)
{
	int ret;
	const struct sensor_trigger trig = {
		.type = trig_config->common_trigger_type,
		.chan = trig_config->common_trigger_channel,
	};

	ret = sensor_trigger_set(zephyr_dev, &trig, iio_device_trigger_common_handler);
	if (ret != 0) {
		LOG_ERR("Failed to set common trigger for device %s: %d", zephyr_dev->name, ret);
		return ret;
	}

	return 0;
}

void iio_device_trigger_init(const struct iio_device *iio_device)
{
	struct iio_device_trigger_config *config = (struct iio_device_trigger_config *)iio_device_get_pdata(iio_device);

	if (config->inited) {
			return;
	}

	switch (config->type) {
	case IIO_DEVICE_TRIGGER_TIMER:
		sys_slist_init(&config->subscribers);
		break;

	case IIO_DEVICE_TRIGGER_ZEPHYR_COMMON:
		config->common_trigger_type = SENSOR_TRIG_DATA_READY;
		config->common_trigger_channel = SENSOR_CHAN_ALL;
		break;
	case IIO_DEVICE_TRIGGER_ZEPHYR_STREAM:
	default:
		//not supported yet
		break;
	}

	config->refcnt = 0;
	config->inited = true;

	k_mutex_init(&config->lock);

	k_work_init(&config->work, iio_device_trigger_work_handler);
	k_timer_init(&config->timer, iio_device_trigger_timer_handler, NULL);
}

struct iio_device * iio_device_trigger_create(struct iio_context *ctx, enum iio_device_trigger_type type, const char *id)
{
	struct iio_device *trig;
	struct iio_device_trigger_config *trigg_config;
	char name[32];

	switch (type) {
	case IIO_DEVICE_TRIGGER_TIMER:
		snprintk(name, sizeof(name), "timer%zu", iio_trigger_timer_count++);
		break;
	case IIO_DEVICE_TRIGGER_ZEPHYR_COMMON:
		snprintk(name, sizeof(name), "common_trigger%zu", iio_trigger_zephyr_common_count++);
		break;
	case IIO_DEVICE_TRIGGER_ZEPHYR_STREAM:
		snprintk(name, sizeof(name), "stream_trigger%zu", iio_trigger_zephyr_stream_count++);
		break;
	}

	trig = iio_context_add_device(ctx, id, name, NULL);
	if (!trig) {
		LOG_ERR("Could not add trigger device.");
		return NULL;
	}

	trigg_config = zalloc(sizeof(*trigg_config));
	if (!trigg_config) {
		LOG_ERR("Could not allocate memory for trigger config.");
		return NULL;
	}

	trigg_config->type = type;
	trigg_config->inited = false;

	iio_device_set_pdata((struct iio_device *)trig, (struct iio_device_pdata *)trigg_config);

	if (type == IIO_DEVICE_TRIGGER_TIMER) {
		if (iio_device_add_attr(trig, sampling_period_name, IIO_ATTR_TYPE_DEVICE)) {
			LOG_ERR("Could not add trigger device %s attribute %s", name, sampling_period_name);
			return NULL;
		}
	} else if (type == IIO_DEVICE_TRIGGER_ZEPHYR_COMMON) {
		if (iio_device_add_attr(trig, common_trigger_type_name, IIO_ATTR_TYPE_DEVICE)) {
			LOG_ERR("Could not add trigger device %s attribute %s", name, common_trigger_type_name);
			return NULL;
		}

		if (iio_device_add_attr(trig, common_trigger_channel_name, IIO_ATTR_TYPE_DEVICE)) {
			LOG_ERR("Could not add trigger device %s attribute %s", name, common_trigger_channel_name);
			return NULL;
		}
	}

	return trig;
}

int iio_device_trigger_subscribe(sys_snode_t *node)
{
	struct iio_buffer_pdata *buf = CONTAINER_OF(node, struct iio_buffer_pdata, node);
	const struct iio_device *iio_dev = buf->iio_dev;
	const struct device *iio_device = (const struct device *)iio_device_get_pdata(iio_dev);
	const struct iio_device *trigger = iio_device_get_trigger(iio_dev);
	struct iio_device_trigger_config *trig_config = (struct iio_device_trigger_config *)iio_device_get_pdata(trigger);
	int ret = 0;

	k_mutex_lock(&trig_config->lock, K_FOREVER);

	sys_snode_t *n;
	SYS_SLIST_FOR_EACH_NODE(&trig_config->subscribers, n) {
		if (n == node) {
			k_mutex_unlock(&trig_config->lock);
			return ret;
		}
	}

	sys_slist_append(&trig_config->subscribers, node);
	trig_config->refcnt++;

	switch (trig_config->type) {
	case IIO_DEVICE_TRIGGER_TIMER:
		
		if (trig_config->refcnt == 1) {
			k_timer_start(&trig_config->timer, K_MSEC(trig_config->period_ms),
					K_MSEC(trig_config->period_ms));
		}
		break;
	case IIO_DEVICE_TRIGGER_ZEPHYR_COMMON:
		if (trig_config->refcnt == 1) {
			trig_config->common_zephyr_dev = iio_device_get_zephyr_dev(iio_device);
			sys_slist_append(&active_common_triggers, &trig_config->common_node);
			ret = iio_device_trigger_set_common_trigger(trig_config->common_zephyr_dev, trig_config);
			if (ret < 0) {
				LOG_ERR("Could not set common trigger for device %s", trig_config->common_zephyr_dev->name);
				sys_slist_find_and_remove(&active_common_triggers, &trig_config->common_node);
				sys_slist_find_and_remove(&trig_config->subscribers, node);
				trig_config->refcnt--;
			}
		} else {
			LOG_ERR("IIO device %s is already subscribed to this common trigger, refcnt: %u",
				iio_dev->name, trig_config->refcnt);
		}
		break;
	
	case IIO_DEVICE_TRIGGER_ZEPHYR_STREAM:
	default:
		//not supported yet
		break;
	}

	k_mutex_unlock(&trig_config->lock);
	return ret;
}

void iio_device_trigger_unsubscribe(sys_snode_t *node)
{
	struct iio_buffer_pdata *buf = CONTAINER_OF(node, struct iio_buffer_pdata, node);
	const struct iio_device *iio_dev = buf->iio_dev;
	const struct iio_device *trigger = iio_device_get_trigger(iio_dev);
	struct iio_device_trigger_config *trig_config = (struct iio_device_trigger_config *)iio_device_get_pdata(trigger);

	k_mutex_lock(&trig_config->lock, K_FOREVER);

	bool removed = sys_slist_find_and_remove(&trig_config->subscribers, node);
	if (removed && trig_config->refcnt > 0) {
		trig_config->refcnt--;
		if (trig_config->refcnt == 0) {
			if (trig_config->type == IIO_DEVICE_TRIGGER_TIMER) {
				k_timer_stop(&trig_config->timer);
			} else if (trig_config->type == IIO_DEVICE_TRIGGER_ZEPHYR_COMMON) {
				sys_slist_find_and_remove(&active_common_triggers, &trig_config->common_node);
				sensor_trigger_set(trig_config->common_zephyr_dev, NULL, NULL);
			} else if (trig_config->type == IIO_DEVICE_TRIGGER_ZEPHYR_STREAM) {
				//not supported yet
			}
		}
	}

	k_mutex_unlock(&trig_config->lock);
}

static void iio_device_trigger_common_handler(const struct device *dev,
			    const struct sensor_trigger *trig)
{
	sys_snode_t *n;

	SYS_SLIST_FOR_EACH_NODE(&active_common_triggers, n) {
		struct iio_device_trigger_config *trig_config =
			CONTAINER_OF(n, struct iio_device_trigger_config, common_node);

		if (trig_config->common_zephyr_dev == dev) {
			k_work_submit(&trig_config->work);
			return;
		}
	}
}

static void iio_device_trigger_timer_handler(struct k_timer *t)
{
	struct iio_device_trigger_config *trig_config = CONTAINER_OF(t, struct iio_device_trigger_config, timer);
	k_work_submit(&trig_config->work);
}

static void iio_device_trigger_work_handler(struct k_work *w)
{
	struct iio_device_trigger_config *trig_config = CONTAINER_OF(w, struct iio_device_trigger_config, work);

	k_mutex_lock(&trig_config->lock, K_FOREVER);

	sys_snode_t *bn;
	SYS_SLIST_FOR_EACH_NODE(&trig_config->subscribers, bn) {
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

	k_mutex_unlock(&trig_config->lock);
}

static int iio_device_trigger_sampling_period_read(const struct iio_device *iio_device,
		char *dst, size_t len)
{
	struct iio_device_trigger_config *trig_config = (struct iio_device_trigger_config *)iio_device_get_pdata(iio_device);

	if (len < IIO_DEVICE_SAMPLING_PERIOD_LEN) {
		LOG_ERR("Buffer size %u is too small for sampling period value, need %u",
			len, IIO_DEVICE_SAMPLING_PERIOD_LEN);
		return -ENOMEM;
	}

	return snprintk(dst, len, "%u", trig_config->period_ms) + 1;
}

static const char *iio_device_trigger_common_type_to_str(enum sensor_trigger_type type)
{
	switch (type) {
	case SENSOR_TRIG_TIMER:          return "timer";
	case SENSOR_TRIG_DATA_READY:     return "data_ready";
	case SENSOR_TRIG_DELTA:          return "delta";
	case SENSOR_TRIG_NEAR_FAR:       return "near_far";
	case SENSOR_TRIG_THRESHOLD:      return "threshold";
	case SENSOR_TRIG_TAP:            return "tap";
	case SENSOR_TRIG_DOUBLE_TAP:     return "double_tap";
	case SENSOR_TRIG_FREEFALL:       return "freefall";
	case SENSOR_TRIG_MOTION:         return "motion";
	case SENSOR_TRIG_STATIONARY:     return "stationary";
	case SENSOR_TRIG_FIFO_WATERMARK: return "fifo_watermark";
	case SENSOR_TRIG_FIFO_FULL:      return "fifo_full";
	case SENSOR_TRIG_TILT:           return "tilt";
	case SENSOR_TRIG_OVERFLOW:       return "overflow";
	default:                         return "unknown";
	}
}

static const char *iio_device_trigger_common_channel_to_str(enum sensor_channel chan)
{
	switch (chan) {
	case SENSOR_CHAN_ACCEL_X:        return "accel_x";
	case SENSOR_CHAN_ACCEL_Y:        return "accel_y";
	case SENSOR_CHAN_ACCEL_Z:        return "accel_z";
	case SENSOR_CHAN_ACCEL_XYZ:      return "accel_xyz";
	case SENSOR_CHAN_GYRO_X:         return "gyro_x";
	case SENSOR_CHAN_GYRO_Y:         return "gyro_y";
	case SENSOR_CHAN_GYRO_Z:         return "gyro_z";
	case SENSOR_CHAN_GYRO_XYZ:       return "gyro_xyz";
	case SENSOR_CHAN_MAGN_X:         return "magn_x";
	case SENSOR_CHAN_MAGN_Y:         return "magn_y";
	case SENSOR_CHAN_MAGN_Z:         return "magn_z";
	case SENSOR_CHAN_MAGN_XYZ:       return "magn_xyz";
	case SENSOR_CHAN_DIE_TEMP:       return "die_temp";
	case SENSOR_CHAN_AMBIENT_TEMP:   return "ambient_temp";
	case SENSOR_CHAN_PRESS:          return "press";
	case SENSOR_CHAN_PROX:           return "prox";
	case SENSOR_CHAN_HUMIDITY:       return "humidity";
	case SENSOR_CHAN_AMBIENT_LIGHT:  return "ambient_light";
	case SENSOR_CHAN_LIGHT:          return "light";
	case SENSOR_CHAN_IR:             return "ir";
	case SENSOR_CHAN_RED:            return "red";
	case SENSOR_CHAN_GREEN:          return "green";
	case SENSOR_CHAN_BLUE:           return "blue";
	case SENSOR_CHAN_ALTITUDE:       return "altitude";
	case SENSOR_CHAN_DISTANCE:       return "distance";
	case SENSOR_CHAN_CO2:            return "co2";
	case SENSOR_CHAN_O2:             return "o2";
	case SENSOR_CHAN_VOC:            return "voc";
	case SENSOR_CHAN_GAS_RES:        return "gas_res";
	case SENSOR_CHAN_VOLTAGE:        return "voltage";
	case SENSOR_CHAN_CURRENT:        return "current";
	case SENSOR_CHAN_POWER:          return "power";
	case SENSOR_CHAN_RESISTANCE:     return "resistance";
	case SENSOR_CHAN_ROTATION:       return "rotation";
	case SENSOR_CHAN_RPM:            return "rpm";
	case SENSOR_CHAN_FREQUENCY:      return "frequency";
	case SENSOR_CHAN_ALL:            return "all";
	default:                         return "unknown";
	}
}

static int iio_device_trigger_common_type_read(const struct iio_device *iio_device,
		char *dst, size_t len)
{
	struct iio_device_trigger_config *trig_config = (struct iio_device_trigger_config *)iio_device_get_pdata(iio_device);

	if (len < IIO_DEVICE_COMMON_TRIGGER_TYPE_LEN) {
		LOG_ERR("Buffer size %u is too small for common trigger type value, need %u",
			len, IIO_DEVICE_COMMON_TRIGGER_TYPE_LEN);
		return -ENOMEM;
	}

	const char *str = iio_device_trigger_common_type_to_str(trig_config->common_trigger_type);

	return snprintk(dst, len, "%s", str) + 1;
}

static int iio_device_trigger_common_channel_read(const struct iio_device *iio_device,
		char *dst, size_t len)
{
	struct iio_device_trigger_config *trig_config = (struct iio_device_trigger_config *)iio_device_get_pdata(iio_device);

	if (len < IIO_DEVICE_COMMON_TRIGGER_CHANNEL_LEN) {
		LOG_ERR("Buffer size %u is too small for common trigger channel value, need %u",
			len, IIO_DEVICE_COMMON_TRIGGER_CHANNEL_LEN);
		return -ENOMEM;
	}

	const char *str = iio_device_trigger_common_channel_to_str(trig_config->common_trigger_channel);

	return snprintk(dst, len, "%s", str) + 1;
}

int iio_device_trigger_read_attr(const struct iio_device *iio_device, const struct iio_attr *attr,
		char *dst, size_t len)
{

	switch (attr->type) {
	case IIO_ATTR_TYPE_DEVICE:
		if (!strcmp(attr->name, sampling_period_name)) {
			return iio_device_trigger_sampling_period_read(iio_device, dst, len);
		} else if (!strcmp(attr->name, common_trigger_type_name)) {
			return iio_device_trigger_common_type_read(iio_device, dst, len);
		} else if (!strcmp(attr->name, common_trigger_channel_name)) {
			return iio_device_trigger_common_channel_read(iio_device, dst, len);
		}
		break;

	default:
		break;
	}

	LOG_ERR("Invalid attr");
	return -EINVAL;
}

static int iio_device_trigger_sampling_period_write(const struct iio_device *iio_device,
	const char *src, size_t len)
{
	struct iio_device_trigger_config *trig_config =
		(struct iio_device_trigger_config *)iio_device_get_pdata(iio_device);
	char *end;
	unsigned long val = strtoul(src, &end, 10);

	if (end == src || val > UINT32_MAX) {
		LOG_ERR("Invalid sampling period value");
		return -EINVAL;
	}

	trig_config->period_ms = (uint32_t)val;

	return len;
}

static int iio_device_trigger_str_to_common_type(const char *str, enum sensor_trigger_type *type)
{
	if (!strcmp(str, "timer"))               { *type = SENSOR_TRIG_TIMER; }
	else if (!strcmp(str, "data_ready"))      { *type = SENSOR_TRIG_DATA_READY; }
	else if (!strcmp(str, "delta"))           { *type = SENSOR_TRIG_DELTA; }
	else if (!strcmp(str, "near_far"))        { *type = SENSOR_TRIG_NEAR_FAR; }
	else if (!strcmp(str, "threshold"))       { *type = SENSOR_TRIG_THRESHOLD; }
	else if (!strcmp(str, "tap"))             { *type = SENSOR_TRIG_TAP; }
	else if (!strcmp(str, "double_tap"))      { *type = SENSOR_TRIG_DOUBLE_TAP; }
	else if (!strcmp(str, "freefall"))        { *type = SENSOR_TRIG_FREEFALL; }
	else if (!strcmp(str, "motion"))          { *type = SENSOR_TRIG_MOTION; }
	else if (!strcmp(str, "stationary"))      { *type = SENSOR_TRIG_STATIONARY; }
	else if (!strcmp(str, "fifo_watermark")) { *type = SENSOR_TRIG_FIFO_WATERMARK; }
	else if (!strcmp(str, "fifo_full"))       { *type = SENSOR_TRIG_FIFO_FULL; }
	else if (!strcmp(str, "tilt"))            { *type = SENSOR_TRIG_TILT; }
	else if (!strcmp(str, "overflow"))        { *type = SENSOR_TRIG_OVERFLOW; }
	else { return -EINVAL; }

	return 0;
}

static int iio_device_trigger_common_type_write(const struct iio_device *iio_device,
	const char *src, size_t len)
{
	struct iio_device_trigger_config *trig_config =
		(struct iio_device_trigger_config *)iio_device_get_pdata(iio_device);
	enum sensor_trigger_type type;

	if (iio_device_trigger_str_to_common_type(src, &type)) {
		LOG_ERR("Invalid common trigger type value: %s", src);
		return -EINVAL;
	}

	trig_config->common_trigger_type = type;

	return len;
}

static int iio_device_trigger_str_to_common_channel(const char *str, enum sensor_channel *chan)
{
	if (!strcmp(str, "accel_x"))             { *chan = SENSOR_CHAN_ACCEL_X; }
	else if (!strcmp(str, "accel_y"))         { *chan = SENSOR_CHAN_ACCEL_Y; }
	else if (!strcmp(str, "accel_z"))         { *chan = SENSOR_CHAN_ACCEL_Z; }
	else if (!strcmp(str, "accel_xyz"))       { *chan = SENSOR_CHAN_ACCEL_XYZ; }
	else if (!strcmp(str, "gyro_x"))          { *chan = SENSOR_CHAN_GYRO_X; }
	else if (!strcmp(str, "gyro_y"))          { *chan = SENSOR_CHAN_GYRO_Y; }
	else if (!strcmp(str, "gyro_z"))          { *chan = SENSOR_CHAN_GYRO_Z; }
	else if (!strcmp(str, "gyro_xyz"))        { *chan = SENSOR_CHAN_GYRO_XYZ; }
	else if (!strcmp(str, "magn_x"))          { *chan = SENSOR_CHAN_MAGN_X; }
	else if (!strcmp(str, "magn_y"))          { *chan = SENSOR_CHAN_MAGN_Y; }
	else if (!strcmp(str, "magn_z"))          { *chan = SENSOR_CHAN_MAGN_Z; }
	else if (!strcmp(str, "magn_xyz"))        { *chan = SENSOR_CHAN_MAGN_XYZ; }
	else if (!strcmp(str, "die_temp"))        { *chan = SENSOR_CHAN_DIE_TEMP; }
	else if (!strcmp(str, "ambient_temp"))    { *chan = SENSOR_CHAN_AMBIENT_TEMP; }
	else if (!strcmp(str, "press"))           { *chan = SENSOR_CHAN_PRESS; }
	else if (!strcmp(str, "prox"))            { *chan = SENSOR_CHAN_PROX; }
	else if (!strcmp(str, "humidity"))        { *chan = SENSOR_CHAN_HUMIDITY; }
	else if (!strcmp(str, "ambient_light"))   { *chan = SENSOR_CHAN_AMBIENT_LIGHT; }
	else if (!strcmp(str, "light"))           { *chan = SENSOR_CHAN_LIGHT; }
	else if (!strcmp(str, "ir"))              { *chan = SENSOR_CHAN_IR; }
	else if (!strcmp(str, "red"))             { *chan = SENSOR_CHAN_RED; }
	else if (!strcmp(str, "green"))           { *chan = SENSOR_CHAN_GREEN; }
	else if (!strcmp(str, "blue"))            { *chan = SENSOR_CHAN_BLUE; }
	else if (!strcmp(str, "altitude"))        { *chan = SENSOR_CHAN_ALTITUDE; }
	else if (!strcmp(str, "distance"))        { *chan = SENSOR_CHAN_DISTANCE; }
	else if (!strcmp(str, "co2"))             { *chan = SENSOR_CHAN_CO2; }
	else if (!strcmp(str, "o2"))              { *chan = SENSOR_CHAN_O2; }
	else if (!strcmp(str, "voc"))             { *chan = SENSOR_CHAN_VOC; }
	else if (!strcmp(str, "gas_res"))         { *chan = SENSOR_CHAN_GAS_RES; }
	else if (!strcmp(str, "voltage"))         { *chan = SENSOR_CHAN_VOLTAGE; }
	else if (!strcmp(str, "current"))         { *chan = SENSOR_CHAN_CURRENT; }
	else if (!strcmp(str, "power"))           { *chan = SENSOR_CHAN_POWER; }
	else if (!strcmp(str, "resistance"))      { *chan = SENSOR_CHAN_RESISTANCE; }
	else if (!strcmp(str, "rotation"))        { *chan = SENSOR_CHAN_ROTATION; }
	else if (!strcmp(str, "rpm"))             { *chan = SENSOR_CHAN_RPM; }
	else if (!strcmp(str, "frequency"))       { *chan = SENSOR_CHAN_FREQUENCY; }
	else if (!strcmp(str, "all"))             { *chan = SENSOR_CHAN_ALL; }
	else { return -EINVAL; }

	return 0;
}

static int iio_device_trigger_common_channel_write(const struct iio_device *iio_device,
	const char *src, size_t len)
{
	struct iio_device_trigger_config *trig_config =
		(struct iio_device_trigger_config *)iio_device_get_pdata(iio_device);
	enum sensor_channel chan;

	if (iio_device_trigger_str_to_common_channel(src, &chan)) {
		LOG_ERR("Invalid common trigger channel value: %s", src);
		return -EINVAL;
	}

	trig_config->common_trigger_channel = chan;

	return len;
}

int iio_device_trigger_write_attr(const struct iio_device *iio_device, const struct iio_attr *attr,
		const char *src, size_t len)
{
	switch (attr->type) {
	case IIO_ATTR_TYPE_DEVICE:
		if (!strcmp(attr->name, sampling_period_name)) {
			return iio_device_trigger_sampling_period_write(iio_device, src, len);
		} else if (!strcmp(attr->name, common_trigger_type_name)) {
			return iio_device_trigger_common_type_write(iio_device, src, len);
		} else if (!strcmp(attr->name, common_trigger_channel_name)) {
			return iio_device_trigger_common_channel_write(iio_device, src, len);
		}
		
		break;

	default:
		break;
	}

	LOG_ERR("Invalid attr");
	return -EINVAL;
}