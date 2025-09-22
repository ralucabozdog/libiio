/***************************************************************************//**
 *   @file   zephyr_util.h
 *   @brief  Header file of utility functions.
 *   @author DBogdan (dragos.bogdan@analog.com)
********************************************************************************
 * Copyright 2018(c) Analog Devices, Inc.
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *  - Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  - Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *  - Neither the name of Analog Devices, Inc. nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *  - The use of this software may or may not infringe the patent rights
 *    of one or more patent holders.  This license does not release you
 *    from the requirement that you obtain separate licenses from these
 *    patent holders to use this software.
 *  - Use of the software either in source or binary form, must be run
 *    on or directly connected to an Analog Devices Inc. component.
 *
 * THIS SOFTWARE IS PROVIDED BY ANALOG DEVICES "AS IS" AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, NON-INFRINGEMENT,
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL ANALOG DEVICES BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, INTELLECTUAL PROPERTY RIGHTS, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*******************************************************************************/
#ifndef _NO_OS_UTIL_H_
#define _NO_OS_UTIL_H_

/******************************************************************************/
/***************************** Include Files **********************************/
/******************************************************************************/
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
/******************************************************************************/
/********************** Macros and Constants Definitions **********************/
/******************************************************************************/
#define NO_OS_BIT(x)	(1 << (x))

#define NO_OS_ARRAY_SIZE(x) \
	(sizeof(x) / sizeof((x)[0]))

#define NO_OS_DIV_ROUND_UP(x,y) \
	(((x) + (y) - 1) / (y))
#define NO_OS_DIV_ROUND_CLOSEST(x, y) \
	(((x) + (y) / 2) / (y))
#define NO_OS_DIV_ROUND_CLOSEST_ULL(x, y) \
	NO_OS_DIV_ROUND_CLOSEST(x, y)

#define no_os_min(x, y) \
	(((x) < (y)) ? (x) : (y))
#define no_os_min_t(type, x, y) \
	(type)no_os_min((type)(x), (type)(y))

#define no_os_max(x, y) \
	(((x) > (y)) ? (x) : (y))
#define no_os_max_t(type, x, y) \
	(type)no_os_max((type)(x), (type)(y))

#define no_os_clamp(val, min_val, max_val) \
	(no_os_max(no_os_min((val), (max_val)), (min_val)))
#define no_os_clamp_t(type, val, min_val, max_val) \
	(type)no_os_clamp((type)(val), (type)(min_val), (type)(max_val))

#define no_os_swap(x, y) \
	{typeof(x) _tmp_ = (x); (x) = (y); (y) = _tmp_;}

#define no_os_round_up(x,y) \
		(((x)+(y)-1)/(y))

#define NO_OS_BITS_PER_LONG 32

#define NO_OS_GENMASK(h, l) ({ 					\
		uint32_t t = (uint32_t)(~0UL);			\
		t = t << (NO_OS_BITS_PER_LONG - (h - l + 1));		\
		t = t >> (NO_OS_BITS_PER_LONG - (h + 1));		\
		t;						\
})

#define no_os_bswap_constant_32(x) \
	((((x) & 0xff000000) >> 24) | (((x) & 0x00ff0000) >>  8) | \
	 (((x) & 0x0000ff00) <<  8) | (((x) & 0x000000ff) << 24))

#define no_os_bswap_constant_16(x) ((((x) & (uint16_t)0xff00) >> 8) | \
				 (((x) & (uint16_t)0x00ff) << 8))

#define no_os_bit_swap_constant_8(x) \
	((((x) & 0x80) >> 7) | \
	 (((x) & 0x40) >> 5) | \
	 (((x) & 0x20) >> 3) | \
	 (((x) & 0x10) >> 1) | \
	 (((x) & 0x08) << 1) | \
	 (((x) & 0x04) << 3) | \
	 (((x) & 0x02) << 5) | \
	 (((x) & 0x01) << 7))

#define NO_OS_U16_MAX		((uint16_t)~0U)
#define NO_OS_S16_MAX		((int16_t)(NO_OS_U16_MAX>>1))

#define NO_OS_DIV_U64(x, y) (x / y)

#define NO_OS_UNUSED_PARAM(x) ((void)x)

#define no_os_shift_right(x, s) ((x) < 0 ? -(-(x) >> (s)) : (x) >> (s))

#define no_os_align(x, align) (((x) + ((typeof(x))(align) - 1)) & ~((typeof(x))(align) - 1))

#define no_os_bcd2bin(x)	(((x) & 0x0f) + ((x) >> 4) * 10)
#define no_os_bin2bcd(x)	((((x) / 10) << 4) + (x) % 10)

/******************************************************************************/
/************************ Functions Declarations ******************************/
/******************************************************************************/

/* Converts from string to uint32_t */
uint32_t zephyr_str_to_uint32(const char *str);

#endif // _NO_OS_UTIL_H_
