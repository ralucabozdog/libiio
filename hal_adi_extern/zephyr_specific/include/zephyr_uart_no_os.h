/**
 * @file zephyr_uart_no_os.h
 * @brief Zephyr platform operations for no-OS UART interface
 */

#ifndef _ZEPHYR_UART_NO_OS_H_
#define _ZEPHYR_UART_NO_OS_H_

/******************************************************************************/
/***************************** Include Files **********************************/
/******************************************************************************/
#include "no_os_uart.h"

/******************************************************************************/
/************************ Functions Declarations ******************************/
/******************************************************************************/

extern const struct no_os_uart_platform_ops zephyr_uart_no_os_ops;

#endif // _ZEPHYR_UART_NO_OS_H_
