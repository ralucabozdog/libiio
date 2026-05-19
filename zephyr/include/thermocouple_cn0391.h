/*
 * Copyright (c) 2025 Analog Devices, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef THERMOCOUPLE_CN0391_H_
#define THERMOCOUPLE_CN0391_H_

#include <zephyr/device.h>

#define NUM_VIRTUAL_CHANNELS 4

/**
 * @brief Sample a single thermocouple virtual channel.
 *
 * Reads the ADC channels associated with the given virtual channel index,
 * computes the hot junction temperature using Type K thermocouple and
 * Pt1000 RTD cold junction compensation, and stores the result in the
 * driver's internal data.
 *
 * @param dev Pointer to the CN0391 thermocouple device.
 * @param index Virtual channel index (0 to NUM_VIRTUAL_CHANNELS-1).
 * @return 0 on success, -EINVAL for invalid index, -EBUSY if ADC is in use,
 *         or a negative error code on ADC failure.
 */
int thermocouple_cn0391_sample(const struct device *dev, int index);

#endif /* THERMOCOUPLE_CN0391_H_ */
