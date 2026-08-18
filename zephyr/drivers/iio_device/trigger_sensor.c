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

LOG_MODULE_REGISTER(iio_device_trigger_sensor, CONFIG_LIBIIO_LOG_LEVEL);

#define IIO_DEVICE_SENSOR_TRIGGER_TYPE_LEN 17
#define IIO_DEVICE_SENSOR_TRIGGER_CHANNEL_LEN 17

struct iio_device_trigger_sensor_data {
	struct k_work work;
	struct k_mutex lock;
	bool inited;
	enum sensor_trigger_type sensor_trigger_type;
	enum sensor_channel sensor_trigger_channel;
	const struct device *sensor_zephyr_dev;
	struct iio_trigger_node sensor_node;
	struct iio_trigger_node *subscriber;
	struct sensor_trigger zephyr_trig;
};

static struct iio_trigger_list active_sensor_triggers = 
	IIO_TRIGGER_NODE_LIST_STATIC_INIT(&active_sensor_triggers);
static struct k_spinlock active_sensor_triggers_lock;
static const char *const sensor_trigger_type_name = "sensor_trigger_type";
static const char *const sensor_trigger_channel_name = "sensor_trigger_channel";

static void iio_device_trigger_sensor_work_handler(struct k_work *w);
static void iio_device_trigger_sensor_handler(const struct device *dev,
			    const struct sensor_trigger *trig);

static int iio_device_trigger_set_sensor_trigger(const struct device *zephyr_dev, 
	struct iio_device_trigger_sensor_data *trig_data)
{
	int ret;
	trig_data->zephyr_trig.type = trig_data->sensor_trigger_type;
	trig_data->zephyr_trig.chan = trig_data->sensor_trigger_channel;

	ret = sensor_trigger_set(zephyr_dev, &trig_data->zephyr_trig,
					iio_device_trigger_sensor_handler);

	if (ret != 0) {
		LOG_ERR("Failed to set sensor trigger for device %s: %d", zephyr_dev->name, ret);
		return ret;
	}

	return 0;
}

static void iio_device_trigger_sensor_init(const struct device *dev)
{
	struct iio_device_trigger_sensor_data *trig_data =
		(struct iio_device_trigger_sensor_data *)dev->data;

	if (trig_data->inited) {
			return;
	}

	trig_data->sensor_trigger_type = SENSOR_TRIG_DATA_READY;
	trig_data->sensor_trigger_channel = SENSOR_CHAN_ALL;
	trig_data->subscriber = NULL;
	trig_data->inited = true;

	k_mutex_init(&trig_data->lock);
	k_work_init(&trig_data->work, iio_device_trigger_sensor_work_handler);
}

static struct iio_device * iio_device_trigger_sensor_create(struct iio_context *ctx,
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

	if (iio_device_add_attr(trig, sensor_trigger_type_name, IIO_ATTR_TYPE_DEVICE)) {
		LOG_ERR("Could not add trigger device %s attribute %s",
				trig_config->name, sensor_trigger_type_name);
		return NULL;
	}

	if (iio_device_add_attr(trig, sensor_trigger_channel_name, IIO_ATTR_TYPE_DEVICE)) {
		LOG_ERR("Could not add trigger device %s attribute %s",
				trig_config->name, sensor_trigger_channel_name);
		return NULL;
	}

	return trig;
}

static int iio_device_trigger_sensor_subscribe(struct iio_trigger_node *node)
{
	struct iio_buffer_pdata *buf = CONTAINER_OF(node, struct iio_buffer_pdata, node);
	const struct iio_device *iio_dev = buf->iio_dev;
	const struct iio_device *trigger = iio_device_get_trigger(iio_dev);
	const struct device *trig_dev = (const struct device *) iio_device_get_pdata(trigger);
	struct iio_device_trigger_sensor_data *trig_data =
		(struct iio_device_trigger_sensor_data *)trig_dev->data;
	int ret = 0;

	k_mutex_lock(&trig_data->lock, K_FOREVER);

	if (trig_data->subscriber != NULL) {
		k_mutex_unlock(&trig_data->lock);
		return -EBUSY;
	}

	trig_data->subscriber = node;

	k_spinlock_key_t key = k_spin_lock(&active_sensor_triggers_lock);
	iio_trigger_node_add(&active_sensor_triggers, &trig_data->sensor_node);
	k_spin_unlock(&active_sensor_triggers_lock, key);

	ret = iio_device_trigger_set_sensor_trigger(trig_data->sensor_zephyr_dev, trig_data);
	if (ret < 0) {
		LOG_ERR("Could not set sensor trigger for device %s",
				trig_data->sensor_zephyr_dev->name);

		key = k_spin_lock(&active_sensor_triggers_lock);
		iio_trigger_node_remove(&active_sensor_triggers, &trig_data->sensor_node);
		k_spin_unlock(&active_sensor_triggers_lock, key);

		trig_data->subscriber = NULL;
	}

	k_mutex_unlock(&trig_data->lock);
	return ret;
}

