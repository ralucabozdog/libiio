/*
 * Copyright (c) 2026 Analog Devices, Inc.
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/logging/log.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/rtio/rtio.h>
#include <iio/iio-backend.h>
#include <iio_device.h>
#include <iio_trigger.h>
#include <iio_trigger_private.h>
#include <iio-private.h>

LOG_MODULE_REGISTER(iio_device_trigger_sensor_stream, CONFIG_LIBIIO_LOG_LEVEL);

union iio_device_trigger_sensor_stream_out_data {
	struct sensor_three_axis_data accel_data;
	q31_t int_data;
};

struct iio_device_trigger_sensor_stream_data {
	struct iio_device_trigger_common_stream_data common;
	const struct device *sensor_zephyr_dev;
	const enum sensor_channel *channels;
	size_t channel_count;
};

static void iio_device_trigger_sensor_stream_work_handler(struct k_work *w);

static void iio_device_trigger_sensor_stream_init(const struct device *dev)
{
	struct iio_device_trigger_sensor_stream_data *trig_data =
		(struct iio_device_trigger_sensor_stream_data *)dev->data;

	iio_trigger_stream_init(&trig_data->common,
		CONFIG_LIBIIO_IIO_DEVICE_TRIGGER_SENSOR_STREAM_THREAD_STACK_SIZE,
		CONFIG_LIBIIO_IIO_DEVICE_TRIGGER_SENSOR_STREAM_THREAD_PRIORITY,
		iio_device_trigger_sensor_stream_work_handler);
}

static struct iio_device * iio_device_trigger_sensor_stream_create(struct iio_context *ctx,
	const struct device *dev)
{
	struct iio_device_trigger_sensor_stream_data *trig_data =
		(struct iio_device_trigger_sensor_stream_data *)dev->data;

	return iio_trigger_stream_create(ctx, dev, &trig_data->common);
}

static int iio_device_trigger_sensor_stream_subscribe(struct iio_trigger_node *node)
{
	struct iio_buffer_pdata *buf = CONTAINER_OF(node, struct iio_buffer_pdata, node);
	const struct iio_device *iio_dev = buf->iio_dev;
	const struct iio_device *trigger = iio_device_get_trigger(iio_dev);
	const struct device *trig_dev = (const struct device *) iio_device_get_pdata(trigger);
	struct iio_device_trigger_sensor_stream_data *trig_data =
		(struct iio_device_trigger_sensor_stream_data *)trig_dev->data;

	return iio_trigger_stream_subscribe(node, &trig_data->common, &sensor_stream);
}

static void iio_device_trigger_sensor_stream_unsubscribe(struct iio_trigger_node *node)
{
	struct iio_buffer_pdata *buf = CONTAINER_OF(node, struct iio_buffer_pdata, node);
	const struct iio_device *iio_dev = buf->iio_dev;
	const struct iio_device *trigger = iio_device_get_trigger(iio_dev);
	const struct device *trig_dev = (const struct device *) iio_device_get_pdata(trigger);
	struct iio_device_trigger_sensor_stream_data *trig_data =
		(struct iio_device_trigger_sensor_stream_data *)trig_dev->data;

	iio_trigger_stream_unsubscribe(node, &trig_data->common);
}

static const struct iio_data_format * iio_device_trigger_sensor_stream_get_fmt(
	struct iio_device_trigger_sensor_stream_data *trig_data, enum sensor_channel sensor_channel)
{
	struct iio_buffer_pdata *buf = CONTAINER_OF(trig_data->common.subscriber,
							struct iio_buffer_pdata, node);
	const struct iio_device *iio_dev = buf->iio_dev;
	unsigned int nb_channels = iio_device_get_channels_count(iio_dev);

	for (size_t i = 0; i < trig_data->channel_count; i++) {
		for (unsigned int ch_idx = 0; ch_idx < nb_channels; ch_idx++) {
			const struct iio_channel *chn = iio_device_get_channel(iio_dev, ch_idx);
			if (iio_channel_is_scan_element(chn) &&
				iio_channel_is_enabled(chn, buf->mask)) {
				enum sensor_channel ch_type =
					(enum sensor_channel)iio_channel_get_pdata(chn);
				if ((sensor_channel == SENSOR_CHAN_ACCEL_XYZ) &&
					(ch_type == SENSOR_CHAN_ACCEL_X
						|| ch_type == SENSOR_CHAN_ACCEL_Y
						|| ch_type == SENSOR_CHAN_ACCEL_Z)) {
					const struct iio_data_format *fmt = 
						iio_channel_get_data_format(chn);
					return fmt;
				} else if ((sensor_channel == SENSOR_CHAN_GYRO_XYZ) &&
					(ch_type == SENSOR_CHAN_GYRO_X
						|| ch_type == SENSOR_CHAN_GYRO_Y
						|| ch_type == SENSOR_CHAN_GYRO_Z)) {
					const struct iio_data_format *fmt =
						iio_channel_get_data_format(chn);
					return fmt;
				} else if ((sensor_channel == SENSOR_CHAN_MAGN_XYZ) &&
					(ch_type == SENSOR_CHAN_MAGN_X
						|| ch_type == SENSOR_CHAN_MAGN_Y
						|| ch_type == SENSOR_CHAN_MAGN_Z)) {
					const struct iio_data_format *fmt =
						iio_channel_get_data_format(chn);
					return fmt;
				} else if (ch_type == sensor_channel) {
					const struct iio_data_format *fmt =
						iio_channel_get_data_format(chn);
					return fmt;
				}
			}
		}
	}

	return NULL;
}

static void iio_device_trigger_sensor_stream_submit_work(struct iio_trigger_node *node)
{
	struct iio_buffer_pdata *buf = CONTAINER_OF(node, struct iio_buffer_pdata, node);
	const struct iio_device *iio_dev = buf->iio_dev;
	const struct iio_device *trigger = iio_device_get_trigger(iio_dev);
	const struct device *trig_dev = (const struct device *) iio_device_get_pdata(trigger);
	struct iio_device_trigger_sensor_stream_data *trig_data =
		(struct iio_device_trigger_sensor_stream_data *)trig_dev->data;

	iio_trigger_stream_submit_work(node, &trig_data->common);
}

static int iio_device_trigger_sensor_stream_get_frame_count(
	struct iio_device_trigger_sensor_stream_data *trig_data, uint8_t *buf, uint32_t buf_len,
	uint16_t *frame_count, const struct sensor_decoder_api *decoder, uint16_t *min_frames)
{
	int rc = 0;

	for (int i = 0; i < trig_data->channel_count; i++) {
		rc = decoder->get_frame_count(buf,
			(struct sensor_chan_spec) {trig_data->channels[i], 0}, &frame_count[i]);

		if (rc != 0) {
			LOG_ERR("get_frame_count failed %d\n", rc);
			return rc;
		}
	}

	*min_frames = frame_count[0];
	for (size_t i = 1; i < trig_data->channel_count; i++) {
		if (frame_count[i] < *min_frames) {
			*min_frames = frame_count[i];
		}
	}

	return 0;
}

static void iio_device_trigger_sensor_stream_process_ch_data(const enum sensor_channel channel, 
	union iio_device_trigger_sensor_stream_out_data output_data,
	const struct iio_data_format *fmt, uint8_t *out, size_t bytes_per_channel,
	unsigned *ptr_pos)
{
	int8_t shift;
	q31_t raw;
	int32_t int_val;

	switch (channel) {
	case SENSOR_CHAN_ACCEL_X:
		shift = output_data.accel_data.shift;
		raw = output_data.accel_data.readings[0].x;
		/* Convert to milli-meters per second squared (milli-m/s^2) */
		int_val = (int32_t)((int64_t)raw*1000 >> (31 - shift));
		iio_trigger_stream_write_ch_data(fmt, (out + *ptr_pos), int_val,
							bytes_per_channel);
		*ptr_pos += bytes_per_channel;
		break;
	case SENSOR_CHAN_ACCEL_Y:
		shift = output_data.accel_data.shift;
		raw = output_data.accel_data.readings[0].y;
		/* Convert to milli-meters per second squared (milli-m/s^2) */
		int_val = (int32_t)((int64_t)raw*1000 >> (31 - shift));
		iio_trigger_stream_write_ch_data(fmt, (out + *ptr_pos), int_val,
							bytes_per_channel);
		*ptr_pos += bytes_per_channel;
		break;
	case SENSOR_CHAN_ACCEL_Z:
		shift = output_data.accel_data.shift;
		raw = output_data.accel_data.readings[0].z;
		/* Convert to milli-meters per second squared (milli-m/s^2) */
		int_val = (int32_t)((int64_t)raw*1000 >> (31 - shift)); 
		iio_trigger_stream_write_ch_data(fmt, (out + *ptr_pos), int_val,
							bytes_per_channel);
		*ptr_pos += bytes_per_channel;
		break;
	case SENSOR_CHAN_ACCEL_XYZ:
		shift = output_data.accel_data.shift;
		q31_t raw_x = output_data.accel_data.readings[0].x;
		q31_t raw_y = output_data.accel_data.readings[0].y;
		q31_t raw_z = output_data.accel_data.readings[0].z;
		/* Convert to milli-meters per second squared (milli-m/s^2) */
		int_val = (int32_t)((int64_t)raw_x*1000 >> (31 - shift));
		iio_trigger_stream_write_ch_data(fmt, (out + *ptr_pos), int_val,
							bytes_per_channel);
		*ptr_pos += bytes_per_channel;
		/* Convert to milli-meters per second squared (milli-m/s^2) */
		int_val = (int32_t)((int64_t)raw_y*1000 >> (31 - shift));
		iio_trigger_stream_write_ch_data(fmt, (out + *ptr_pos), int_val,
							bytes_per_channel);
		*ptr_pos += bytes_per_channel;
		/* Convert to milli-meters per second squared (milli-m/s^2) */
		int_val = (int32_t)((int64_t)raw_z*1000 >> (31 - shift));
		iio_trigger_stream_write_ch_data(fmt, (out + *ptr_pos), int_val,
							bytes_per_channel);
		*ptr_pos += bytes_per_channel;
		break;
	default:
		LOG_ERR("Unsupported channel type: %d\n", channel);
		break;
	}
}

