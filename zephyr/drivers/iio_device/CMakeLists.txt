# Copyright (c) 2025 Analog Devices, Inc.
# SPDX-License-Identifier: MIT

zephyr_syscall_header(${ZEPHYR_LIBIIO_MODULE_DIR}/zephyr/include/iio_device.h)
zephyr_syscall_header(${ZEPHYR_LIBIIO_MODULE_DIR}/zephyr/include/iio_trigger.h)

zephyr_linker_sources(SECTIONS iio-device-rom.ld)
zephyr_iterable_section(NAME iio_device_info KVMA RAM_REGION GROUP RODATA_REGION)

if(CONFIG_LIBIIO_IIO_DEVICE_SENSOR OR CONFIG_LIBIIO_IIO_DEVICE_IO_CHANNELS)
  zephyr_library_sources(trigger_private.c)
endif()
zephyr_library_sources_ifdef(CONFIG_LIBIIO_IIO_DEVICE_IO_CHANNELS io_channels.c)
zephyr_library_sources_ifdef(CONFIG_LIBIIO_IIO_DEVICE_SENSOR sensor.c)