static void iio_device_trigger_sensor_unsubscribe(struct iio_trigger_node *node)
{
	struct iio_buffer_pdata *buf = CONTAINER_OF(node, struct iio_buffer_pdata, node);
	const struct iio_device *iio_dev = buf->iio_dev;
	const struct iio_device *trigger = iio_device_get_trigger(iio_dev);
	const struct device *trig_dev = (const struct device *) iio_device_get_pdata(trigger);
	struct iio_device_trigger_sensor_data *trig_data =
		(struct iio_device_trigger_sensor_data *)trig_dev->data;

	k_mutex_lock(&trig_data->lock, K_FOREVER);

	__ASSERT(trig_data->subscriber == node, "Subscriber mismatch");

	if (trig_data->subscriber == node) {
		trig_data->zephyr_trig.type = trig_data->sensor_trigger_type;
		trig_data->zephyr_trig.chan = trig_data->sensor_trigger_channel;

		k_spinlock_key_t key = k_spin_lock(&active_sensor_triggers_lock);
		iio_trigger_node_remove(&active_sensor_triggers, &trig_data->sensor_node);
		k_spin_unlock(&active_sensor_triggers_lock, key);

		trig_data->subscriber = NULL;
		sensor_trigger_set(trig_data->sensor_zephyr_dev, &trig_data->zephyr_trig, NULL);
	}

	k_mutex_unlock(&trig_data->lock);
}


static int iio_device_trigger_sensor_handler_cb(struct iio_trigger_node *node, void *user_data)
{
	const struct device *dev = (const struct device *)user_data;
	struct iio_device_trigger_sensor_data *trig_data =
		 CONTAINER_OF(node, struct iio_device_trigger_sensor_data, sensor_node);

	if (trig_data->sensor_zephyr_dev == dev) {
		k_work_submit(&trig_data->work);
		return 1;
	}

	return 0;
}

static void iio_device_trigger_sensor_handler(const struct device *dev,
			    const struct sensor_trigger *trig)
{
	k_spinlock_key_t key = k_spin_lock(&active_sensor_triggers_lock);
	iio_trigger_node_foreach(&active_sensor_triggers,
					iio_device_trigger_sensor_handler_cb, (void *)dev);
	k_spin_unlock(&active_sensor_triggers_lock, key);
}

static void iio_device_trigger_sensor_process_subscriber(struct iio_trigger_node *node)
{
	struct iio_buffer_pdata *buf = CONTAINER_OF(node, struct iio_buffer_pdata, node);
	const struct iio_backend_ops *ops = buf->iio_dev->ctx->ops;

	if (!buf->enabled || !ops->readbuf) {
		return;
	}

	k_mutex_lock(&buf->lock, K_FOREVER);

	struct iio_trigger_node *n = iio_trigger_node_get(&buf->pending_blocks);
	if (!n) {
		k_mutex_unlock(&buf->lock);
		return;
	}

	struct iio_block_pdata *blk = CONTAINER_OF(n, struct iio_block_pdata, node);
	size_t to_write = MIN(blk->bytes_used, blk->size);

	k_mutex_unlock(&buf->lock);

    (void)ops->readbuf(buf, blk->data, to_write);

	k_sem_give(&blk->ready_sem);
}

