/**
 * @file zephyr_uart.c
 * @brief Zephyr-specific implementation of no-OS UART functions
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "zephyr_uart.h"
#include "zephyr_alloc.h"
#include "zephyr_mutex.h"
#include "zephyr_util.h"

#define UART_DEVICE_NODE DT_CHOSEN(zephyr_shell_uart)
static const struct device *const uart_dev = DEVICE_DT_GET(UART_DEVICE_NODE);

static void *uart_mutex_table[UART_MAX_NUMBER + 1];

int32_t zephyr_uart_init(struct zephyr_uart_desc **desc, struct zephyr_uart_init_param *param)
{    
	int32_t ret;

	if (!param || !param->platform_ops
	    || param->device_id >= NO_OS_ARRAY_SIZE(uart_mutex_table))
		return -EINVAL;

	if (!param->platform_ops->init)
		return -ENOSYS;

	ret = param->platform_ops->init(desc, param);
	if (ret)
		return ret;

	(*desc)->platform_ops = param->platform_ops;

	zephyr_mutex_init(&(uart_mutex_table[param->device_id]));
	(*desc)-> mutex = uart_mutex_table[param->device_id];
    
    return 0;
}

int32_t zephyr_uart_remove(struct zephyr_uart_desc *desc)
{
	if (!desc || !desc->platform_ops)
		return -EINVAL;

	if (!desc->platform_ops->remove)
		return -ENOSYS;

	zephyr_mutex_remove(desc->mutex);
	uart_mutex_table[desc->device_id] = NULL;

	return desc->platform_ops->remove(desc);
}

int32_t zephyr_uart_write(struct zephyr_uart_desc *desc, const uint8_t *data, uint32_t bytes_number)
{
    // printk("Din zephyr_uart_write\n");

    int32_t ret;
    
    if (!desc || !desc->platform_ops || !data) {
        return -EINVAL;
    }

    if (!desc->platform_ops->write)
		return -ENOSYS;

	zephyr_mutex_lock(desc->mutex);
	ret = desc->platform_ops->write(desc, data, bytes_number);
	zephyr_mutex_unlock(desc->mutex);

	return ret;
}

int32_t zephyr_uart_read(struct zephyr_uart_desc *desc, uint8_t *data, uint32_t bytes_number)
{
    int32_t ret;
    if (!desc || !data || !desc->platform_ops) {
        return -EINVAL;
    }

    if (!desc->platform_ops->read)
        return -ENOSYS;

    zephyr_mutex_lock(desc->mutex);
    ret = desc->platform_ops->read(desc, data, bytes_number);
    zephyr_mutex_unlock(desc->mutex);

    return ret;
}

uint32_t zephyr_uart_get_errors(struct zephyr_uart_desc *desc)
{
    if (!desc) {
        return -EINVAL;
    }
    
    return 0;
}
