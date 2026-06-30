/*
 * Copyright (c) 2025 Analog Devices, Inc.
 *
 * SPDX-License-Identifier: MIT
 */

#include <math.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/dac.h>
#include <zephyr/logging/log.h>
#include <iio/iio-backend.h>
#include <iio_device.h>
#include <iio_trigger.h>

#ifdef CONFIG_ADC_STREAM
#include <zephyr/rtio/rtio.h>
#endif

LOG_MODULE_REGISTER(iio_device_io_channels, CONFIG_LIBIIO_LOG_LEVEL);

#define IIO_DEVICE_INT_REF_VOL_LEN 6 /* max voltage is 65535 which is 5 digits + null terminator */
#define IIO_DEVICE_SCALE_LEN 10 /* scale format is xx.xxx + null terminator */
#define IIO_DEVICE_GAIN_LEN 4 /* max gain is 128 which is 3 digits + null terminator */
#define IIO_DEVICE_PROCESS_LEN 12 /* process is int type so it needs 11 digits + null terminator */
#define IIO_DEVICE_REF_LEN 11 /* max reference is External0 so it needs 10 digits + null terminator */
#define IIO_DEVICE_DIFFERENTIAL_LEN 2 /* max differential is 1 so it needs 1 digit + null terminator */

enum io_channel_type {
	IO_CHANNEL_TYPE_ADC,
	IO_CHANNEL_TYPE_DAC,
};

#define IO_CHANNEL_DAC_ADC_CHANNEL_IDX_NONE UINT8_MAX

struct iio_device_io_channels_channel {
	enum io_channel_type type;
	const char *name;

	union {
		struct adc_dt_spec adc;
		struct dac_dt_spec dac;
	};
};

struct iio_device_io_channels_config {
	const struct iio_device_io_channels_channel *channels;
	size_t num_channels;
	uint8_t address;
	const char *buffer_name;
	const struct device *trigger_dev;
};

struct iio_device_io_channels_channel_adc_overrides {
	enum adc_gain gain;
	enum adc_reference reference;
	uint8_t differential;
};
struct iio_device_io_channels_channel_dac_overrides {
};

struct iio_device_io_channels_channel_overrides {
	union {
		struct iio_device_io_channels_channel_adc_overrides adc;
		struct iio_device_io_channels_channel_dac_overrides dac;
	};
};

struct iio_device_io_channels_dac_channel_data {
	uint8_t adc_channel_idx;
};

struct iio_device_io_channels_channel_data {
	union {
		struct iio_device_io_channels_dac_channel_data dac;
	};
};

struct iio_device_io_channels_data {
	struct iio_device_io_channels_channel_overrides *overrides;
	struct iio_device_io_channels_channel_data *channels;
#ifdef CONFIG_ADC_STREAM
	struct rtio *stream_ctx;
	struct rtio_iodev *stream_iodev;
	const struct adc_decoder_api *decoder;
	struct adc_sequence stream_seq;
	bool streaming_active;
#endif
};

static const char *const raw_name = "raw";
static const char *const scale_name = "scale";
static const char *const internal_ref_voltage_name = "internal_ref_voltage";
static const char *const gain_name = "gain";
static const char *const process_name = "process";
static const char *const reference_name = "reference";
static const char *const differential_name = "differential";

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

static const char *const reference_values[] = {
	[ADC_REF_VDD_1] = "VDD",
	[ADC_REF_VDD_1_2] = "VDD/2",
	[ADC_REF_VDD_1_3] = "VDD/3",
	[ADC_REF_VDD_1_4] = "VDD/4",
	[ADC_REF_INTERNAL] = "Internal",
	[ADC_REF_EXTERNAL0] = "External0",
	[ADC_REF_EXTERNAL1] = "External1",
};

static int iio_device_io_channels_add_channels(const struct device *dev,
		struct iio_device *iio_device)
{
	const struct iio_device_io_channels_config *config = dev->config;
	struct iio_channel *iio_channel;

	bool scan_element = true;
	const char *name = NULL;
	const char *label = NULL;
	const char *filename = NULL;
	char id[32];
	int index;

