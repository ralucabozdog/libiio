/*
 * Copyright (c) 2025 Analog Devices, Inc.
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>

#include <thermocouple_cn0391.h>

LOG_MODULE_REGISTER(cn0391_app, CONFIG_LIBIIO_LOG_LEVEL);

#define CN0391_SAMPLING_THREAD_STACK_SIZE 2048
#define CN0391_SAMPLING_THREAD_PRIORITY 7
#define CN0391_SAMPLING_PERIOD_MS 5

static const struct device *cn0391_dev = DEVICE_DT_GET(DT_NODELABEL(cn0391_thermocouple));

static void cn0391_sampling_thread(void *arg1, void *arg2, void *arg3)
{
	int ret;

	if (!device_is_ready(cn0391_dev)) {
		LOG_ERR("CN0391 thermocouple device not ready");
		return;
	}

	while (1) {
		for (int i = 0; i < NUM_VIRTUAL_CHANNELS; i++) {
			ret = thermocouple_cn0391_sample(cn0391_dev, i);
			if (ret < 0 && ret != -EBUSY) {
				LOG_ERR("CN0391 sample failed for channel %d (%d)", i, ret);
			}
		}
		k_sleep(K_MSEC(CN0391_SAMPLING_PERIOD_MS));
	}
}

K_THREAD_DEFINE(cn0391_sampling, CN0391_SAMPLING_THREAD_STACK_SIZE,
		cn0391_sampling_thread, NULL, NULL, NULL,
		CN0391_SAMPLING_THREAD_PRIORITY, 0, 10000);