static size_t iio_device_trigger_sensor_stream_get_bytes_per_channel(void *trig_data,
	const struct iio_data_format *fmt, size_t *bytes_per_channel)
{
	struct iio_device_trigger_sensor_stream_data *data =
		(struct iio_device_trigger_sensor_stream_data *)trig_data;
	*bytes_per_channel = DIV_ROUND_UP(fmt->length, BITS_PER_BYTE);
	unsigned bytes_per_sample = 0;

	for (size_t i = 0; i < data->channel_count; i++) {
		if ((data->channels[i] == SENSOR_CHAN_ACCEL_XYZ) ||
			(data->channels[i] == SENSOR_CHAN_GYRO_XYZ) ||
			(data->channels[i] == SENSOR_CHAN_MAGN_XYZ)) {
			/* For XYZ channels, we have 3 axes */
			bytes_per_sample += *bytes_per_channel * 3;
		} else {
			bytes_per_sample += *bytes_per_channel;
		}
	}

	return bytes_per_sample;
}

static size_t sensor_stream_get_channel_count(void *trig_data)
{
	return ((struct iio_device_trigger_sensor_stream_data *)trig_data)->channel_count;
}

static int sensor_stream_get_decoder(void *trig_data, const void **decoder)
{
	struct iio_device_trigger_sensor_stream_data *data = trig_data;

	return sensor_get_decoder(data->sensor_zephyr_dev,
		(const struct sensor_decoder_api **)decoder);
}