	for (index = 0; index < config->num_channels; index++) {
		bool output = config->channels[index].type == IO_CHANNEL_TYPE_DAC;
		bool is_signed;
		int scale_resolution;
		struct iio_data_format fmt;

		if (config->channels[index].type == IO_CHANNEL_TYPE_ADC) {
			const struct adc_dt_spec *ch = &config->channels[index].adc;

			is_signed = ch->channel_cfg_dt_node_exists
				  ? ch->channel_cfg.differential : 0;
			scale_resolution = ch->resolution - (is_signed ? 1 : 0);
			fmt = (struct iio_data_format){
				.length = (ch->resolution / 8 + (ch->resolution % 8 != 0)) * 8,
				.bits = ch->resolution,
				.is_signed = is_signed,
				.with_scale = true,
				.scale = ((double)ch->vref_mv)
					/ (double)(1u << scale_resolution),
				.is_be = true,
			};
		} else {
			const struct dac_dt_spec *ch = &config->channels[index].dac;

			fmt = (struct iio_data_format){
				.length = (ch->channel_cfg.resolution / 8 + (ch->channel_cfg.resolution % 8 != 0)) * 8,
				.bits = ch->channel_cfg.resolution,
				.is_signed = false,
				.with_scale = (ch->vref_mv != 0
					    && ch->channel_cfg.resolution != 0),
				.scale = (ch->channel_cfg.resolution != 0)
					? ((double)ch->vref_mv)
					  / (double)(1u << ch->channel_cfg.resolution)
					: 0.0,
				.is_be = true,
			};
		}

		strncpy(id, config->channels[index].name, sizeof(id));

		iio_channel = iio_device_add_channel(iio_device, index, id,
				name, label, output, scan_element, &fmt);

		if (iio_err(iio_channel)) {
			LOG_ERR("Could not add channel %d", index);
			return -EINVAL;
		}

		iio_channel_set_pdata(iio_channel, (struct iio_channel_pdata *) index);

		if (iio_channel_add_attr(iio_channel, raw_name, filename)) {
			LOG_ERR("Could not add channel %d attribute %s", index, raw_name);
			return -EINVAL;
		}

		switch (config->channels[index].type) {
			case IO_CHANNEL_TYPE_ADC: {
				const struct adc_dt_spec *channel = &config->channels[index].adc;

				if (channel->resolution != 0) {
					if (iio_channel_add_attr(iio_channel, scale_name, filename)) {
						LOG_ERR("Could not add channel %d attribute %s", index, scale_name);
						return -EINVAL;
					}
				}

				if (iio_channel_add_attr(iio_channel, gain_name, filename)) {
					LOG_ERR("Could not add channel %d attribute %s", index, gain_name);
					return -EINVAL;
				}

				if (channel->vref_mv != 0) {
					if (iio_channel_add_attr(iio_channel, process_name, filename)) {
						LOG_ERR("Could not add channel %d attribute %s", index, process_name);
						return -EINVAL;
					}
				}

				if (iio_channel_add_attr(iio_channel, reference_name, filename)) {
					LOG_ERR("Could not add channel %d attribute %s", index, reference_name);
					return -EINVAL;
				}

				if (iio_channel_add_attr(iio_channel, differential_name, filename)) {
					LOG_ERR("Could not add channel %d attribute %s", index, differential_name);
					return -EINVAL;
				}

				break;
			}
			case IO_CHANNEL_TYPE_DAC: {
				const struct dac_dt_spec *channel = &config->channels[index].dac;
				if (channel->channel_cfg.resolution != 0) {
					if (iio_channel_add_attr(iio_channel, scale_name, filename)) {
						LOG_ERR("Could not add channel %d attribute %s", index, scale_name);
						return -EINVAL;
					}
				}

				break;
			}
		}
	}

	if (iio_device_add_attr(iio_device, internal_ref_voltage_name, IIO_ATTR_TYPE_DEVICE)) {
		LOG_ERR("Could not add device %d attribute %s", index, internal_ref_voltage_name);
		return -EINVAL;
	}

	return 0;
}

static int iio_device_io_channels_read_adc_channel_raw(const struct device *dev,
		int index, char *dst, size_t len, uint32_t *val)
{
	const struct iio_device_io_channels_config *config = dev->config;
	const struct adc_dt_spec *channel = &config->channels[index].adc;
	struct adc_sequence sequence = {
		.buffer = val,
		.buffer_size = sizeof(*val),
	};
	int ret;

	ret = adc_sequence_init_dt(channel, &sequence);
	if (ret < 0) {
		LOG_ERR("Error initializing adc sequence");
		return ret;
	}

	if (len < sequence.buffer_size) {
		LOG_ERR("Buffer size %u is too small for process value, need %u",
			len, sequence.buffer_size);
		return -ENOMEM;
	}

	ret = adc_read_dt(channel, &sequence);
	if (ret < 0) {
		LOG_ERR("Error reading adc");
		return ret;
	}

	return 0;
}

static int iio_device_io_channels_read_dac_channel_raw(const struct device *dev,
		int index, char *dst, size_t len, uint32_t *val)
{
	struct iio_device_io_channels_data *data = dev->data;

	if (data->channels[index].dac.adc_channel_idx == IO_CHANNEL_DAC_ADC_CHANNEL_IDX_NONE) {
		LOG_ERR("Attempt to read DAC channel with no matching ADC channel available");
		return -ENODEV;
	}

	/* Delegate read to the corresponding ADC channel with a matching name */
	return iio_device_io_channels_read_adc_channel_raw(dev, data->channels[index].dac.adc_channel_idx, dst, len, val);
}

static int iio_device_io_channels_read_channel_raw(const struct device *dev,
		int index, char *dst, size_t len)
{
	const struct iio_device_io_channels_config *config = dev->config;

	int ret;
	uint32_t tmp_buf = 0;

	switch (config->channels[index].type) {
		case IO_CHANNEL_TYPE_ADC:
			ret = iio_device_io_channels_read_adc_channel_raw(dev, index, dst, len, &tmp_buf);
			break;
		case IO_CHANNEL_TYPE_DAC:
			ret = iio_device_io_channels_read_dac_channel_raw(dev, index, dst, len, &tmp_buf);
			break;
		default:
			return -EINVAL;
	}

	if (ret) {
		LOG_ERR("Failed to get the value from the channel device (%d)", ret);
		return ret;
	}

	return snprintk(dst, len, "%u", tmp_buf) + 1;
}

