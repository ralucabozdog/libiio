# Copyright (c) 2022 TOKITA Hiroshi <tokita.hiroshi@fujitsu.com>
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(accel_polling LANGUAGES C)

# Add application sources
target_sources(app PRIVATE 
    src/main.c
    ../modules/hal/adi/common/common_data.c
    ../modules/hal/adi/platform/maxim/maxim_delay.c
    ../modules/hal/adi/platform/maxim/parameters.c
    ../modules/hal/adi/zephyr_specific/source/zephyr_uart_no_os.c  # Add Zephyr UART bridge
    ../modules/hal/adi/adc_demo/adc_demo.c
    ../modules/hal/adi/adc_demo/iio_adc_demo.c
    ../modules/hal/adi/dac_demo/dac_demo.c
    ../modules/hal/adi/dac_demo/iio_dac_demo.c
    ../modules/hal/adi/iio/iio_app/iio_app.c
    ../modules/hal/adi/iio/iio.c
    ../modules/hal/adi/iio/iiod.c
    ../modules/hal/adi/zephyr_specific/source/zephyr_util.c
    ../modules/hal/adi/zephyr_specific/source/zephyr_alloc.c
    ../modules/hal/adi/zephyr_specific/source/zephyr_mutex.c
    ../modules/hal/adi/zephyr_specific/source/zephyr_uart.c
    ../modules/hal/adi/zephyr_specific/source/zephyr_circular_buffer.c
)

# Add include directories
target_include_directories(app PRIVATE
    ../modules/hal/adi/common
    ../modules/hal/adi/platform
    ../modules/hal/adi/platform/maxim
    ../modules/hal/adi/adc_demo
    ../modules/hal/adi/dac_demo
    ../modules/hal/adi/iio
    ../modules/hal/adi/iio/iio_app
    ../modules/hal/adi/zephyr_specific/include
    ../modules/hal/adi/zephyr_specific/source
    ../modules/hal/adi/MAX/Libraries/CMSIS/Device/Maxim/MAX32690/Include
)

# Add compiler definitions
target_compile_definitions(app PRIVATE 
    MAXIM_PLATFORM
    IIO_SUPPORT
    ZEPHYR_BACKEND
)

# Libraries are automatically linked by the Zephyr build system