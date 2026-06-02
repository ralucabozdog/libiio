/*
 * Copyright (c) 2025 Analog Devices, Inc.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ZEPHYR_INCLUDE_IIO_DEVICE_H_
#define ZEPHYR_INCLUDE_IIO_DEVICE_H_

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/iterable_sections.h>
#include <iio/iio.h>
#include <iio_trigger.h>

#ifdef __cplusplus
extern "C" {
#endif

struct iio_buffer_pdata {
	sys_snode_t node;
	const struct device *zephyr_dev;
	const struct iio_device *iio_dev;
	struct iio_channels_mask *mask;
	bool enabled;
	struct k_mutex lock;
	sys_slist_t pending_blocks;
	void *trig;
	// struct zephyr_trigger_state *trig;
	size_t sample_size;
};

struct iio_block_pdata {
	sys_snode_t node;
	struct iio_buffer_pdata *buf_pdata;
	void *data;
	size_t size;
	size_t bytes_used;
	bool cyclic;
	struct k_sem ready_sem;
};

struct iio_device_info {
	const char *name;
	const struct device *dev;
};

#define IIO_DEVICE_INFO_INITIALIZER(_dev, _name)				\
	{									\
		.dev = _dev,							\
		.name = _name,							\
	}

#define IIO_DEVICE_INFO_DEFINE(name, ...)					\
	static const STRUCT_SECTION_ITERABLE(iio_device_info, name) =		\
		IIO_DEVICE_INFO_INITIALIZER(__VA_ARGS__)

#define IIO_DEVICE_INFO_DT_NAME(node_id)					\
	_CONCAT(__iio_device_info, DEVICE_DT_NAME_GET(node_id))

#define IIO_DEVICE_INFO_DT_DEFINE(node_id, name)				\
	IIO_DEVICE_INFO_DEFINE(IIO_DEVICE_INFO_DT_NAME(node_id),		\
			       DEVICE_DT_GET(node_id), name)

#define IIO_DEVICE_DT_DEFINE(node_id, name, init_fn, pm_device,			\
			     data_ptr, cfg_ptr, level, prio,			\
			     api_ptr, ...)					\
	DEVICE_DT_DEFINE(node_id, init_fn, pm_device,				\
			 data_ptr, cfg_ptr, level, prio,			\
			 api_ptr, __VA_ARGS__);					\
										\
	IIO_DEVICE_INFO_DT_DEFINE(node_id, name);


#define IIO_DEVICE_DT_INST_DEFINE(inst, ...)					\
	IIO_DEVICE_DT_DEFINE(DT_DRV_INST(inst), __VA_ARGS__)

typedef int (*iio_device_add_channels_t)(const struct device *dev,
		struct iio_device *iio_device);

typedef int (*iio_device_read_attr_t)(const struct device *dev,
		const struct iio_device *iio_device, const struct iio_attr *attr,
		char *dst, size_t len);

typedef int (*iio_device_write_attr_t)(const struct device *dev,
		const struct iio_device *iio_device, const struct iio_attr *attr,
		const char *src, size_t len);

typedef const char *(*iio_device_get_buffer_name_t)(const struct device *dev);

typedef int (*iio_device_add_trigger_t)(struct iio_context *ctx, struct iio_device *iio_device);

typedef const struct device *(*iio_device_get_zephyr_dev_t)(const struct device *dev);

__subsystem struct iio_device_driver_api {
	iio_device_add_channels_t add_channels;
	iio_device_read_attr_t read_attr;
	iio_device_write_attr_t write_attr;
	iio_device_get_buffer_name_t get_buffer_name;
	iio_device_add_trigger_t add_trigger;
	iio_device_get_zephyr_dev_t get_zephyr_dev;
};

__syscall int iio_device_add_channels(const struct device *dev,
		struct iio_device *iio_device);

static inline int z_impl_iio_device_add_channels(const struct device *dev,
		struct iio_device *iio_device)
{
	const struct iio_device_driver_api *api = DEVICE_API_GET(iio_device, dev);

	return api->add_channels(dev, iio_device);
}

__syscall int iio_device_read_attr(const struct device *dev,
		const struct iio_device *iio_device, const struct iio_attr *attr,
		char *dst, size_t len);

static inline int z_impl_iio_device_read_attr(const struct device *dev,
		const struct iio_device *iio_device, const struct iio_attr *attr,
		char *dst, size_t len)
{
	const struct iio_device_driver_api *api = DEVICE_API_GET(iio_device, dev);

	if (api->read_attr == NULL) {
		return -ENOSYS;
	}

	return api->read_attr(dev, iio_device, attr, dst, len);
}

__syscall int iio_device_write_attr(const struct device *dev,
		const struct iio_device *iio_device, const struct iio_attr *attr,
		const char *src, size_t len);

static inline int z_impl_iio_device_write_attr(const struct device *dev,
		const struct iio_device *iio_device, const struct iio_attr *attr,
		const char *src, size_t len)
{
	const struct iio_device_driver_api *api = DEVICE_API_GET(iio_device, dev);

	if (api->write_attr == NULL) {
		return -ENOSYS;
	}

	return api->write_attr(dev, iio_device, attr, src, len);
}

__syscall const char *iio_device_get_buffer_name(const struct device *dev);

static inline const char *z_impl_iio_device_get_buffer_name(const struct device *dev)
{
	const struct iio_device_driver_api *api = DEVICE_API_GET(iio_device, dev);

	if (api->get_buffer_name == NULL) {
		return "buffer";
	}

	return api->get_buffer_name(dev);
}

__syscall int iio_device_add_trigger(struct iio_context *ctx, struct iio_device *iio_device);

static inline int z_impl_iio_device_add_trigger(struct iio_context *ctx, struct iio_device *iio_device)
{
	const struct device *dev = (const struct device *) iio_device_get_pdata(iio_device);
	const struct iio_device_driver_api *api = DEVICE_API_GET(iio_device, dev);

	if (api->add_trigger == NULL) {
		return -ENOSYS;
	}

	return api->add_trigger(ctx, iio_device);
}

__syscall const struct device *iio_device_get_zephyr_dev(const struct device *dev);

static inline const struct device *z_impl_iio_device_get_zephyr_dev(const struct device *dev)
{
	const struct iio_device_driver_api *api = DEVICE_API_GET(iio_device, dev);

	if (api->get_zephyr_dev == NULL) {
		return NULL;
	}

	return api->get_zephyr_dev(dev);
}

#ifdef __cplusplus
}
#endif

#include <zephyr/syscalls/iio_device.h>

#endif  /* ZEPHYR_INCLUDE_IIO_DEVICE_H_ */