static int iio_device_io_channels_read_channel_scale(const struct device *dev,
		int index, char *dst, size_t len)
{
	const struct iio_device_io_channels_config *config = dev->config;
	uint32_t scale_uv;
	uint16_t vref_mv;
	uint8_t resolution;
	uint32_t whole;
	uint32_t frac;

	if (len < IIO_DEVICE_SCALE_LEN) {
		LOG_ERR("Buffer size %u is too small for scale value, need %u",
			len, IIO_DEVICE_SCALE_LEN);
		return -ENOMEM;
	}

	if (config->channels[index].type == IO_CHANNEL_TYPE_ADC) {
		const struct adc_dt_spec *channel = &config->channels[index].adc;
		vref_mv = channel->vref_mv;
		resolution = channel->resolution;
		if (channel->channel_cfg_dt_node_exists && channel->channel_cfg.differential) {
			resolution -= 1;
		}
	} else {
		const struct dac_dt_spec *channel = &config->channels[index].dac;
		vref_mv = channel->vref_mv;
		resolution = channel->channel_cfg.resolution;
	}

	scale_uv = ((uint32_t)vref_mv * 1000000u) / (1u << resolution); /* uV/LSB */
	whole = scale_uv / 1000000u;
	frac = scale_uv % 1000000u;

	return snprintk(dst, len, "%u.%03u", whole, frac) + 1;
}

static int iio_device_io_channels_read_channel_gain(const struct device *dev,
		int index, char *dst, size_t len)
{
	const struct iio_device_io_channels_config *config = dev->config;
	struct iio_device_io_channels_data *data = dev->data;

	if (len < IIO_DEVICE_GAIN_LEN) {
		LOG_ERR("Buffer size %u is too small for gain value, need %u",
			len, IIO_DEVICE_GAIN_LEN);
		return -ENOMEM;
	}

	if (index >= config->num_channels) {
		return -EINVAL;
	}

	if (config->channels[index].type == IO_CHANNEL_TYPE_DAC) {
		if (data->channels[index].dac.adc_channel_idx != IO_CHANNEL_DAC_ADC_CHANNEL_IDX_NONE) {
			return iio_device_io_channels_read_channel_gain(dev, data->channels[index].dac.adc_channel_idx, dst, len);
		}

		LOG_WRN("Ignoring read gain request for non-ADC channel.");
		return -ENOTSUP;
	}

	return snprintk(dst, len, "%s", gain_values[data->overrides[index].adc.gain]) + 1;
}

static int iio_device_io_channels_parse_raw(const char *src, size_t len, uint32_t *raw_value)
{
	// TODO: Configurable max RAW size
	char buffer[32] = { 0 };
	char *endptr = NULL;

	/* Be extra cautious src is properly terminated before calling strtol */

	strncpy(buffer, src, MIN(len, ARRAY_SIZE(buffer)) - 1);
	buffer[31] = '\0';

#ifdef CONFIG_LIBIIO_IIO_DEVICE_FP
	*raw_value = (uint32_t)roundf(strtof(buffer, &endptr));
#else

	*raw_value = (uint32_t)strtol(buffer, &endptr, 10);
#endif

	if (*endptr != '\0') {
		return -EINVAL;
	}

	return 0;
}

static int iio_device_io_channels_write_channel_raw(const struct device *dev,
        int index, const char *src, size_t len)
{
	const struct iio_device_io_channels_config *config = dev->config;
	const struct dac_dt_spec *channel = &config->channels[index].dac;
	uint32_t raw;
	int ret;

	if (config->channels[index].type != IO_CHANNEL_TYPE_DAC) {
		LOG_ERR("RAW write not allowed on inputs");
		return -EINVAL;
	}

	ret = iio_device_io_channels_parse_raw(src, len, &raw);
	if (ret) {
		LOG_ERR("Invalid raw value '%.*s'", (int)len, src);
		return ret;
	}

	ret = dac_write_value_dt(channel, raw);
	if (ret) {
		LOG_ERR("Failed to write raw value '%.*s': %d", (int)len, src, ret);
		return ret;
	}

	return 0;
}

static int iio_device_io_channels_parse_gain(const char *src, size_t len, enum adc_gain *gain_out)
{
	for (size_t i = 0; i < ARRAY_SIZE(gain_values); i++) {
		const char *compare_gain = gain_values[i];

		if (compare_gain == NULL) {
			continue;
		}

		if (strcmp(src, compare_gain) == 0) {
			*gain_out = (enum adc_gain)i;
			return 0;
		}
	}

	return -EINVAL;
}

struct adc_channel_cfg iio_device_io_channels_get_adc_channel_cfg(const struct device *dev, int index)
{
	const struct iio_device_io_channels_config *config = dev->config;
	struct iio_device_io_channels_data *data = dev->data;
	const struct adc_dt_spec *channel = &config->channels[index].adc;

	struct adc_channel_cfg channel_cfg = {0};

	if (channel->channel_cfg_dt_node_exists) {
		channel_cfg.gain = data->overrides[index].adc.gain;
		channel_cfg.acquisition_time = channel->channel_cfg.acquisition_time;
		channel_cfg.differential = data->overrides[index].adc.differential;
		channel_cfg.channel_id = channel->channel_id;
		channel_cfg.reference = data->overrides[index].adc.reference;
	}

	return channel_cfg;
}

static int iio_device_io_channels_write_channel_gain(const struct device *dev,
        int index, const char *src, size_t len)
{
	const struct iio_device_io_channels_config *config = dev->config;
	const struct adc_dt_spec *channel = &config->channels[index].adc;
	struct iio_device_io_channels_data *data = dev->data;

	enum adc_gain gain;
	int ret;

	ret = iio_device_io_channels_parse_gain(src, len, &gain);
	if (ret) {
		LOG_ERR("Invalid gain value '%.*s'", (int)len, src);
		return ret;
	}

	struct adc_channel_cfg channel_cfg = iio_device_io_channels_get_adc_channel_cfg(dev, index);
	channel_cfg.gain = gain;

	ret = adc_channel_setup(channel->dev, &channel_cfg);
	if (ret == 0) {
		data->overrides[index].adc.gain = gain;
		ret = len;
	}

	return ret;
}

