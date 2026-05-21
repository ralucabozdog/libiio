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

struct iio_device * iio_device_trigger_create(struct iio_context *ctx, enum iio_device_trigger_type type, const char *id);
void iio_device_trigger_init(const struct iio_device *iio_device);
int iio_device_trigger_subscribe(sys_snode_t *node);
void iio_device_trigger_unsubscribe(sys_snode_t *node);
void iio_device_trigger_set_period(const struct iio_device *iio_device, uint32_t period_ms);
int iio_device_trigger_read_attr(const struct iio_device *iio_device, const struct iio_attr *attr,
		char *dst, size_t len);
int iio_device_trigger_write_attr(const struct iio_device *iio_device, const struct iio_attr *attr,
		const char *src, size_t len);

#endif  /* ZEPHYR_INCLUDE_IIO_TRIGGER_H_ */