/*
 * Copyright (c) 2026 Analog Devices, Inc.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ZEPHYR_INCLUDE_IIO_TRIGGER_PRIVATE_H_
#define ZEPHYR_INCLUDE_IIO_TRIGGER_PRIVATE_H_

#include <zephyr/device.h>
#include <iio_device.h>

#ifdef __cplusplus
extern "C" {
#endif

int iio_trigger_attach(struct iio_context *ctx,
				     struct iio_device *iio_device,
				     const struct device *trigger_dev);

#ifdef __cplusplus
}
#endif

#endif  /* ZEPHYR_INCLUDE_IIO_TRIGGER_PRIVATE_H_ */