static void iio_device_trigger_sensor_work_handler(struct k_work *w)
{
	struct iio_device_trigger_sensor_data *trig_data = CONTAINER_OF(w,
		struct iio_device_trigger_sensor_data, work);

	k_mutex_lock(&trig_data->lock, K_FOREVER);

	if (trig_data->subscriber != NULL) {
		iio_device_trigger_sensor_process_subscriber(trig_data->subscriber);
	}

	k_mutex_unlock(&trig_data->lock);
}

static const struct {
	enum sensor_trigger_type type;
	const char *name;
} iio_sensor_trigger_type_map[] = {
	{ SENSOR_TRIG_TIMER,          "timer" },
	{ SENSOR_TRIG_DATA_READY,     "data_ready" },
	{ SENSOR_TRIG_DELTA,          "delta" },
	{ SENSOR_TRIG_NEAR_FAR,       "near_far" },
	{ SENSOR_TRIG_THRESHOLD,      "threshold" },
	{ SENSOR_TRIG_TAP,            "tap" },
	{ SENSOR_TRIG_DOUBLE_TAP,     "double_tap" },
	{ SENSOR_TRIG_FREEFALL,       "freefall" },
	{ SENSOR_TRIG_MOTION,         "motion" },
	{ SENSOR_TRIG_STATIONARY,     "stationary" },
	{ SENSOR_TRIG_FIFO_WATERMARK, "fifo_watermark" },
	{ SENSOR_TRIG_FIFO_FULL,      "fifo_full" },
	{ SENSOR_TRIG_TILT,           "tilt" },
	{ SENSOR_TRIG_OVERFLOW,       "overflow" },
};

static const char *iio_device_trigger_sensor_type_to_str(enum sensor_trigger_type type)
{
	for (size_t i = 0; i < ARRAY_SIZE(iio_sensor_trigger_type_map); i++) {
		if (iio_sensor_trigger_type_map[i].type == type) {
			return iio_sensor_trigger_type_map[i].name;
		}
	}
	return "unknown";
}

static const struct {
	enum sensor_channel chan;
	const char *name;
} iio_sensor_channel_map[] = {
	{ SENSOR_CHAN_ACCEL_X,       "accel_x" },
	{ SENSOR_CHAN_ACCEL_Y,       "accel_y" },
	{ SENSOR_CHAN_ACCEL_Z,       "accel_z" },
	{ SENSOR_CHAN_ACCEL_XYZ,     "accel_xyz" },
	{ SENSOR_CHAN_GYRO_X,        "gyro_x" },
	{ SENSOR_CHAN_GYRO_Y,        "gyro_y" },
	{ SENSOR_CHAN_GYRO_Z,        "gyro_z" },
	{ SENSOR_CHAN_GYRO_XYZ,      "gyro_xyz" },
	{ SENSOR_CHAN_MAGN_X,        "magn_x" },
	{ SENSOR_CHAN_MAGN_Y,        "magn_y" },
	{ SENSOR_CHAN_MAGN_Z,        "magn_z" },
	{ SENSOR_CHAN_MAGN_XYZ,      "magn_xyz" },
	{ SENSOR_CHAN_DIE_TEMP,      "die_temp" },
	{ SENSOR_CHAN_AMBIENT_TEMP,  "ambient_temp" },
	{ SENSOR_CHAN_PRESS,         "press" },
	{ SENSOR_CHAN_PROX,          "prox" },
	{ SENSOR_CHAN_HUMIDITY,      "humidity" },
	{ SENSOR_CHAN_AMBIENT_LIGHT, "ambient_light" },
	{ SENSOR_CHAN_LIGHT,         "light" },
	{ SENSOR_CHAN_IR,            "ir" },
	{ SENSOR_CHAN_RED,           "red" },
	{ SENSOR_CHAN_GREEN,         "green" },
	{ SENSOR_CHAN_BLUE,          "blue" },
	{ SENSOR_CHAN_ALTITUDE,      "altitude" },
	{ SENSOR_CHAN_DISTANCE,      "distance" },
	{ SENSOR_CHAN_CO2,           "co2" },
	{ SENSOR_CHAN_O2,            "o2" },
	{ SENSOR_CHAN_VOC,           "voc" },
	{ SENSOR_CHAN_GAS_RES,       "gas_res" },
	{ SENSOR_CHAN_VOLTAGE,       "voltage" },
	{ SENSOR_CHAN_CURRENT,       "current" },
	{ SENSOR_CHAN_POWER,         "power" },
	{ SENSOR_CHAN_RESISTANCE,    "resistance" },
	{ SENSOR_CHAN_ROTATION,      "rotation" },
	{ SENSOR_CHAN_RPM,           "rpm" },
	{ SENSOR_CHAN_FREQUENCY,     "frequency" },
	{ SENSOR_CHAN_ALL,           "all" },
};

