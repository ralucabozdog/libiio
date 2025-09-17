##############################################################################
#
# Copyright (C) 2023-2024 Analog Devices, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
##############################################################################

# IIO library sources
zephyr_library()

zephyr_library_sources(
    iio.c
    iiod.c
    iio_trigger.c
    iio_app/iio_app.c
)

zephyr_library_include_directories(
    .
    iio_app
    ../no-OS/include
    ../no-OS/zephyr
    ${CMAKE_SOURCE_DIR}/src/platform/maxim
    ${CMAKE_SOURCE_DIR}/src/drivers_platform/maxim/max32690
)

# Define IIO_SUPPORT macro for the library
zephyr_library_compile_definitions(IIO_SUPPORT)

# Make headers available to the rest of the system
zephyr_include_directories(.)
zephyr_include_directories(iio_app)
zephyr_include_directories(../no-OS/include)
zephyr_include_directories(${CMAKE_SOURCE_DIR}/src/platform/maxim)
zephyr_include_directories(${CMAKE_SOURCE_DIR}/src/drivers_platform/maxim/max32690)
