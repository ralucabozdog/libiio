/**
 * @file zephyr_uart_no_os.c
 * @brief Zephyr platform operations for no-OS UART interface
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "no_os_uart.h"
#include "no_os_error.h"
#include "no_os_mutex.h"

#define UART_DEVICE_NODE DT_CHOSEN(zephyr_shell_uart)
static const struct device *const uart_dev = DEVICE_DT_GET(UART_DEVICE_NODE);

static void *uart_mutex_table[UART_MAX_NUMBER + 1];


struct zephyr_uart_no_os_desc {
    const struct device *dev;
    bool initialized;
};


static int32_t zephyr_uart_no_os_init(struct no_os_uart_desc **desc,
                                     struct no_os_uart_init_param *param)
{
    struct no_os_uart_desc *descriptor;
    struct zephyr_uart_no_os_desc *zephyr_desc;
    
    if (!desc || !param) {
        return -EINVAL;
    }

    descriptor = (struct no_os_uart_desc *)calloc(1, sizeof(*descriptor));
    if (!descriptor) {
        return -ENOMEM;
    }

    zephyr_desc = (struct zephyr_uart_no_os_desc *)calloc(1, sizeof(*zephyr_desc));
    if (!zephyr_desc) {
        free(descriptor);
        return -ENOMEM;
    }

    struct uart_config uart_cfg = {
        .baudrate = param->baud_rate,
        .parity = (param->parity == NO_OS_UART_PAR_NO) ? UART_CFG_PARITY_NONE : 
                 (param->parity == NO_OS_UART_PAR_EVEN) ? UART_CFG_PARITY_EVEN : 
                 UART_CFG_PARITY_ODD,
        .stop_bits = (param->stop == NO_OS_UART_STOP_1_BIT) ? UART_CFG_STOP_BITS_1 : 
                     UART_CFG_STOP_BITS_2,
        .data_bits = (param->size == NO_OS_UART_CS_8) ? UART_CFG_DATA_BITS_8 : 
                     UART_CFG_DATA_BITS_7,
        .flow_ctrl = UART_CFG_FLOW_CTRL_NONE
    };

    int ret = uart_configure(uart_dev, &uart_cfg);
    if (ret) {
        free(zephyr_desc);
        free(descriptor);
        return -EIO;
    }

    zephyr_desc->dev = uart_dev;
    zephyr_desc->initialized = true;

    descriptor->device_id = param->device_id;
    descriptor->irq_id = param->irq_id;
    descriptor->baud_rate = param->baud_rate;
    descriptor->platform_ops = param->platform_ops;
    descriptor->extra = zephyr_desc;

    no_os_mutex_init(&(uart_mutex_table[param->device_id]));
    descriptor->mutex = uart_mutex_table[param->device_id];

    *desc = descriptor;
    return 0;
}


static int32_t zephyr_uart_no_os_remove(struct no_os_uart_desc *desc)
{
    if (!desc) {
        return -EINVAL;
    }

    if (desc->extra) {
        free(desc->extra);
    }

    no_os_mutex_remove(desc->mutex);
    uart_mutex_table[desc->device_id] = NULL;

    free(desc);
    return 0;
}


static int32_t zephyr_uart_no_os_read(struct no_os_uart_desc *desc, uint8_t *data,
                                     uint32_t bytes_number)
{
    if (!desc || !desc->extra || !data) {
        return -EINVAL;
    }

    struct zephyr_uart_no_os_desc *zephyr_desc = desc->extra;
    
    uint32_t i;
    // uint16_t aux = 0;

    for (i = 0; i < bytes_number; i++) {
        unsigned char ch;
        if (uart_poll_in(zephyr_desc->dev, &ch) < 0) {
            break;
        }
        data[i] = ch;
        // if (ch == '\n')
        // {
        //     data[i] = ch;
        //     aux++;
        // }
    }

    return i;
}


static int32_t zephyr_uart_no_os_write(struct no_os_uart_desc *desc, const uint8_t *data,
                                      uint32_t bytes_number)
{
    if (!desc || !desc->extra || !data) {
        return -EINVAL;
    }

    struct zephyr_uart_no_os_desc *zephyr_desc = desc->extra;
    
    uint32_t i;

    for (i = 0; i < bytes_number; i++) {
        uart_poll_out(zephyr_desc->dev, data[i]);
        // if (data[i] == '>')
        // {
        //     uart_poll_out(zephyr_desc->dev, '\r');
        //     uart_poll_out(zephyr_desc->dev, '\n');
        // }
    }

    return i;
}


static int32_t zephyr_uart_no_os_read_nonblocking(struct no_os_uart_desc *desc, uint8_t *data,
                                                  uint32_t bytes_number)
{
    if (!desc || !desc->extra || !data) {
        return -EINVAL;
    }

    struct zephyr_uart_no_os_desc *zephyr_desc = desc->extra;
    uint32_t bytes_read = 0;
    
    for (uint32_t i = 0; i < bytes_number; i++) {
        unsigned char ch;
        int ret = uart_poll_in(zephyr_desc->dev, &ch);
        if (ret != 0) {
            break; // No more data available
        }
        data[i] = ch;
        bytes_read++;
    }

    return bytes_read;
}


static int32_t zephyr_uart_no_os_write_nonblocking(struct no_os_uart_desc *desc,
                                                   const uint8_t *data,
                                                   uint32_t bytes_number)
{
    return zephyr_uart_no_os_write(desc, data, bytes_number);
}


static uint32_t zephyr_uart_no_os_get_errors(struct no_os_uart_desc *desc)
{    
    return 0;
}


const struct no_os_uart_platform_ops zephyr_uart_no_os_ops = {
    .init = zephyr_uart_no_os_init,
    .read = zephyr_uart_no_os_read,
    .write = zephyr_uart_no_os_write,
    .read_nonblocking = zephyr_uart_no_os_read_nonblocking,
    .write_nonblocking = zephyr_uart_no_os_write_nonblocking,
    .remove = zephyr_uart_no_os_remove,
    .get_errors = zephyr_uart_no_os_get_errors
};
