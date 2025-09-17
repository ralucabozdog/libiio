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
#include "no_os_alloc.h"
#include "no_os_mutex.h"

#define UART_DEVICE_NODE DT_CHOSEN(zephyr_shell_uart)
static const struct device *const uart_dev = DEVICE_DT_GET(UART_DEVICE_NODE);

static void *uart_mutex_table[UART_MAX_NUMBER + 1];

int32_t zephyr_uart_init(struct zephyr_uart_desc **desc, struct zephyr_uart_init_param *param)
{    
    if (!desc || !param) {
        return -EINVAL;
    }

    struct uart_config uart_cfg = {
		.baudrate = param->baud_rate,
		.parity = param->parity,
		.stop_bits = param->stop,
		.data_bits = param->size,
		.flow_ctrl = UART_CFG_FLOW_CTRL_NONE
	};

    /* TODO call to param->platform_ops->init instead of directly calling uart_configure?
        ?how to kepp the max_uart_init implementation relevant, without re-implementation? */
	int ret = uart_configure(uart_dev, &uart_cfg);
    // int ret = param->platform_ops->init(desc, param);
	printk("Status uart_configure: %d\n", ret);

	(*desc)->platform_ops = param->platform_ops;
	no_os_mutex_init(&(uart_mutex_table[param->device_id]));
	(*desc)->mutex = uart_mutex_table[param->device_id];
    
    return 0;
}

int32_t zephyr_uart_remove(struct zephyr_uart_desc *desc)
{
    if (!desc) {
        return -EINVAL;
    }
    
    no_os_free(desc);
    return 0;
}

int32_t zephyr_uart_write(struct zephyr_uart_desc *desc, const uint8_t *data, uint32_t bytes_number)
{
    printk("Din zephyr_uart_write\n");

    int32_t ret;
    
    if (!desc || !desc->platform_ops || !data) {
        return -EINVAL;
    }

    if (!desc->platform_ops->write)
		return -ENOSYS;

	no_os_mutex_lock(desc->mutex);
	ret = desc->platform_ops->write(desc, data, bytes_number);
	no_os_mutex_unlock(desc->mutex);

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

    no_os_mutex_lock(desc->mutex);
    ret = desc->platform_ops->read(desc, data, bytes_number);
    no_os_mutex_unlock(desc->mutex);

    return ret;
}

uint32_t zephyr_uart_get_errors(struct zephyr_uart_desc *desc)
{
    if (!desc) {
        return -EINVAL;
    }
    
    return 0;
}
