/*
 * Copyright (c) 2026 Analog Devices, Inc.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ZEPHYR_INCLUDE_IIO_TRIGGER_H_
#define ZEPHYR_INCLUDE_IIO_TRIGGER_H_

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <iio/iio.h>


enum iio_device_trigger_type {
	IIO_DEVICE_TRIGGER_TIMER,
	IIO_DEVICE_TRIGGER_ZEPHYR_COMMON,
	IIO_DEVICE_TRIGGER_ZEPHYR_STREAM,
};

struct iio_trigger_ops {
	struct iio_device * (*create)(struct iio_context *ctx, const struct device *dev, 
						enum iio_device_trigger_type type, const char *id);
	void (*init)(const struct device *dev);
	int (*subscribe)(sys_snode_t *node);
	void (*unsubscribe)(sys_snode_t *node);
};

struct iio_device_trigger_config {
	enum iio_device_trigger_type trigger_type;
	const char trigger_id[11];
	struct iio_trigger_ops const *ops;
};

#endif  /* ZEPHYR_INCLUDE_IIO_TRIGGER_H_ */
