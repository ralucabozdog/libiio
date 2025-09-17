zephyr_library()

# Explicitly list only the source files we want to include
# to avoid automatic inclusion of problematic files
zephyr_library_sources(
    util/no_os_alloc.c
    util/no_os_uart.c
    util/no_os_util.c
    util/no_os_list.c
    util/no_os_mutex.c
    util/no_os_lf256fifo.c
    zephyr/zephyr_circular_buffer.c
    zephyr/zephyr_uart.c
)


set_property(SOURCE 
    util/no_os_circular_buffer.c
    PROPERTY HEADER_FILE_ONLY TRUE
)

zephyr_library_include_directories(
    include
    util
    zephyr
)

zephyr_include_directories(
    include
    zephyr
)
