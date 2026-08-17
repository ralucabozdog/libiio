/*
 * Copyright (c) 2026 Analog Devices, Inc.
 *
 * SPDX-License-Identifier: MIT
 */

#include <iio/iio-backend.h>
#include <iio_trigger.h>

/**
 * @brief Create and attach a trigger device to an IIO device.
 *
 * Shared helper used by iio,sensor and iio,io-channels drivers.
 * Reads trigger type and id from the trigger Zephyr device config,
 * creates the IIO trigger device, associates it with @p iio_device,
 * and calls iio_trigger_init().
 *
 * @param ctx         IIO context.
 * @param iio_device  IIO device to attach the trigger to.
 * @param trigger_dev Zephyr device that implements the iio_trigger API.
 *                    May be NULL, in which case -ENODEV is returned.
 * @return 0 on success, negative errno on failure.
 */
int iio_trigger_attach(struct iio_context *ctx,
				     struct iio_device *iio_device,
				     const struct device *trigger_dev)
{
	struct iio_device *trig;
	int ret;

	if (!trigger_dev) {
		return -ENODEV;
	}

	trig = iio_trigger_create(ctx, trigger_dev);
	if (!trig) {
		return -ENOMEM;
	}

	ret = iio_device_set_trigger(iio_device, trig);
	if (ret < 0) {
		return ret;
	}

	iio_trigger_init(trigger_dev);
	return 0;
}