static int iio_device_io_channels_int_ref_voltage_read(const struct device *dev,
		char *dst, size_t len)
{
	const struct iio_device_io_channels_config *config = dev->config;
	const struct adc_dt_spec *channel = &config->channels[0].adc;

	if (len < IIO_DEVICE_INT_REF_VOL_LEN) {
		LOG_ERR("Buffer size %u is too small for internal reference voltage value, need %u",
			len, IIO_DEVICE_INT_REF_VOL_LEN);
		return -ENOMEM;
	}

	uint16_t int_ref_mv = adc_ref_internal(channel->dev);

	return snprintk(dst, len, "%u", int_ref_mv) + 1;
}

static int iio_device_io_channels_read_channel_process(const struct device *dev,
		int index, char *dst, size_t len)
{
	struct iio_device_io_channels_data *data = dev->data;
	const struct iio_device_io_channels_config *config = dev->config;
	const struct adc_dt_spec *channel = &config->channels[index].adc;
	uint32_t tmp_buf = 0;
	struct adc_sequence sequence = {
		.buffer = &tmp_buf,
		.buffer_size = sizeof(tmp_buf),
	};
	uint8_t resolution = channel->resolution;
	int ret;

	if (len < IIO_DEVICE_PROCESS_LEN) {
		LOG_ERR("Buffer size %u is too small for process value, need %u",
			len, IIO_DEVICE_PROCESS_LEN);
		return -ENOMEM;
	}

	ret = adc_sequence_init_dt(channel, &sequence);
	if (ret < 0) {
		LOG_ERR("Error initializing adc sequence");
		return ret;
	}

	ret = adc_read_dt(channel, &sequence);
	if (ret < 0) {
		LOG_ERR("Error reading adc");
		return ret;
	}

	if (data->overrides[index].adc.differential) {
		resolution -= 1;
	}

	ret = adc_raw_to_millivolts(channel->vref_mv, data->overrides[index].adc.gain, resolution,
					&tmp_buf);
	if (ret < 0) {
		LOG_ERR("Error converting raw to millivolts");
		return ret;
	}

	return snprintk(dst, len, "%d", tmp_buf) + 1;
}

static int iio_device_io_channels_read_channel_reference(const struct device *dev,
		int index, char *dst, size_t len)
{
	struct iio_device_io_channels_data *data = dev->data;

	if (len < IIO_DEVICE_REF_LEN) {
		LOG_ERR("Buffer size %u is too small for reference value, need %u",
			len, IIO_DEVICE_REF_LEN);
		return -ENOMEM;
	}

	return snprintk(dst, len, "%s", reference_values[data->overrides[index].adc.reference]) + 1;
}

static int iio_device_io_channels_parse_reference(const char *src, size_t len, enum adc_reference *reference_out)
{
	for (size_t i = 0; i < ARRAY_SIZE(reference_values); i++) {
		const char *compare_reference = reference_values[i];

		if (compare_reference == NULL) {
			continue;
		}

		if (strcmp(src, compare_reference) == 0) {
			*reference_out = (enum adc_reference)i;
			return 0;
		}
	}

	return -EINVAL;
}

static int iio_device_io_channels_write_channel_reference(const struct device *dev,
	int index, const char *src, size_t len)
{
	const struct iio_device_io_channels_config *config = dev->config;
	const struct adc_dt_spec *channel = &config->channels[index].adc;
	struct iio_device_io_channels_data *data = dev->data;

	enum adc_reference reference;
	int ret;

	ret = iio_device_io_channels_parse_reference(src, len, &reference);
	if (ret) {
		LOG_ERR("Invalid reference value '%.*s'", (int)len, src);
		return ret;
	}

	struct adc_channel_cfg channel_cfg = iio_device_io_channels_get_adc_channel_cfg(dev, index);
	channel_cfg.reference = reference;

	ret = adc_channel_setup(channel->dev, &channel_cfg);
	if (ret == 0) {
		data->overrides[index].adc.reference = reference;
		ret = len;
	}

	return ret;
}

static int iio_device_io_channels_read_channel_differential(const struct device *dev,
		int index, char *dst, size_t len)
{
	struct iio_device_io_channels_data *data = dev->data;

	if (len < IIO_DEVICE_DIFFERENTIAL_LEN) {
		LOG_ERR("Buffer size %u is too small for differential value, need %u",
			len, IIO_DEVICE_DIFFERENTIAL_LEN);
		return -ENOMEM;
	}

	return snprintk(dst, len, "%d", data->overrides[index].adc.differential) + 1;
}

static int iio_device_io_channels_write_channel_differential(const struct device *dev,
	int index, const char *src, size_t len)
{
	const struct iio_device_io_channels_config *config = dev->config;
	const struct adc_dt_spec *channel = &config->channels[index].adc;
	struct iio_device_io_channels_data *data = dev->data;
	uint8_t differential;
	int ret;

	if (len != 2 || (src[0] != '0' && src[0] != '1')) {
		LOG_ERR("Invalid differential value '%.*s'", (int)len, src);
		return -EINVAL;
	}

	differential = src[0] == '1' ? 1 : 0; /* Convert char '0' or '1' to unsigned integer */
	struct adc_channel_cfg channel_cfg = iio_device_io_channels_get_adc_channel_cfg(dev, index);
	channel_cfg.differential = differential;

	ret = adc_channel_setup(channel->dev, &channel_cfg);
	if (ret == 0) {
		data->overrides[index].adc.differential = differential;
		ret = len;
	}

	return ret;
}

