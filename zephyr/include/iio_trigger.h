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
#include <iio_device.h>
#include <iio_trigger_node.h>

#ifdef __cplusplus
extern "C" {
#endif

struct iio_device_trigger_config {
	const char *trigger_id;
	const char *name;
};

typedef struct iio_device *(*iio_trigger_create_t)(struct iio_context *ctx,
			const struct device *trigg_iio_dev);

typedef void (*iio_trigger_init_t)(const struct device *dev);

typedef int (*iio_trigger_subscribe_t)(struct iio_trigger_node *node);

typedef void (*iio_trigger_unsubscribe_t)(struct iio_trigger_node *node);

__subsystem struct iio_trigger_driver_api {
	struct iio_attr_driver_api attr_api;
	iio_trigger_create_t create;
	iio_trigger_init_t init;
	iio_trigger_subscribe_t subscribe;
	iio_trigger_unsubscribe_t unsubscribe;
};

DEVICE_API_EXTENDS(iio_trigger, iio_attr, attr_api);

__syscall struct iio_device * iio_trigger_create(struct iio_context *ctx,
				const struct device *trigg_iio_dev);

static inline struct iio_device * z_impl_iio_trigger_create(struct iio_context *ctx,
				const struct device *trigg_iio_dev)
{
	const struct iio_trigger_driver_api *api = DEVICE_API_GET(iio_trigger, trigg_iio_dev);

	if (api->create == NULL) {
		return NULL;
	}

	return api->create(ctx, trigg_iio_dev);
}

__syscall void iio_trigger_init(const struct device *trigg_iio_dev);

static inline void z_impl_iio_trigger_init(const struct device *trigg_iio_dev)
{
	const struct iio_trigger_driver_api *api = DEVICE_API_GET(iio_trigger, trigg_iio_dev);

	if (api->init == NULL) {
		return;
	}

	api->init(trigg_iio_dev);
}

__syscall int iio_trigger_subscribe(const struct device *trigg_iio_dev, struct iio_trigger_node *node);

static inline int z_impl_iio_trigger_subscribe(const struct device *trigg_iio_dev, struct iio_trigger_node *node)
{
	const struct iio_trigger_driver_api *api = DEVICE_API_GET(iio_trigger, trigg_iio_dev);

	if (api->subscribe == NULL) {
		return -ENOSYS;
	}

	return api->subscribe(node);
}

__syscall void iio_trigger_unsubscribe(const struct device *trigg_iio_dev, struct iio_trigger_node *node);

static inline void z_impl_iio_trigger_unsubscribe(const struct device *trigg_iio_dev, struct iio_trigger_node *node)
{
	const struct iio_trigger_driver_api *api = DEVICE_API_GET(iio_trigger, trigg_iio_dev);

	if (api->unsubscribe == NULL) {
		return;
	}

	api->unsubscribe(node);
}

#ifdef __cplusplus
}
#endif

#include <zephyr/syscalls/iio_trigger.h>
#endif  /* ZEPHYR_INCLUDE_IIO_TRIGGER_H_ */