static const char *iio_device_trigger_sensor_channel_to_str(enum sensor_channel chan)
{
	for (size_t i = 0; i < ARRAY_SIZE(iio_sensor_channel_map); i++) {
		if (iio_sensor_channel_map[i].chan == chan) {
			return iio_sensor_channel_map[i].name;
		}
	}
	return "unknown";
}

static int iio_device_trigger_sensor_type_read(const struct device *dev, char *dst, size_t len)
{
	struct iio_device_trigger_sensor_data *trig_data =
		(struct iio_device_trigger_sensor_data *)dev->data;

	if (len < IIO_DEVICE_SENSOR_TRIGGER_TYPE_LEN) {
		LOG_ERR("Buffer size %zu is too small for sensor trigger type value, need %u",
			len, IIO_DEVICE_SENSOR_TRIGGER_TYPE_LEN);
		return -ENOMEM;
	}

	const char *str = iio_device_trigger_sensor_type_to_str(trig_data->sensor_trigger_type);

	return snprintk(dst, len, "%s", str) + 1;
}

static int iio_device_trigger_sensor_channel_read(const struct device *dev,
		char *dst, size_t len)
{
	struct iio_device_trigger_sensor_data *trig_data =
		(struct iio_device_trigger_sensor_data *)dev->data;

	if (len < IIO_DEVICE_SENSOR_TRIGGER_CHANNEL_LEN) {
		LOG_ERR("Buffer size %zu is too small for sensor trigger channel value, need %u",
			len, IIO_DEVICE_SENSOR_TRIGGER_CHANNEL_LEN);
		return -ENOMEM;
	}

	const char *str =
		iio_device_trigger_sensor_channel_to_str(trig_data->sensor_trigger_channel);

	return snprintk(dst, len, "%s", str) + 1;
}

static int iio_device_trigger_sensor_read_attr(const struct device *dev,
		const struct iio_device *iio_device, const struct iio_attr *attr,
		char *dst, size_t len)
{

	switch (attr->type) {
	case IIO_ATTR_TYPE_DEVICE:
		if (!strcmp(attr->name, sensor_trigger_type_name)) {
			return iio_device_trigger_sensor_type_read(dev, dst, len);
		} else if (!strcmp(attr->name, sensor_trigger_channel_name)) {
			return iio_device_trigger_sensor_channel_read(dev, dst, len);
		}
		break;

	default:
		break;
	}

	LOG_ERR("Invalid attr type: %d, name: %s", attr->type, attr->name);
	return -EINVAL;
}

static int iio_device_trigger_str_to_sensor_type(const char *str, enum sensor_trigger_type *type,
						size_t len)
{
	for (size_t i = 0; i < ARRAY_SIZE(iio_sensor_trigger_type_map); i++) {
		if (!strcmp(str, iio_sensor_trigger_type_map[i].name)) {
			*type = iio_sensor_trigger_type_map[i].type;
			return 0;
		}
	}
	return -EINVAL;
}

static int iio_device_trigger_sensor_type_write(const struct device *dev,
						const char *src, size_t len)
{
	struct iio_device_trigger_sensor_data *trig_data =
		(struct iio_device_trigger_sensor_data *)dev->data;
	enum sensor_trigger_type type;
	char tmp[32];
 	size_t n = MIN(len, sizeof(tmp) - 1);

	strncpy(tmp, src, n);
 	tmp[n] = '\0';
 	while (n > 0 && (tmp[n - 1] == '\n' || tmp[n - 1] == '\r' || tmp[n - 1] == ' '
		|| tmp[n - 1] == '\t')) {
 		tmp[--n] = '\0';
 	}

 	if (iio_device_trigger_str_to_sensor_type(tmp, &type, n)) {
 		LOG_ERR("Invalid sensor trigger type value: %s", tmp);
		return -EINVAL;
	}

	trig_data->sensor_trigger_type = type;

	return len;
}