static const struct iio_data_format *sensor_stream_get_fmt(
	struct iio_device_trigger_common_stream_data *common,
	void *trig_data, size_t ch_idx)
{
	struct iio_device_trigger_sensor_stream_data *data = trig_data;

	(void)common;
	return iio_device_trigger_sensor_stream_get_fmt(data, data->channels[ch_idx]);
}

static int sensor_stream_get_min_frame_count(void *trig_data, const void *decoder,
	uint8_t *buf, uint32_t buf_len, uint16_t *min_frames)
{
	struct iio_device_trigger_sensor_stream_data *data = trig_data;
	uint16_t frame_count[IIO_TRIGGER_STREAM_MAX_CHANNELS];

	return iio_device_trigger_sensor_stream_get_frame_count(data, buf, buf_len,
		frame_count, (const struct sensor_decoder_api *)decoder, min_frames);
}

static int sensor_stream_decode_channel(void *trig_data, const void *decoder,
	uint8_t *buf, size_t ch_idx, uint32_t *fit, void *scratch)
{
	struct iio_device_trigger_sensor_stream_data *data = trig_data;

	return ((const struct sensor_decoder_api *)decoder)->decode(buf,
		(struct sensor_chan_spec){data->channels[ch_idx], 0},
		fit, 1, scratch);
}

static void sensor_stream_pack_channel(void *trig_data, size_t ch_idx,
	const void *scratch, const struct iio_data_format *fmt,
	uint8_t *out, size_t bytes_per_channel, unsigned *ptr_pos)
{
	struct iio_device_trigger_sensor_stream_data *data = trig_data;
	union iio_device_trigger_sensor_stream_out_data output_data;

	memcpy(&output_data, scratch, sizeof(output_data));
	iio_device_trigger_sensor_stream_process_ch_data(data->channels[ch_idx],
		output_data, fmt, out, bytes_per_channel, ptr_pos);
}

static int iio_trigger_stream_check_decode_return(int rc)
{
	return rc == 0 ? -EINVAL : rc;
}

static const struct iio_trigger_stream_ops sensor_stream_ops = {
	.get_channel_count    = sensor_stream_get_channel_count,
	.get_decoder          = sensor_stream_get_decoder,
	.get_fmt              = sensor_stream_get_fmt,
	.get_bytes_per_sample = iio_device_trigger_sensor_stream_get_bytes_per_channel,
	.get_min_frame_count  = sensor_stream_get_min_frame_count,
	.decode_channel       = sensor_stream_decode_channel,
	.pack_channel         = sensor_stream_pack_channel,
	.check_decode_return  = iio_trigger_stream_check_decode_return,
};

