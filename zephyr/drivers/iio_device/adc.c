/*
 * Copyright (c) 2025 Analog Devices, Inc.
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/logging/log.h>
#include <iio/iio-backend.h>
#include <iio_device.h>

LOG_MODULE_REGISTER(iio_device_adc, CONFIG_LIBIIO_LOG_LEVEL);

struct iio_device_adc_config {
	const char *name;
	struct adc_dt_spec *channels;
	size_t num_channels;
	uint8_t address;
};

struct iio_device_adc_data {
};

static const char *const gain_name = "gain";
static const char *const sample_name = "raw";
static const char *const sample_freq = "sampling_frequency";
static const char *const scale_name = "scale";

static const char *const gain_values[] = {
	[ADC_GAIN_1_6] = "1/6",
	[ADC_GAIN_1_5] = "1/5",
	[ADC_GAIN_1_4] = "1/4",
	[ADC_GAIN_2_7] = "2/7",
	[ADC_GAIN_1_3] = "1/3",
	[ADC_GAIN_2_5] = "2/5",
	[ADC_GAIN_1_2] = "1/2",
	[ADC_GAIN_2_3] = "2/3",
	[ADC_GAIN_4_5] = "4/5",
	[ADC_GAIN_1] = "1",
	[ADC_GAIN_2] = "2",
	[ADC_GAIN_3] = "3",
	[ADC_GAIN_4] = "4",
	[ADC_GAIN_6] = "6",
	[ADC_GAIN_8] = "8",
	[ADC_GAIN_12] = "12",
	[ADC_GAIN_16] = "16",
	[ADC_GAIN_24] = "24",
	[ADC_GAIN_32] = "32",
	[ADC_GAIN_64] = "64",
	[ADC_GAIN_128] = "128",
};

static int iio_device_adc_add_channels(const struct device *dev,
		struct iio_device *iio_device)
{
	const struct iio_device_adc_config *config = dev->config;
	struct iio_channel *iio_channel;

	for (int index = 0; index < config->num_channels; index++) {
		bool output = false;
		bool scan_element = false;
		const char *name = NULL;
		const char *label = NULL;
		const char *filename = NULL;
		const struct iio_data_format fmt = {
			.length = 16,
			.bits = 16,
			.is_signed = true,
		};
		char id[32];

		snprintk(id, sizeof(id), "voltage%zu", index);

		iio_channel = iio_device_add_channel(iio_device, index, id,
				name, label, output, scan_element, &fmt);

		if (iio_err(iio_channel)) {
			LOG_ERR("Could not add channel %d", index);
			return -EINVAL;
		}

		iio_channel_set_pdata(iio_channel, (struct iio_channel_pdata *) index);

		if (iio_channel_add_attr(iio_channel, gain_name, filename)) {
			LOG_ERR("Could not add channel %d attribute %s", index, gain_name);
			return -EINVAL;
		}

		if (iio_channel_add_attr(iio_channel, sample_name, filename)) {
			LOG_ERR("Could not add channel %d attribute %s", index, sample_name);
			return -EINVAL;
		}

		if (iio_channel_add_attr(iio_channel, sample_freq, filename)) {
			LOG_ERR("Could not add channel %d attribute %s", index, sample_freq);
			return -EINVAL;
		}

		if (iio_channel_add_attr(iio_channel, scale_name, filename)) {
			LOG_ERR("Could not add channel %d attribute %s", index, scale_name);
			return -EINVAL;
		}
	}
	return 0;
}

static int iio_device_adc_read_channel_gain(const struct device *dev,
		int index, char *dst, size_t len)
{
	const struct iio_device_adc_config *config = dev->config;
	enum adc_gain gain_enum = config->channels[index].channel_cfg.gain;
	const char *gain_value;
	int gain_len;

	if (gain_enum >= ARRAY_SIZE(gain_values)) {
		LOG_ERR("Invalid gain enum: %u", gain_enum);
		return -EINVAL;
	}

	gain_value = gain_values[gain_enum];
	gain_len = strlen(gain_value) + 1;

	if (len < gain_len) {
		LOG_ERR("Buffer size %u is too small for gain value, need %u",
			len, gain_len);
		return -ENOMEM;
	}

	strcpy(dst, gain_value);

	return gain_len;
}

static int iio_device_adc_read_channel_sample(const struct device *dev,
		int index, char *dst, size_t len)
{
	const struct iio_device_adc_config *config = dev->config;
	struct adc_dt_spec *channel = &config->channels[index];
	int sample_len = channel->resolution / 8; /* bits to bytes */
	int ret;

	if (len < sample_len) {
		LOG_ERR("Buffer size %u is too small for sample value, need %u",
			len, sample_len);
		return -ENOMEM;
	}

	uint32_t tmp_buf = 0;
	struct adc_sequence sequence = {
		.buffer = &tmp_buf,
		.buffer_size = sizeof(tmp_buf),
		.channels = BIT(index),
		.resolution = channel->resolution,
	};

	ret = adc_read_dt(channel, &sequence);
	if (ret < 0) {
		LOG_ERR("Error reading adc");
		return ret;
	}

	sample_len = snprintk(dst, len, "%u", tmp_buf);

	return sample_len + 1;
}

