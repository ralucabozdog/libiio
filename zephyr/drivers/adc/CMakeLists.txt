# Copyright (c) 2025 Analog Devices, Inc.
# SPDX-License-Identifier: MIT

zephyr_library_sources_ifdef(CONFIG_ADC_EMUL_GENERATOR	adc_emul_generator.c)
zephyr_library_sources_ifdef(CONFIG_LIBIIO_AD463X_IIO_PROVIDER	ad463x_iio_provider.c)
