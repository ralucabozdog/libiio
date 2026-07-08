/*
 * Copyright (c) 2026 Analog Devices, Inc.
 *
 * SPDX-License-Identifier: MIT
 *
 * Generic bulk-capture capability for IIO devices.
 *
 * A device that can deliver a whole block of samples straight off its
 * acquisition path (e.g. AXI DMAC) publishes an iio_bulk_capture_provider
 * in the linker iterable section. io_channels.c's readbuf looks it up by
 * the backing Zephyr device handle and drives binary capture through it
 * without naming any concrete driver.
 */

#ifndef ZEPHYR_INCLUDE_IIO_BULK_H_
#define ZEPHYR_INCLUDE_IIO_BULK_H_

#include <zephyr/device.h>
#include <zephyr/sys/iterable_sections.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct iio_bulk_capture_api {
	/*
	 * Capture raw samples straight off the device into dst. Returns bytes
	 * written (a whole multiple of frame_size) or a negative errno.
	 */
	ssize_t (*read)(const struct device *dev, void *dst, size_t len);
	/* Raw bytes per captured sample as delivered by read(). */
	size_t (*frame_size)(const struct device *dev);
	/* Number of 32-bit raw words per captured sample. */
	uint8_t words_per_sample;
	/*
	 * word_map[phys] -> index of the 32-bit raw word holding physical
	 * channel `phys` within one frame. Length == words_per_sample.
	 */
	const uint8_t *word_map;
};

struct iio_bulk_capture_provider {
	const struct device *dev;
	const struct iio_bulk_capture_api *api;
};

#define IIO_BULK_CAPTURE_PROVIDER_DEFINE(name, _dev, _api)			\
	static const STRUCT_SECTION_ITERABLE(iio_bulk_capture_provider, name) =	\
	{									\
		.dev = (_dev),							\
		.api = (_api),							\
	}

static inline const struct iio_bulk_capture_api *
iio_bulk_capture_find(const struct device *dev)
{
	STRUCT_SECTION_FOREACH(iio_bulk_capture_provider, p) {
		if (p->dev == dev) {
			return p->api;
		}
	}
	return NULL;
}

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_IIO_BULK_H_ */