static int iio_device_io_channels_read_attr(const struct device *dev,
		const struct iio_device *iio_device, const struct iio_attr *attr,
		char *dst, size_t len)
{
	const struct iio_device_io_channels_config *config = dev->config;
	int index;

	switch (attr->type) {
	case IIO_ATTR_TYPE_CHANNEL:
		index = (int) iio_channel_get_pdata(attr->iio.chn);

		if (index >= config->num_channels) {
			LOG_ERR("Invalid index: %d", index);
			return -EINVAL;
		}

		if (!strcmp(attr->name, raw_name)) {
			return iio_device_io_channels_read_channel_raw(dev, index, dst, len);
		} else if (!strcmp(attr->name, scale_name)) {
			return iio_device_io_channels_read_channel_scale(dev, index, dst, len);
		} else if (!strcmp(attr->name, gain_name)) {
			return iio_device_io_channels_read_channel_gain(dev, index, dst, len);
		} else if (!strcmp(attr->name, process_name)) {
			return iio_device_io_channels_read_channel_process(dev, index, dst, len);
		} else if (!strcmp(attr->name, reference_name)) {
			return iio_device_io_channels_read_channel_reference(dev, index, dst, len);
		} else if (!strcmp(attr->name, differential_name)) {
			return iio_device_io_channels_read_channel_differential(dev, index, dst, len);
		}
		break;

	case IIO_ATTR_TYPE_DEVICE:
		if (!strcmp(attr->name, internal_ref_voltage_name)) {
			return iio_device_io_channels_int_ref_voltage_read(dev, dst, len);
		}
		break;
	case IIO_ATTR_TYPE_BUFFER:
		dst[0] = '\0';
		return 1;
	default:
		break;
	}

	LOG_ERR("Invalid attr");
	return -EINVAL;
}

static int iio_device_io_channels_write_attr(const struct device *dev,
		const struct iio_device *iio_device, const struct iio_attr *attr,
		const char *src, size_t len)
{
	const struct iio_device_io_channels_config *config = dev->config;
	int index = 0;

	switch (attr->type) {
	case IIO_ATTR_TYPE_CHANNEL:
		index = (int) iio_channel_get_pdata(attr->iio.chn);

		if (index >= config->num_channels) {
			LOG_ERR("Invalid index: %d", index);
			return -EINVAL;
		}
		if (!strcmp(attr->name, raw_name)) {
			return iio_device_io_channels_write_channel_raw(dev, index, src, len);
		} else if (!strcmp(attr->name, gain_name)) {
			return iio_device_io_channels_write_channel_gain(dev, index, src, len);
		} else if (!strcmp(attr->name, reference_name)) {
			return iio_device_io_channels_write_channel_reference(dev, index, src, len);
		} else if (!strcmp(attr->name, differential_name)) {
			return iio_device_io_channels_write_channel_differential(dev, index, src, len);
		}
		break;

	case IIO_ATTR_TYPE_DEVICE:
		break;

	default:
		break;
	}

	LOG_ERR("Invalid attr");
	return -EINVAL;
}

static int iio_device_io_channels_init(const struct device *dev)
{
	const struct iio_device_io_channels_config *config = dev->config;
	struct iio_device_io_channels_data *data = dev->data;
	int ret = 0;

	for (size_t i = 0; i < config->num_channels; i++) {
		if (config->channels[i].type == IO_CHANNEL_TYPE_ADC) {
			data->overrides[i].adc.gain = config->channels[i].adc.channel_cfg.gain;
			data->overrides[i].adc.reference = config->channels[i].adc.channel_cfg.reference;
			data->overrides[i].adc.differential = config->channels[i].adc.channel_cfg.differential;
			ret = adc_channel_setup_dt(&config->channels[i].adc);

			if (ret < 0) {
				LOG_ERR("Error setting up channel %zu", i);
				break;
			}
		} else {
			/* Find the matching ADC channel, if present */
			data->channels[i].dac.adc_channel_idx = IO_CHANNEL_DAC_ADC_CHANNEL_IDX_NONE;
			for (size_t j = 0; j < config->num_channels; j++) {
				if (config->channels[j].type == IO_CHANNEL_TYPE_ADC && !strcmp(config->channels[i].name, config->channels[j].name)) {
					data->channels[i].dac.adc_channel_idx = j;
					break;
				}
			}

			ret = dac_channel_setup_dt(&config->channels[i].dac);
			if (ret < 0) {
				LOG_ERR("Error setting up channel %zu", i);
				break;
			}
		}
	}

	return ret;
}

static const char *iio_device_io_channels_get_buffer_name(const struct device *dev)
{
	const struct iio_device_io_channels_config *config = dev->config;

	return config->buffer_name;
}

static int iio_device_io_channels_add_trigger(struct iio_context *ctx, struct iio_device *iio_device)
{
	const struct device *dev = (const struct device *)iio_device_get_pdata(iio_device);
	const struct iio_device_io_channels_config *config = dev->config;
	const struct device *trigger_dev = config->trigger_dev;

	if (trigger_dev == NULL) {
		return -ENODEV;
	}
	
	const struct iio_device_trigger_config *trigger_config = trigger_dev->config;
	struct iio_device *trig;
	int ret = 0;

	trig = trigger_config->ops->create(ctx, trigger_dev, trigger_config->trigger_type, trigger_config->trigger_id);
	if (!trig) {
		LOG_ERR("Could not add trigger device.");
		return -ENOMEM;
	}

	ret = iio_device_set_trigger(iio_device, trig);
	if (ret < 0) {
		LOG_ERR("Could not set trigger.");
		return ret;
	}

	trigger_config->ops->init(trigger_dev);

	return ret;
}

#ifdef CONFIG_ADC_STREAM

