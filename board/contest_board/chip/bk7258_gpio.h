/****************************************************************************
 * board/contest_board/chip/bk7258_gpio.h
 *
 * BK7258 GPIO pin multiplexing.
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

#ifndef __BOARD_CONTEST_BOARD_CHIP_BK7258_GPIO_H
#define __BOARD_CONTEST_BOARD_CHIP_BK7258_GPIO_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>

#include "bk7258_memorymap.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define BK7258_NGPIOS               56

/* Per-pin configuration register, one 32-bit word per pin. */

#define BK7258_GPIO_CFG(n)          (BK7258_AON_GPIO_BASE + ((n) << 2))

#define GPIO_CFG_INPUT              (1 << 0)  /* Bit 0:  Input value (RO) */
#define GPIO_CFG_OUTPUT             (1 << 1)  /* Bit 1:  Output value */
#define GPIO_CFG_INPUT_EN           (1 << 2)  /* Bit 2:  Input enable */
#define GPIO_CFG_OUTPUT_EN          (1 << 3)  /* Bit 3:  Output enable */
#define GPIO_CFG_PULL_UP            (1 << 4)  /* Bit 4:  1=pull up, 0=down */
#define GPIO_CFG_PULL_EN            (1 << 5)  /* Bit 5:  Pull enable */
#define GPIO_CFG_FUNC_EN            (1 << 6)  /* Bit 6:  Peripheral function */
#define GPIO_CFG_INPUT_MONITOR      (1 << 7)  /* Bit 7:  Input monitor */
#define GPIO_CFG_CAPACITY_SHIFT     (8)       /* Bits 8-9:   Drive strength */
#define GPIO_CFG_CAPACITY_MASK      (3 << GPIO_CFG_CAPACITY_SHIFT)
#define GPIO_CFG_INT_TYPE_SHIFT     (10)      /* Bits 10-11: Interrupt type */
#define GPIO_CFG_INT_TYPE_MASK      (3 << GPIO_CFG_INT_TYPE_SHIFT)
#define GPIO_CFG_INT_EN             (1 << 12) /* Bit 12: Interrupt enable */
#define GPIO_CFG_INT_CLEAR          (1 << 13) /* Bit 13: Interrupt clear */

/* Peripheral (alternate) function select.  These registers live in the
 * system register block, not the GPIO block: eight pins per 32-bit word,
 * four bits each.  The value is the index into the pin's alternate function
 * list, so the datasheet's "AF1" column is index 0.
 */

#define BK7258_GPIO_FUNCMODE_BASE   (BK7258_SYS_BASE + (0x30 << 2))
#define BK7258_GPIO_FUNCMODE(n)     (BK7258_GPIO_FUNCMODE_BASE + (((n) >> 3) << 2))
#define GPIO_FUNCMODE_SHIFT(n)      ((((n) & 7)) << 2)
#define GPIO_FUNCMODE_MASK(n)       (0xful << GPIO_FUNCMODE_SHIFT(n))

/* Alternate function indices used by this port.
 *
 * Note the Beken SDK names the UART devices one higher than the register
 * blocks: its GPIO_DEV_UART1_TXD is the alternate function of the UART0
 * register block.  The pin assignments here follow the datasheet, where
 * GPIO10/GPIO11 are UART0 RX/TX (and double as the ROM flash-download pins).
 */

#define BK7258_GPIO_AF0             0
#define BK7258_GPIO_AF1             1
#define BK7258_GPIO_AF2             2

/* Console/UART pin assignments (BK7258 datasheet table 3-5) */

#define BK7258_GPIO_UART0_RX        10
#define BK7258_GPIO_UART0_TX        11
#define BK7258_GPIO_UART0_AF        BK7258_GPIO_AF0

#define BK7258_GPIO_UART1_TX        0
#define BK7258_GPIO_UART1_RX        1
#define BK7258_GPIO_UART1_AF        BK7258_GPIO_AF0

#define BK7258_GPIO_UART2_RX        40
#define BK7258_GPIO_UART2_TX        41
#define BK7258_GPIO_UART2_AF        BK7258_GPIO_AF0

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_gpio_setaf
 *
 * Description:
 *   Route a pin to one of its peripheral functions.  'af' is the index into
 *   the pin's alternate function list as given in the datasheet pin
 *   multiplexing table, counting the AF1 column as index 0.
 *
 ****************************************************************************/

void bk7258_gpio_setaf(int pin, uint8_t af);

/****************************************************************************
 * Name: bk7258_gpio_config
 *
 * Description:
 *   Configure a pin as plain GPIO input or output, optionally with a pull
 *   resistor.
 *
 ****************************************************************************/

void bk7258_gpio_config(int pin, bool output, bool pullup, bool pulldown);

/****************************************************************************
 * Name: bk7258_gpio_write / bk7258_gpio_read
 ****************************************************************************/

void bk7258_gpio_write(int pin, bool value);
bool bk7258_gpio_read(int pin);

#endif /* __BOARD_CONTEST_BOARD_CHIP_BK7258_GPIO_H */
