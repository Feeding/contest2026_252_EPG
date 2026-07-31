/****************************************************************************
 * board/contest_board/chip/bk7258_lowputc.h
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

#ifndef __BOARD_CONTEST_BOARD_CHIP_BK7258_LOWPUTC_H
#define __BOARD_CONTEST_BOARD_CHIP_BK7258_LOWPUTC_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

#include "bk7258_uart.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Pick out the console UART, if there is one. */

#if defined(CONFIG_UART0_SERIAL_CONSOLE) && defined(CONFIG_BK7258_UART0)
#  define BK7258_CONSOLE_BASE   BK7258_UART0_BASE
#  define BK7258_CONSOLE_UART   0
#  define BK7258_CONSOLE_BAUD   CONFIG_UART0_BAUD
#  define BK7258_CONSOLE_BITS   CONFIG_UART0_BITS
#  define BK7258_CONSOLE_PARITY CONFIG_UART0_PARITY
#  define BK7258_CONSOLE_2STOP  CONFIG_UART0_2STOP
#  define HAVE_CONSOLE          1
#elif defined(CONFIG_UART1_SERIAL_CONSOLE) && defined(CONFIG_BK7258_UART1)
#  define BK7258_CONSOLE_BASE   BK7258_UART1_BASE
#  define BK7258_CONSOLE_UART   1
#  define BK7258_CONSOLE_BAUD   CONFIG_UART1_BAUD
#  define BK7258_CONSOLE_BITS   CONFIG_UART1_BITS
#  define BK7258_CONSOLE_PARITY CONFIG_UART1_PARITY
#  define BK7258_CONSOLE_2STOP  CONFIG_UART1_2STOP
#  define HAVE_CONSOLE          1
#elif defined(CONFIG_UART2_SERIAL_CONSOLE) && defined(CONFIG_BK7258_UART2)
#  define BK7258_CONSOLE_BASE   BK7258_UART2_BASE
#  define BK7258_CONSOLE_UART   2
#  define BK7258_CONSOLE_BAUD   CONFIG_UART2_BAUD
#  define BK7258_CONSOLE_BITS   CONFIG_UART2_BITS
#  define BK7258_CONSOLE_PARITY CONFIG_UART2_PARITY
#  define BK7258_CONSOLE_2STOP  CONFIG_UART2_2STOP
#  define HAVE_CONSOLE          1
#else
#  undef HAVE_CONSOLE
#endif

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_lowsetup
 *
 * Description:
 *   Configure the console UART so that arm_lowputc() works.  Called from
 *   __start() before anything that might want to print.
 *
 ****************************************************************************/

void bk7258_lowsetup(void);

/****************************************************************************
 * Name: bk7258_uart_configure
 *
 * Description:
 *   Apply a line configuration to one UART: pin muxing, clock, baud rate and
 *   frame format.  Interrupts are left disabled.
 *
 ****************************************************************************/

void bk7258_uart_configure(uintptr_t base, int uart, uint32_t baud,
                           unsigned int databits, unsigned int parity,
                           bool stop2);

#endif /* __BOARD_CONTEST_BOARD_CHIP_BK7258_LOWPUTC_H */
