/*
 * Copyright (c) 2026 Analog Devices, Inc.
 * SPDX-License-Identifier: MIT
 *
 * libiio bulk-capture provider for the AD463x family.
 *
 * This is the only file that couples libiio to the ad463x driver. The driver
 * itself (drivers/adc/adc_ad463x.c) exposes ad463x_read_buffer() and
 * ad463x_get_frame_size() as its stable bulk-capture interface. This provider
 * registers them into the IIO_BULK_CAPTURE_PROVIDER linker section so
 * io_channels.c can find them at runtime without naming the driver concretely.
 */

#include <zephyr/drivers/misc/adi/ad463x.h>
#include <iio_bulk.h>

#define DT_DRV_COMPAT adi_ad463x

static ssize_t ad463x_bulk_read(const struct device *dev, void *dst, size_t len)
{
	return ad463x_read_buffer(dev, dst, len);
}

static size_t ad463x_bulk_frame_size(const struct device *dev)
{
	return ad463x_get_frame_size(dev);
}

/* Physical channel 0 is at word offset 0, channel 1 at word offset 1. */
static const uint8_t ad463x_word_map[] = { 0, 1 };

static const struct iio_bulk_capture_api ad463x_bulk_api = {
	.read           = ad463x_bulk_read,
	.frame_size     = ad463x_bulk_frame_size,
	.words_per_sample = 2,
	.word_map       = ad463x_word_map,
};

#define AD463X_IIO_PROVIDER(n)						\
	IIO_BULK_CAPTURE_PROVIDER_DEFINE(ad463x_iio_provider_##n,	\
					 DEVICE_DT_INST_GET(n),		\
					 &ad463x_bulk_api);

DT_INST_FOREACH_STATUS_OKAY(AD463X_IIO_PROVIDER)