static ssize_t iio_device_io_channels_readbuf_stream(const struct device *dev,
		const struct iio_device *iio_dev,
		const struct iio_channels_mask *mask,
		void *dst, size_t len)
{
	const struct iio_device_io_channels_config *config = dev->config;
	struct iio_device_io_channels_data *data = dev->data;
	unsigned int nb_channels = iio_device_get_channels_count(iio_dev);
	uint8_t *out = (uint8_t *)dst;
	int ret;

	/* Lazy-start streaming on first call */
	if (!data->streaming_active) {
		/* Drain any stale CQEs from a previous session */
		struct rtio_cqe *stale;

		while ((stale = rtio_cqe_consume(data->stream_ctx)) != NULL) {
			uint8_t *sbuf;
			uint32_t slen;

			if (rtio_cqe_get_mempool_buffer(data->stream_ctx,
							stale, &sbuf, &slen) == 0)
				rtio_release_buffer(data->stream_ctx, sbuf, slen);
			rtio_cqe_release(data->stream_ctx, stale);
		}

		/* Init sequence with all ADC channels */
		const struct iio_device_io_channels_channel *first_adc = NULL;

		for (size_t i = 0; i < config->num_channels; i++) {
			if (config->channels[i].type == IO_CHANNEL_TYPE_ADC) {
				if (!first_adc) {
					first_adc = &config->channels[i];
					ret = adc_sequence_init_dt(&first_adc->adc,
								   &data->stream_seq);
					if (ret < 0)
						return ret;
				} else {
					data->stream_seq.channels |=
						BIT(config->channels[i].adc.channel_id);
				}
			}
		}

		if (!first_adc)
			return -EINVAL;

		struct adc_read_config *cfg = data->stream_iodev->data;

		cfg->sequence = &data->stream_seq;

		ret = adc_get_decoder(first_adc->adc.dev, &data->decoder);
		if (ret < 0)
			return ret;

		ret = adc_stream(data->stream_iodev, data->stream_ctx,
				 NULL, NULL);
		if (ret < 0) {
			LOG_ERR("adc_stream failed: %d", ret);
			return ret;
		}

		LOG_INF("ADC streaming started");
		data->streaming_active = true;
	}

	/* Determine output format from first enabled channel */
	const struct iio_data_format *fmt = NULL;
	size_t bytes_per_channel = 0;
	unsigned int enabled_count = 0;

	for (unsigned int i = 0; i < nb_channels; i++) {
		const struct iio_channel *chn =
			iio_device_get_channel(iio_dev, i);

		if (!iio_channel_is_scan_element(chn) ||
		    !iio_channel_is_enabled(chn, mask))
			continue;

		if (!fmt) {
			fmt = iio_channel_get_data_format(chn);
			bytes_per_channel = DIV_ROUND_UP(fmt->length,
							 BITS_PER_BYTE);
		}
		enabled_count++;
	}

	if (!enabled_count || !fmt)
		return -EINVAL;

	size_t bytes_per_sample = enabled_count * bytes_per_channel;
	size_t total_samples = len / bytes_per_sample;

	if (total_samples == 0)
		return -EINVAL;

	size_t samples_written = 0;

	while (samples_written < total_samples) {
		struct rtio_cqe *cqe;
		uint8_t *buf;
		uint32_t buf_len;
		uint16_t frame_count;

		cqe = rtio_cqe_consume(data->stream_ctx);
		if (!cqe) {
			/* Yield to let RTIO/ADC ISR produce CQEs.
			 * Safe: readbuf runs on the dedicated trigger
			 * work queue, not the system one. */
			k_yield();
			cqe = rtio_cqe_consume(data->stream_ctx);
			if (!cqe) {
				if (samples_written > 0)
					break;
				return -ETIMEDOUT;
			}
		}
		if (cqe->result != 0) {
			rtio_cqe_release(data->stream_ctx, cqe);
			if (samples_written > 0)
				break;
			return -EIO;
		}

		ret = rtio_cqe_get_mempool_buffer(data->stream_ctx, cqe,
						  &buf, &buf_len);
		rtio_cqe_release(data->stream_ctx, cqe);
		if (ret != 0) {
			if (samples_written > 0)
				break;
			return ret;
		}

		/* Decode frames from this CQE using the portable decoder API */
		ret = data->decoder->get_frame_count(buf, 0, &frame_count);
		if (ret != 0 || frame_count == 0) {
			rtio_release_buffer(data->stream_ctx, buf, buf_len);
			continue;
		}

		uint32_t fit = 0;
		struct adc_data adc_data = {0};

		for (uint16_t f = 0;
		     f < frame_count && samples_written < total_samples;
		     f++) {
			ret = data->decoder->decode(buf, 0, &fit, 1, &adc_data);
			if (ret <= 0)
				break;

			/* Convert Q31 back to raw value.
			 * Q31 value = raw * 2^(31-shift) / full_scale
			 * raw = value >> (31 - resolution)
			 * The shift from decoder tells us the scaling. */
			int32_t raw = adc_data.readings[0].value;

			if (adc_data.shift > 0)
				raw >>= adc_data.shift;

			raw <<= fmt->shift;

			uint8_t *dest = out + samples_written * bytes_per_sample;

			if (fmt->is_be) {
				for (int b = bytes_per_channel - 1; b >= 0; b--)
					dest[bytes_per_channel - 1 - b] =
						(uint8_t)((raw >> (b * 8)) & 0xFF);
			} else {
				for (size_t b = 0; b < bytes_per_channel; b++)
					dest[b] =
						(uint8_t)((raw >> (b * 8)) & 0xFF);
			}

			samples_written++;
		}

		rtio_release_buffer(data->stream_ctx, buf, buf_len);
	}

	return (ssize_t)(samples_written * bytes_per_sample);
}