static void iio_device_trigger_sensor_stream_work_handler(struct k_work *w)
{
	struct iio_device_trigger_common_stream_data *common =
		CONTAINER_OF(w, struct iio_device_trigger_common_stream_data, work);
	struct iio_device_trigger_sensor_stream_data *trig_data =
		CONTAINER_OF(common, struct iio_device_trigger_sensor_stream_data, common);

	iio_trigger_stream_work_handler_common(w, &sensor_stream_ops, trig_data);
}

static DEVICE_API(iio_trigger, iio_device_trigger_sensor_stream_driver_api) = {
	.create = iio_device_trigger_sensor_stream_create,
	.init = iio_device_trigger_sensor_stream_init,
	.subscribe = iio_device_trigger_sensor_stream_subscribe,
	.unsubscribe = iio_device_trigger_sensor_stream_unsubscribe,
	.submit_work = iio_device_trigger_sensor_stream_submit_work,
};

#define DT_DRV_COMPAT iio_trigger_sensor_stream

#define IIO_DEVICE_TRIGGER_SENSOR_STREAM_INIT(inst)						\
												\
	static const enum sensor_channel iio_trigger_sensor_stream_channels_##inst[] =		\
		DT_INST_PROP(inst, sensor_stream_channels);					\
												\
	K_THREAD_STACK_DEFINE(iio_trigger_sensor_stream_stack_##inst,				\
		CONFIG_LIBIIO_IIO_DEVICE_TRIGGER_SENSOR_STREAM_THREAD_STACK_SIZE);		\
												\
	SENSOR_DT_STREAM_IODEV(iio_trigger_sensor_stream_iodev_##inst,				\
	DT_PHANDLE(DT_PARENT(DT_DRV_INST(inst)), sensor_device),				\
	{SENSOR_TRIG_FIFO_FULL, SENSOR_STREAM_DATA_INCLUDE},					\
	{SENSOR_TRIG_FIFO_WATERMARK, SENSOR_STREAM_DATA_INCLUDE});				\
												\
	RTIO_DEFINE_WITH_MEMPOOL(iio_trigger_sensor_stream_rtio_ctx_##inst,			\
		CONFIG_LIBIIO_IIO_DEVICE_TRIGGER_SENSOR_STREAM_SQ_SZ,				\
		CONFIG_LIBIIO_IIO_DEVICE_TRIGGER_SENSOR_STREAM_CQ_SZ,				\
		CONFIG_LIBIIO_IIO_DEVICE_TRIGGER_SENSOR_STREAM_NUM_BLKS,			\
		CONFIG_LIBIIO_IIO_DEVICE_TRIGGER_SENSOR_STREAM_BLK_SIZE,			\
		sizeof(void *));								\
												\
	static struct iio_device_trigger_sensor_stream_data					\
		iio_device_trigger_sensor_stream_data_##inst = {				\
		.common.inited = false,								\
		.common.iodev = &iio_trigger_sensor_stream_iodev_##inst,			\
		.common.rtio_ctx = &iio_trigger_sensor_stream_rtio_ctx_##inst,			\
		.sensor_zephyr_dev = COND_CODE_1(						\
		DT_NODE_HAS_PROP(DT_PARENT(DT_DRV_INST(inst)), sensor_device),			\
		(DEVICE_DT_GET(DT_PHANDLE(DT_PARENT(DT_DRV_INST(inst)), sensor_device))),	\
		(NULL)),									\
		.channels = iio_trigger_sensor_stream_channels_##inst,				\
		.channel_count = DT_INST_PROP_LEN(inst, sensor_stream_channels),		\
		.common.trig_wq_stack = iio_trigger_sensor_stream_stack_##inst,			\
	};											\
												\
	static const struct iio_device_trigger_config						\
		iio_device_trigger_sensor_stream_config_##inst = {				\
		.trigger_id = DT_INST_PROP_OR(inst, trigger_id, "trigger" STRINGIFY(inst)),	\
		.name = DT_INST_PROP_OR(inst, io_name, ""),					\
	};											\
												\
	IIO_DEVICE_DT_INST_DEFINE(inst,								\
		NULL, NULL,	&iio_device_trigger_sensor_stream_data_##inst,			\
		&iio_device_trigger_sensor_stream_config_##inst,				\
		POST_KERNEL, CONFIG_LIBIIO_IIO_DEVICE_TRIGGER_SENSOR_STREAM_INIT_PRIORITY,	\
		&iio_device_trigger_sensor_stream_driver_api);

DT_INST_FOREACH_STATUS_OKAY(IIO_DEVICE_TRIGGER_SENSOR_STREAM_INIT)