static int iio_device_trigger_str_to_sensor_channel(const char *str, enum sensor_channel *chan,
							size_t len)
{
	for (size_t i = 0; i < ARRAY_SIZE(iio_sensor_channel_map); i++) {
		if (!strcmp(str, iio_sensor_channel_map[i].name)) {
			*chan = iio_sensor_channel_map[i].chan;
			return 0;
		}
	}
	return -EINVAL;
}

static int iio_device_trigger_sensor_channel_write(const struct device *dev,
	const char *src, size_t len)
{
	struct iio_device_trigger_sensor_data *trig_data =
		(struct iio_device_trigger_sensor_data *)dev->data;
	enum sensor_channel chan;
	char tmp[32];
 	size_t n = MIN(len, sizeof(tmp) - 1);

	strncpy(tmp, src, n);
 	tmp[n] = '\0';
 	while (n > 0 && (tmp[n - 1] == '\n' || tmp[n - 1] == '\r' || tmp[n - 1] == ' '
		|| tmp[n - 1] == '\t')) {
 		tmp[--n] = '\0';
 	}

	if (iio_device_trigger_str_to_sensor_channel(tmp, &chan, n)) {
		LOG_ERR("Invalid sensor trigger channel value: %s", tmp);
		return -EINVAL;
	}

	trig_data->sensor_trigger_channel = chan;

	return len;
}

static int iio_device_trigger_sensor_write_attr(const struct device *dev,
		const struct iio_device *iio_device, const struct iio_attr *attr,
		const char *src, size_t len)
{
	switch (attr->type) {
	case IIO_ATTR_TYPE_DEVICE:
		if (!strcmp(attr->name, sensor_trigger_type_name)) {
			return iio_device_trigger_sensor_type_write(dev, src, len);
		} else if (!strcmp(attr->name, sensor_trigger_channel_name)) {
			return iio_device_trigger_sensor_channel_write(dev, src, len);
		}
		
		break;

	default:
		break;
	}

	LOG_ERR("Invalid attr type: %d, name: %s", attr->type, attr->name);
	return -EINVAL;
}

static DEVICE_API(iio_trigger, iio_device_trigger_sensor_driver_api) = {
	.attr_api.read_attr = iio_device_trigger_sensor_read_attr,
	.attr_api.write_attr = iio_device_trigger_sensor_write_attr,
	.create = iio_device_trigger_sensor_create,
	.init = iio_device_trigger_sensor_init,
	.subscribe = iio_device_trigger_sensor_subscribe,
	.unsubscribe = iio_device_trigger_sensor_unsubscribe,
};

#define DT_DRV_COMPAT iio_trigger_sensor

#define IIO_DEVICE_TRIGGER_SENSOR_INIT(inst)												\
static struct iio_device_trigger_sensor_data iio_device_trigger_sensor_data_##inst = {		\
	.inited = false, 									\
	.sensor_zephyr_dev = COND_CODE_1(							\
		DT_NODE_HAS_PROP(DT_PARENT(DT_DRV_INST(inst)), sensor_device),			\
		(DEVICE_DT_GET(DT_PHANDLE(DT_PARENT(DT_DRV_INST(inst)), sensor_device))),	\
		(NULL)),									\
};												\
												\
static const struct iio_device_trigger_config iio_device_trigger_sensor_config_##inst = {	\
	.trigger_id = DT_INST_PROP_OR(inst, trigger_id, "trigger" STRINGIFY(inst)),		\
	.name = DT_INST_PROP_OR(inst, io_name, ""),						\
};												\
												\
IIO_DEVICE_DT_INST_DEFINE(inst,									\
	NULL, NULL, &iio_device_trigger_sensor_data_##inst,					\
	&iio_device_trigger_sensor_config_##inst,						\
	POST_KERNEL, CONFIG_LIBIIO_IIO_DEVICE_TRIGGER_SENSOR_INIT_PRIORITY,			\
	&iio_device_trigger_sensor_driver_api);

DT_INST_FOREACH_STATUS_OKAY(IIO_DEVICE_TRIGGER_SENSOR_INIT)