#endif /* CONFIG_ADC_STREAM */

#define READBUF_CHUNK_SAMPLES 256
#define READBUF_MAX_ENABLED_CHANNELS 8

struct readbuf_channel_info {
	int config_index;
	const struct adc_dt_spec *adc_spec;
	const struct iio_data_format *fmt;
	size_t bytes_per_channel;
	size_t byte_offset;
};

static ssize_t iio_device_io_channels_readbuf(const struct device *dev,
		const struct iio_device *iio_dev,
		const struct iio_channels_mask *mask,
		void *dst, size_t len)
{
#ifdef CONFIG_ADC_STREAM
	struct iio_device_io_channels_data *sdata = dev->data;

	if (sdata->stream_ctx)
		return iio_device_io_channels_readbuf_stream(dev, iio_dev,
							     mask, dst, len);
#endif

	const struct iio_device_io_channels_config *config = dev->config;
	struct iio_device_io_channels_data *data = dev->data;
	unsigned int nb_channels = iio_device_get_channels_count(iio_dev);
	struct readbuf_channel_info ch_info[READBUF_MAX_ENABLED_CHANNELS];
	unsigned int enabled_count = 0;
	size_t bytes_per_sample = 0;

	for (unsigned int ch_idx = 0;
	     ch_idx < nb_channels && enabled_count < READBUF_MAX_ENABLED_CHANNELS;
	     ch_idx++) {
		const struct iio_channel *chn =
			iio_device_get_channel(iio_dev, ch_idx);

		if (!iio_channel_is_scan_element(chn) ||
		    !iio_channel_is_enabled(chn, mask))
			continue;

		int cfg_idx = (int)(intptr_t)iio_channel_get_pdata(chn);

		if (cfg_idx < 0 || cfg_idx >= config->num_channels)
			return -EINVAL;

		struct readbuf_channel_info *ci = &ch_info[enabled_count];
		int adc_idx;

		if (config->channels[cfg_idx].type == IO_CHANNEL_TYPE_DAC) {
			adc_idx = data->channels[cfg_idx].dac.adc_channel_idx;
			if (adc_idx == IO_CHANNEL_DAC_ADC_CHANNEL_IDX_NONE)
				return -ENODEV;
		} else {
			adc_idx = cfg_idx;
		}

		ci->config_index = cfg_idx;
		ci->adc_spec = &config->channels[adc_idx].adc;
		ci->fmt = iio_channel_get_data_format(chn);
		ci->bytes_per_channel = DIV_ROUND_UP(ci->fmt->length,
						     BITS_PER_BYTE);
		ci->byte_offset = bytes_per_sample;
		bytes_per_sample += ci->bytes_per_channel;
		enabled_count++;
	}

	if (enabled_count == 0 || bytes_per_sample == 0)
		return -EINVAL;

	size_t total_samples = len / bytes_per_sample;

	if (total_samples == 0)
		return -EINVAL;

	uint8_t *out = (uint8_t *)dst;

	for (unsigned int ch = 0; ch < enabled_count; ch++) {
		struct readbuf_channel_info *ci = &ch_info[ch];
		size_t adc_sample_size = (ci->adc_spec->resolution <= 16)
			? sizeof(uint16_t) : sizeof(uint32_t);
		size_t samples_done = 0;

		while (samples_done < total_samples) {
			size_t chunk = MIN(READBUF_CHUNK_SAMPLES,
					   total_samples - samples_done);
			uint8_t adc_buf[READBUF_CHUNK_SAMPLES * sizeof(uint32_t)];
			int ret;

			struct adc_sequence_options opts = {
				.interval_us = 0,
				.callback = NULL,
				.user_data = NULL,
				.extra_samplings = (chunk > 1)
					? (uint16_t)(chunk - 1) : 0,
			};
			struct adc_sequence sequence = {
				.options = (chunk > 1) ? &opts : NULL,
				.buffer = adc_buf,
				.buffer_size = chunk * adc_sample_size,
			};

			ret = adc_sequence_init_dt(ci->adc_spec, &sequence);
			if (ret < 0)
				return ret;

			ret = adc_read_dt(ci->adc_spec, &sequence);
			if (ret < 0)
				return ret;

			for (size_t s = 0; s < chunk; s++) {
				int32_t raw;

				if (adc_sample_size == sizeof(uint16_t))
					raw = (int32_t)((uint16_t *)adc_buf)[s];
				else
					raw = (int32_t)((uint32_t *)adc_buf)[s];

				raw <<= ci->fmt->shift;

				uint8_t *dest = out
					+ (samples_done + s) * bytes_per_sample
					+ ci->byte_offset;
				size_t bpc = ci->bytes_per_channel;

				if (ci->fmt->is_be) {
					for (int b = bpc - 1; b >= 0; b--)
						dest[bpc - 1 - b] =
							(uint8_t)((raw >> (b * 8)) & 0xFF);
				} else {
					for (size_t b = 0; b < bpc; b++)
						dest[b] =
							(uint8_t)((raw >> (b * 8)) & 0xFF);
				}
			}

			samples_done += chunk;
		}
	}

	return (ssize_t)(total_samples * bytes_per_sample);
}

