/****************************************************************************
 * board/contest_board/chip/bk7258_clockconfig.h
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

#ifndef __BOARD_CONTEST_BOARD_CHIP_BK7258_CLOCKCONFIG_H
#define __BOARD_CONTEST_BOARD_CHIP_BK7258_CLOCKCONFIG_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

#include "bk7258_memorymap.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* System control registers.  The Beken SDK addresses these by word index;
 * the byte offsets below are those indices multiplied by four.
 */

#define BK7258_SYS_CPU_CLKDIV_MODE1 (BK7258_SYS_BASE + (0x08 << 2))
#define BK7258_SYS_CPU_DEVICE_CKEN  (BK7258_SYS_BASE + (0x0c << 2))

/* CPU clock divider / select register (word 0x08) */

#define SYS_CLKDIV_CORE_SHIFT       (0)       /* Bits 0-3:   Core divider */
#define SYS_CLKDIV_CORE_MASK        (0xf << SYS_CLKDIV_CORE_SHIFT)
#define SYS_CKSEL_CORE_SHIFT        (4)       /* Bits 4-5:   Core source */
#define SYS_CKSEL_CORE_MASK         (0x3 << SYS_CKSEL_CORE_SHIFT)
#define SYS_CLKDIV_BUS              (1 << 6)  /* Bit 6:      Bus divider */

#define SYS_CLKDIV_UART0_SHIFT      (8)       /* Bits 8-9:   UART0 divider */
#define SYS_CLKDIV_UART0_MASK       (0x3 << SYS_CLKDIV_UART0_SHIFT)
#define SYS_CLKSEL_UART0            (1 << 10) /* Bit 10:  0=XTAL26M, 1=APLL */

#define SYS_CLKDIV_UART1_SHIFT      (11)      /* Bits 11-12: UART1 divider */
#define SYS_CLKDIV_UART1_MASK       (0x3 << SYS_CLKDIV_UART1_SHIFT)
#define SYS_CLKSEL_UART1            (1 << 13) /* Bit 13:  0=XTAL26M, 1=APLL */

#define SYS_CLKDIV_UART2_SHIFT      (14)      /* Bits 14-15: UART2 divider */
#define SYS_CLKDIV_UART2_MASK       (0x3 << SYS_CLKDIV_UART2_SHIFT)
#define SYS_CLKSEL_UART2            (1 << 16) /* Bit 16:  0=XTAL26M, 1=APLL */

/* Device clock enable register (word 0x0c) */

#define SYS_CKEN_I2C0               (1 << 0)
#define SYS_CKEN_SPI0               (1 << 1)
#define SYS_CKEN_UART0              (1 << 2)
#define SYS_CKEN_PWM0               (1 << 3)
#define SYS_CKEN_TIM0               (1 << 4)
#define SYS_CKEN_SADC               (1 << 5)
#define SYS_CKEN_IRDA               (1 << 6)
#define SYS_CKEN_EFUSE              (1 << 7)
#define SYS_CKEN_I2C1               (1 << 8)
#define SYS_CKEN_SPI1               (1 << 9)
#define SYS_CKEN_UART1              (1 << 10)
#define SYS_CKEN_UART2              (1 << 11)

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_clockconfig
 *
 * Description:
 *   Bring up the clocks this port depends on.  The boot ROM and the Beken
 *   bootloader have already configured the core PLL and the flash
 *   controller, so this only has to enable and route the peripheral clocks
 *   that NuttX drives itself.
 *
 ****************************************************************************/

void bk7258_clockconfig(void);

/****************************************************************************
 * Name: bk7258_uart_clockenable
 *
 * Description:
 *   Enable the module clock for one UART and source it from the 26MHz
 *   crystal, which is what BK7258_UART_CLOCK assumes.
 *
 ****************************************************************************/

void bk7258_uart_clockenable(int uart);

#endif /* __BOARD_CONTEST_BOARD_CHIP_BK7258_CLOCKCONFIG_H */