static char* sampling = "100000";

static int iio_device_adc_read_sampling_frequency(const struct device *dev, 
	int index, char *dst, size_t len)
{
	const char *gain_value = sampling;
	int gain_len;

	gain_len = strlen(gain_value) + 1;

	if (len < gain_len) {
		LOG_ERR("Buffer size %u is too small for sampling frequency value, need %u",
		len, gain_len);
		return -ENOMEM;
	}

	strcpy(dst, gain_value);

	return gain_len;
}

static char* scale = "1";

static int iio_device_adc_read_channel_scale(const struct device *dev,
	int index, char *dst, size_t len)
{
	const char *gain_value = scale;
	int gain_len;

	gain_len = strlen(gain_value) + 1;

	if (len < gain_len) {
		LOG_ERR("Buffer size %u is too small for scale value, need %u",
		len, gain_len);
		return -ENOMEM;
	}

	strcpy(dst, gain_value);

	return gain_len;
}

static int iio_device_adc_write_channel_gain(const struct device *dev,
		int index, const char *src, size_t len)
{
	const struct iio_device_adc_config *config = dev->config;

	for (int gain_enum = 0; gain_enum < ARRAY_SIZE(gain_values); gain_enum++) {
		if (!strcmp(gain_values[gain_enum], src)) {
			config->channels[index].channel_cfg.gain = gain_enum;
			return len;
		}
	}

	LOG_ERR("Invalid gain value: %s", src);
	return -EINVAL;
}

static int iio_device_adc_read_attr(const struct device *dev,
		const struct iio_device *iio_device, const struct iio_attr *attr,
		char *dst, size_t len)
{
	const struct iio_device_adc_config *config = dev->config;
	int index = (int) iio_channel_get_pdata(attr->iio.chn);

	if (index >= config->num_channels) {
		LOG_ERR("Invalid index: %d", index);
		return -EINVAL;
	}

	if (!strcmp(attr->name, gain_name)) {
		return iio_device_adc_read_channel_gain(dev, index, dst, len);
	} else if (!strcmp(attr->name, sample_name)) {
		return iio_device_adc_read_channel_sample(dev, index, dst, len);
	} else if (!strcmp(attr->name, sample_freq)) {
		return iio_device_adc_read_sampling_frequency(dev, index, dst, len);
	} else if (!strcmp(attr->name, scale_name)) {
		return iio_device_adc_read_channel_scale(dev, index, dst, len);
	}

	LOG_ERR("Invalid attr");
	return -EINVAL;
}

static int iio_device_adc_write_attr(const struct device *dev,
		const struct iio_device *iio_device, const struct iio_attr *attr,
		const char *src, size_t len)
{
	const struct iio_device_adc_config *config = dev->config;
	int index = (int) iio_channel_get_pdata(attr->iio.chn);

	if (index >= config->num_channels) {
		LOG_ERR("Invalid index: %d", index);
		return -EINVAL;
	}

	if (!strcmp(attr->name, gain_name)) {
		return iio_device_adc_write_channel_gain(dev, index, src, len);
	}

	LOG_ERR("Invalid attr");
	return -EINVAL;
}

static int iio_device_adc_init(const struct device *dev)
{
	const struct iio_device_adc_config *config = dev->config;
	int ret;

	for (int i = 0; i < config->num_channels; i++) {
		ret = adc_channel_setup_dt(&config->channels[i]);
		if (ret < 0) {
			LOG_ERR("Could not setup channel %d (%d)", i, ret);
			return ret;
		}
	}

	return 0;
}

static DEVICE_API(iio_device, iio_device_adc_driver_api) = {
	.add_channels = iio_device_adc_add_channels,
	.read_attr = iio_device_adc_read_attr,
	.write_attr = iio_device_adc_write_attr,
};

#define DT_DRV_COMPAT iio_adc

#define IIO_DEVICE_ADC_CHANNEL(node_id, prop, idx)					\
	ADC_DT_SPEC_GET_BY_IDX(node_id, idx)

#define IIO_DEVICE_ADC_INIT(inst)							\
static struct iio_device_adc_data iio_device_adc_data_##inst;				\
											\
static struct adc_dt_spec iio_device_adc_channels_##inst[] = {				\
	DT_INST_FOREACH_PROP_ELEM_SEP(inst, io_channels, IIO_DEVICE_ADC_CHANNEL, (,))	\
};											\
											\
static const struct iio_device_adc_config iio_device_adc_config_##inst = {		\
	.name = DT_INST_PROP_OR(inst, io_name, NULL),					\
	.address = DT_INST_REG_ADDR(inst),						\
	.channels = iio_device_adc_channels_##inst,					\
	.num_channels = ARRAY_SIZE(iio_device_adc_channels_##inst),			\
};											\
											\
IIO_DEVICE_DT_INST_DEFINE(inst, iio_device_adc_init, NULL,				\
	&iio_device_adc_data_##inst, &iio_device_adc_config_##inst,			\
	POST_KERNEL, CONFIG_LIBIIO_IIO_DEVICE_ADC_INIT_PRIORITY,			\
	&iio_device_adc_driver_api);

DT_INST_FOREACH_STATUS_OKAY(IIO_DEVICE_ADC_INIT)
