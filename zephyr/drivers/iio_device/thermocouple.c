/*
 * Copyright (c) 2016 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/logging/log.h>
#include <iio/iio-backend.h>
#include <iio_device.h>

#include <math.h>

LOG_MODULE_REGISTER(iio_device_thermocouple, CONFIG_LIBIIO_LOG_LEVEL);

#define NUM_VIRTUAL_CHANNELS	4

struct iio_device_thermocouple_config {
	const char *name;
	struct adc_dt_spec *channels;
	size_t num_channels;
};

struct iio_device_thermocouple_data {
	double hot_junction_temp[NUM_VIRTUAL_CHANNELS];
	struct k_thread sampling_thread;
	k_thread_stack_t *sampling_stack;
};

static const char* virtual_channel_ids[NUM_VIRTUAL_CHANNELS] = {"voltage3", "voltage2", "voltage1", "voltage0"};
static const char* virtual_channel_names[NUM_VIRTUAL_CHANNELS] = {"CH3", "CH2", "CH1", "CH0"};

static const char *const raw_name = "raw";
static const char *const scale_name = "scale";

#define ADC_RESOLUTION 24
#define ADC_HALF_RESOLUTION (1 << (ADC_RESOLUTION - 1))
#define GAIN 32
#define RTD_GAIN 1
#define THERMO_RES 1600
#define ADC_REF_VOLTAGE 2.5
#define THERMOCOUPLE_CHANNEL 0
#define REFERENCE_RES_CHANNEL 2
#define COLD_JUNCTION_CHANNEL 1

#define CHANNEL_COUNT	3
#define ROUNDS			1
#define TCP_SERVER_PORT 8080

#define SAMPLING_PERIOD_MS 5
#define SAMPLING_THREAD_PRIORITY 7
#define SAMPLING_THREAD_STACK_SIZE 2048

static uint32_t readings[NUM_VIRTUAL_CHANNELS][ROUNDS * CHANNEL_COUNT];

static K_MUTEX_DEFINE(adc_mutex);
static K_MUTEX_DEFINE(data_mutex);

static double typeK_temp_to_voltage(double temp_celsius)
{
	// ITS-90 Type K polynomial coefficients for 0°C to 1372°C
	const double c0 = 0.000000E+00;
	const double c1 = 3.945013E-02;
	const double c2 = 2.362237E-05;
	const double c3 = -3.285891E-07;
	const double c4 = -4.990483E-09;
	const double c5 = -6.750905E-11;
	const double c6 = -5.741032E-13;
	const double c7 = -3.108887E-15;
	const double c8 = -1.045160E-17;
	const double c9 = -1.988926E-20;
	const double c10 = -1.632269E-23;

	double t = temp_celsius;
	double voltage = c0 + c1*t + c2*t*t + c3*t*t*t + c4*pow(t,4) +
	                 c5*pow(t,5) + c6*pow(t,6) + c7*pow(t,7) +
	                 c8*pow(t,8) + c9*pow(t,9) + c10*pow(t,10);

	return voltage;
}

static double typeK_voltage_to_temp(double voltage_mv)
{
	// ITS-90 Type K inverse polynomial for -5.891 mV to 0 mV (-200°C to 0°C)
	const double d0 = 0.0000000E+00;
	const double d1 = 2.5173462E+01;
	const double d2 = -1.1662878E+00;
	const double d3 = -1.0833638E+00;
	const double d4 = -8.9773540E-01;
	const double d5 = -3.7342377E-01;
	const double d6 = -8.6632643E-02;
	const double d7 = -1.0450598E-02;
	const double d8 = -5.1920577E-04;

	// ITS-90 Type K inverse polynomial for 0 mV to 20.644 mV (0°C to 500°C)
	const double e0 = 0.000000E+00;
	const double e1 = 2.508355E+01;
	const double e2 = 7.860106E-02;
	const double e3 = -2.503131E-01;
	const double e4 = 8.315270E-02;
	const double e5 = -1.228034E-02;
	const double e6 = 9.804036E-04;
	const double e7 = -4.413030E-05;
	const double e8 = 1.057734E-06;
	const double e9 = -1.052755E-08;

	double temp;

	if (voltage_mv < 0) {
		double v = voltage_mv;
		temp = d0 + d1*v + d2*v*v + d3*pow(v,3) + d4*pow(v,4) +
		       d5*pow(v,5) + d6*pow(v,6) + d7*pow(v,7) + d8*pow(v,8);
	} else {
		double v = voltage_mv;
		temp = e0 + e1*v + e2*v*v + e3*pow(v,3) + e4*pow(v,4) +
		       e5*pow(v,5) + e6*pow(v,6) + e7*pow(v,7) + e8*pow(v,8) + e9*pow(v,9);
	}

	return temp;
}

static double resistance_to_temperature(double resistance)
{
	// Callendar-Van Dusen equation for Pt1000 RTD (IEC 60751)
	// For T ≥ 0°C: R(T) = R0(1 + A*T + B*T²)
	// For T < 0°C: R(T) = R0[1 + A*T + B*T² + C(T-100)T³]

	const double R0 = 1000.0;          // Pt1000: 1000Ω at 0°C
	const double A = 3.9083e-3;        // °C⁻¹
	const double B = -5.775e-7;        // °C⁻²
	const double C = -4.183e-12;       // °C⁻⁴ (for T < 0°C)

	// Solve: B*T² + A*T + (1 - R/R0) = 0
	double ratio = resistance / R0;
	double c_coef = 1.0 - ratio;
	double discriminant = A * A - 4.0 * B * c_coef;

	if (discriminant < 0) {
		return -999.0f;
	}

	double T = (-A + sqrt(discriminant)) / (2.0 * B);

	if (T < 0.0) {
		for (int i = 0; i < 5; i++) {
			double R_calc = R0 * (1.0 + A*T + B*T*T + C*(T-100.0)*T*T*T);
			double dR_dT = R0 * (A + 2.0*B*T + C*(4.0*T*T*T - 300.0*T*T));
			T = T - (R_calc - resistance) / dR_dT;
		}
	}

	return (double)T;
}

int32_t read_channel_average(int channel, const uint32_t *readings)
{
	int32_t adc_code = 0;
	for (uint8_t r = 0U; r < ROUNDS; r++)
		adc_code += readings[r * CHANNEL_COUNT + channel];

	adc_code /= ROUNDS;

	return adc_code;
}

int read_temperature(const struct device *dev, const struct adc_sequence *seq, int index)
{
	double thermo_voltage;
	double rtd_resistance;
	int ret;
	struct iio_device_thermocouple_data *data = dev->data;
	const struct iio_device_thermocouple_config *config = dev->config;
	int32_t reference_reading, thermo_reading, cold_junction_reading;
	double rtd_voltage_r5, current, rtd_voltage_r1, cold_junction_temp, v_cold_junction, v_total, hot_junction_temp;

	reference_reading = read_channel_average(REFERENCE_RES_CHANNEL, readings[index]);
	rtd_voltage_r5 = (ADC_REF_VOLTAGE) * 
						(double)(reference_reading - ADC_HALF_RESOLUTION) / 
						(ADC_HALF_RESOLUTION * RTD_GAIN);

	current = rtd_voltage_r5 / THERMO_RES;

	thermo_reading = read_channel_average(THERMOCOUPLE_CHANNEL, readings[index]);
	thermo_voltage = (ADC_REF_VOLTAGE) * 
						(double)(thermo_reading - ADC_HALF_RESOLUTION) / 
						(ADC_HALF_RESOLUTION * GAIN) * 1000; // [mV]

	cold_junction_reading = read_channel_average(COLD_JUNCTION_CHANNEL, readings[index]);
	rtd_voltage_r1 = (ADC_REF_VOLTAGE) * 
						(double)(cold_junction_reading - ADC_HALF_RESOLUTION) / 
						(ADC_HALF_RESOLUTION * RTD_GAIN);

	rtd_resistance = rtd_voltage_r1 / current;

	cold_junction_temp = resistance_to_temperature(rtd_resistance);
	v_cold_junction = typeK_temp_to_voltage((double)cold_junction_temp);
	v_total = thermo_voltage + v_cold_junction;
	hot_junction_temp = typeK_voltage_to_temp(v_total);

	k_mutex_lock(&data_mutex, K_FOREVER);
	data->hot_junction_temp[index] = hot_junction_temp;
	k_mutex_unlock(&data_mutex);
	
	return 0;
}

#define MASK(index)		BIT(config->channels[config->num_channels - 1].channel_id)	| \
						BIT(config->channels[index * 2].channel_id) | \
						BIT(config->channels[index * 2 + 1].channel_id);

static void sampling_thread_worker(void *arg1, void *arg2, void* arg3)
{
	const struct device *dev = (const struct device *)arg1;
	const struct iio_device_thermocouple_config *config = dev->config;
	const struct device *adc_dev = config->channels[0].dev;
	int rtd_channel_idx;
	int ret;
	static const struct adc_sequence_options opts = {
		.interval_us = 0,
		.extra_samplings = ROUNDS - 1,
	};
	struct adc_sequence seq;
	seq.resolution = ADC_RESOLUTION;
	seq.oversampling = 0;
	seq.buffer_size = sizeof(uint32_t) * ROUNDS * CHANNEL_COUNT;
	seq.options = &opts;	

	while (1) {
		for (int index = 0; index < NUM_VIRTUAL_CHANNELS; index++) {
			if (k_mutex_lock(&adc_mutex, K_NO_WAIT) == 0) {
				seq.buffer = readings[index];
				rtd_channel_idx = index * 2 + 1;
				ret = adc_channel_setup_dt(&config->channels[rtd_channel_idx]);
				if (ret < 0) {
					LOG_ERR("Failed to setup RTD channel %d (%d)", rtd_channel_idx, ret);
					k_mutex_unlock(&adc_mutex);
					continue;
				}
				seq.channels = MASK(index);

				ret = adc_read(adc_dev, &seq);
				k_mutex_unlock(&adc_mutex);
				if (ret) {
					LOG_ERR("ADC read failed with code %d\n", ret);
					continue;
				}
				ret = read_temperature(dev, &seq, index);
				if (ret < 0) {
					LOG_ERR("Read temperature FAILED for channel %s", virtual_channel_ids[index]);
				}
			}
			k_sleep(K_MSEC(1)); // i think this can be removed
		}
	}
}

static int iio_device_thermocouple_init(const struct device *dev)
{
	const struct iio_device_thermocouple_config *config = dev->config;
	struct iio_device_thermocouple_data *data = dev->data;
	int ret;

	for (int i = 0; i < config->num_channels; i++) {
		if ((i % 2 == 1) && (i < config->num_channels - 1)) {
			continue;
		}

		ret = adc_channel_setup_dt(&config->channels[i]);
		if (ret < 0) {
			LOG_ERR("Could not setup channel %d (%d)", i, ret);
			return ret;
		}
	}

	for (int i = 0; i < NUM_VIRTUAL_CHANNELS; i++) {
		data->hot_junction_temp[i] = 0.0;
	}

	k_thread_create(&data->sampling_thread, 
		data->sampling_stack, SAMPLING_THREAD_STACK_SIZE,
		sampling_thread_worker, (void*)dev, NULL, NULL,
		SAMPLING_THREAD_PRIORITY, 0, K_SECONDS(10));
	k_thread_name_set(&data->sampling_thread, "thermo_sampling");

	return 0;
}

static int iio_device_thermocouple_add_channels(const struct device *dev,
  struct iio_device *iio_device)
{
	struct iio_channel *iio_channel;

	for (int i = 0; i < NUM_VIRTUAL_CHANNELS; i++)
	{
		int index = i;
		const struct iio_data_format fmt = {
			.length = 32,
			.bits = 32,
			.is_signed = true,
		};

		iio_channel = iio_device_add_channel(iio_device, index, virtual_channel_ids[i],
      					virtual_channel_names[i], NULL, false, false, &fmt);

		if (iio_err(iio_channel)) {
			LOG_ERR("Could not add channel %d", index);
			return -EINVAL;
		}

		iio_channel_set_pdata(iio_channel, (struct iio_channel_pdata *) index);

		if (iio_channel_add_attr(iio_channel, raw_name, NULL)) {
			LOG_ERR("Could not add channel %d attribute %s", index, raw_name);
			return -EINVAL;
		}
		if (iio_channel_add_attr(iio_channel, scale_name, NULL)) {
			LOG_ERR("Could not add channel %d attribute %s", index, scale_name);
			return -EINVAL;
		}
	}
	return 0;
}

static int iio_device_adc_read_channel_scale(const struct device *dev,
  int index, char *dst, size_t len)
{
	static char scale_value[10] = "1000";
	int scale_len = strlen(scale_value) + 1;

	if (len < scale_len) {
		LOG_ERR("Buffer size %u is too small for scale value, need %u",
		len, scale_len);
		return -ENOMEM;
	}

	strcpy(dst, scale_value);

	return scale_len;
}

static int iio_device_adc_read_channel_raw(const struct device *dev,
  int index, char *dst, size_t len)
{
	struct iio_device_thermocouple_data *data = dev->data;
	static char raw_value[15];
	int raw_len;

	k_mutex_lock(&data_mutex, K_FOREVER);
	double this_temp = data->hot_junction_temp[index];
	k_mutex_unlock(&data_mutex);

	sprintf(raw_value, "%.5lf", this_temp);
	raw_len = strlen(raw_value) + 1;

	if (len < raw_len) {
		LOG_ERR("Buffer size %u is too small for scale value, need %u",
		len, raw_len);
		return -ENOMEM;
	}

	strcpy(dst, raw_value);
	return raw_len;
}

static int iio_device_thermocouple_read_attr(const struct device *dev,
  const struct iio_device *iio_device, const struct iio_attr *attr,
  char *dst, size_t len)
{
	int index = (int) iio_channel_get_pdata(attr->iio.chn);

	if (index >= NUM_VIRTUAL_CHANNELS) {
		LOG_ERR("Invalid index: %d", index);
		return -EINVAL;
	}

	if (!strcmp(attr->name, raw_name)) {
		return iio_device_adc_read_channel_raw(dev, index, dst, len);
	} else if (!strcmp(attr->name, scale_name)) {
		return iio_device_adc_read_channel_scale(dev, index, dst, len);
	}

 	LOG_ERR("Invalid attr");
 	return -EINVAL;
}

static DEVICE_API(iio_device, iio_device_thermocouple_driver_api) = {
	.add_channels = iio_device_thermocouple_add_channels,
	.read_attr = iio_device_thermocouple_read_attr,
	.write_attr = NULL,
};

#define DT_DRV_COMPAT iio_thermocouple

#define IIO_DEVICE_THERMOCOUPLE_CHANNEL(node_id, prop, idx)     \
	ADC_DT_SPEC_GET_BY_IDX(node_id, idx)

#define IIO_DEVICE_THERMOCOUPLE_INIT(inst)	\
K_THREAD_STACK_DEFINE(thermocouple_sampling_stack_##inst, SAMPLING_THREAD_STACK_SIZE)	\
static struct iio_device_thermocouple_data thermocouple_data_##inst = {	\
	.sampling_stack = thermocouple_sampling_stack_##inst,	\
};	\
		\
static struct adc_dt_spec iio_device_thermocouple_channels_##inst[] = {    \
	DT_INST_FOREACH_PROP_ELEM_SEP(inst, io_channels, IIO_DEVICE_THERMOCOUPLE_CHANNEL, (,)) \
};	\
static const struct iio_device_thermocouple_config thermocouple_config_##inst = {	\
	.name = DT_INST_PROP_OR(inst, io_name, NULL),	\
	.channels = iio_device_thermocouple_channels_##inst,     \
	.num_channels = ARRAY_SIZE(iio_device_thermocouple_channels_##inst),	\
};		\
\
IIO_DEVICE_DT_INST_DEFINE(inst, iio_device_thermocouple_init, NULL, \
    &thermocouple_data_##inst, &thermocouple_config_##inst, \
    POST_KERNEL, CONFIG_LIBIIO_IIO_DEVICE_THERMOCOUPLE_INIT_PRIORITY, \
    &iio_device_thermocouple_driver_api);

DT_INST_FOREACH_STATUS_OKAY(IIO_DEVICE_THERMOCOUPLE_INIT)