#ifdef CONFIG_ADC_STREAM
static void iio_device_io_channels_stop_streaming(const struct device *dev)
{
	struct iio_device_io_channels_data *data = dev->data;

	if (!data->stream_ctx)
		return;

	/* Drain unconsumed CQEs to free mempool blocks.
	 * Do NOT clear streaming_active — the stream keeps running.
	 * adc_stream() must only be called once per boot. */
	struct rtio_cqe *cqe;

	while ((cqe = rtio_cqe_consume(data->stream_ctx)) != NULL) {
		uint8_t *buf;
		uint32_t buf_len;

		if (rtio_cqe_get_mempool_buffer(data->stream_ctx,
						cqe, &buf, &buf_len) == 0)
			rtio_release_buffer(data->stream_ctx, buf, buf_len);
		rtio_cqe_release(data->stream_ctx, cqe);
	}
}
#endif

static DEVICE_API(iio_device, iio_device_io_channels_driver_api) = {
	.add_channels = iio_device_io_channels_add_channels,
	.read_attr = iio_device_io_channels_read_attr,
	.write_attr = iio_device_io_channels_write_attr,
	.get_buffer_name = iio_device_io_channels_get_buffer_name,
	.add_trigger = iio_device_io_channels_add_trigger,
	.readbuf = iio_device_io_channels_readbuf,
#ifdef CONFIG_ADC_STREAM
	.stop_streaming = iio_device_io_channels_stop_streaming,
#endif
};

#define DT_DRV_COMPAT iio_io_channels

#define IIO_DEVICE_IO_CHANNEL(node_id, prop, idx)					\
        COND_CODE_1(DT_PHA_HAS_CELL_AT_IDX(node_id, prop, idx, input), \
                    ({ .type = IO_CHANNEL_TYPE_ADC, .name = DT_PHA_ELEM_NAME_BY_IDX(node_id, prop, idx), .adc = ADC_DT_SPEC_GET_BY_IDX(node_id, idx) }), ({.name = DT_PHA_ELEM_NAME_BY_IDX(node_id, prop, idx), .type = IO_CHANNEL_TYPE_DAC, .dac = DAC_DT_SPEC_GET_BY_IDX(node_id, idx)}))

/* Extract only ADC dt specs for streaming (filter out DAC) */
#ifdef CONFIG_ADC_STREAM
#define IIO_ADC_STREAM_SPEC(node_id, prop, idx)						\
        COND_CODE_1(DT_PHA_HAS_CELL_AT_IDX(node_id, prop, idx, input),			\
                    (ADC_DT_SPEC_GET_BY_IDX(node_id, idx),), ())

#define IIO_DEVICE_IO_CHANNELS_STREAM_INIT(inst)					\
static const struct adc_dt_spec iio_adc_stream_specs_##inst[] = {			\
	DT_INST_FOREACH_PROP_ELEM(inst, io_channels, IIO_ADC_STREAM_SPEC)		\
};											\
ADC_DT_STREAM_IODEV(iio_adc_stream_iodev_##inst,					\
	DT_PHANDLE_BY_IDX(DT_DRV_INST(inst), io_channels, 0),				\
	iio_adc_stream_specs_##inst,							\
	{ADC_TRIG_FIFO_FULL, ADC_STREAM_DATA_INCLUDE});					\
RTIO_DEFINE_WITH_MEMPOOL(iio_adc_stream_ctx_##inst, 4, 4, 32, 64, sizeof(void *));
#else
#define IIO_DEVICE_IO_CHANNELS_STREAM_INIT(inst)
#endif

#define IIO_DEVICE_IO_CHANNELS_INIT(inst)							\
IIO_DEVICE_IO_CHANNELS_STREAM_INIT(inst)						\
static struct iio_device_io_channels_channel_overrides iio_device_io_channel_overrides_##inst[DT_INST_PROP_LEN(inst, io_channels)];	\
static struct iio_device_io_channels_channel_data iio_device_io_channel_channel_data_##inst[DT_INST_PROP_LEN(inst, io_channels)];	\
static struct iio_device_io_channels_data iio_device_io_channel_data_##inst = {			\
	.overrides = iio_device_io_channel_overrides_##inst,					\
	.channels = iio_device_io_channel_channel_data_##inst,					\
	IF_ENABLED(CONFIG_ADC_STREAM, (							\
		.stream_ctx = &iio_adc_stream_ctx_##inst,					\
		.stream_iodev = &iio_adc_stream_iodev_##inst,					\
	))											\
};												\
												\
static const struct iio_device_io_channels_channel iio_device_io_channels_##inst[] = {		\
	DT_INST_FOREACH_PROP_ELEM_SEP(inst, io_channels, IIO_DEVICE_IO_CHANNEL, (,))		\
};												\
												\
static const struct iio_device_io_channels_config iio_device_io_channel_config_##inst = {	\
	.address = DT_INST_REG_ADDR(inst),							\
	.channels = iio_device_io_channels_##inst,						\
	.num_channels = ARRAY_SIZE(iio_device_io_channels_##inst),				\
	.buffer_name = DT_INST_PROP_OR(inst, buffer_name, "buffer"),			\
	.trigger_dev = COND_CODE_1(DT_INST_NODE_HAS_PROP(inst, trigger_device),		\
		(DEVICE_DT_GET(DT_INST_PHANDLE(inst, trigger_device))),			\
		(NULL)),								\
};												\
												\
IIO_DEVICE_DT_INST_DEFINE(inst, DT_INST_PROP_OR(inst, io_name, NULL),				\
	iio_device_io_channels_init, NULL,							\
	&iio_device_io_channel_data_##inst, &iio_device_io_channel_config_##inst,		\
	POST_KERNEL, CONFIG_LIBIIO_IIO_DEVICE_IO_CHANNELS_INIT_PRIORITY,			\
	&iio_device_io_channels_driver_api);

DT_INST_FOREACH_STATUS_OKAY(IIO_DEVICE_IO_CHANNELS_INIT)
