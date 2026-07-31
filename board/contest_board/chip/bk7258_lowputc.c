/****************************************************************************
 * board/contest_board/chip/bk7258_lowputc.c
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

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>

#include "arm_internal.h"

#include "bk7258_clockconfig.h"
#include "bk7258_gpio.h"
#include "bk7258_lowputc.h"
#include "bk7258_uart.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_uart_pinmux
 *
 * Description:
 *   Route the RX/TX pads of one UART to the UART peripheral.
 *
 ****************************************************************************/

static void bk7258_uart_pinmux(int uart)
{
  switch (uart)
    {
      case 0:
        bk7258_gpio_setaf(BK7258_GPIO_UART0_RX, BK7258_GPIO_UART0_AF);
        bk7258_gpio_setaf(BK7258_GPIO_UART0_TX, BK7258_GPIO_UART0_AF);
        break;

      case 1:
        bk7258_gpio_setaf(BK7258_GPIO_UART1_RX, BK7258_GPIO_UART1_AF);
        bk7258_gpio_setaf(BK7258_GPIO_UART1_TX, BK7258_GPIO_UART1_AF);
        break;

      case 2:
        bk7258_gpio_setaf(BK7258_GPIO_UART2_RX, BK7258_GPIO_UART2_AF);
        bk7258_gpio_setaf(BK7258_GPIO_UART2_TX, BK7258_GPIO_UART2_AF);
        break;

      default:
        break;
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_uart_configure
 ****************************************************************************/

void bk7258_uart_configure(uintptr_t base, int uart, uint32_t baud,
                           unsigned int databits, unsigned int parity,
                           bool stop2)
{
  uint32_t regval;

  /* Clock and pads first; the UART cannot be programmed without a clock. */

  bk7258_uart_clockenable(uart);
  bk7258_uart_pinmux(uart);

  /* Reset the block so we start from a known state regardless of what the
   * ROM downloader left behind, then release the reset.
   */

  putreg32(UART_GLOBAL_SOFT_RESET, base + BK7258_UART_GLOBAL_CTRL_OFFSET);
  putreg32(0, base + BK7258_UART_GLOBAL_CTRL_OFFSET);

  /* Mask every interrupt source and clear anything already latched. */

  putreg32(0, base + BK7258_UART_INT_ENABLE_OFFSET);
  putreg32(0xffffffff, base + BK7258_UART_INT_STATUS_OFFSET);

  /* Line format and baud rate. */

  regval = UART_CONFIG_CLKDIV(UART_CLKDIV(baud));

  switch (databits)
    {
      case 5:
        regval |= UART_CONFIG_DATABITS_5;
        break;

      case 6:
        regval |= UART_CONFIG_DATABITS_6;
        break;

      case 7:
        regval |= UART_CONFIG_DATABITS_7;
        break;

      default:
        regval |= UART_CONFIG_DATABITS_8;
        break;
    }

  if (parity == 1)
    {
      regval |= UART_CONFIG_PARITY_ENABLE | UART_CONFIG_PARITY_ODD;
    }
  else if (parity == 2)
    {
      regval |= UART_CONFIG_PARITY_ENABLE;
    }

  if (stop2)
    {
      regval |= UART_CONFIG_STOPBITS_2;
    }

  regval |= UART_CONFIG_TX_ENABLE | UART_CONFIG_RX_ENABLE;
  putreg32(regval, base + BK7258_UART_CONFIG_OFFSET);

  /* Interrupt when the TX FIFO drops to half empty and as soon as a byte
   * lands in the RX FIFO, and treat 32 bit times of idle as end of frame.
   */

  putreg32(UART_FIFO_CONFIG_TXTHR(0x40) | UART_FIFO_CONFIG_RXTHR(0x01) |
           UART_FIFO_CONFIG_STOPDET_32,
           base + BK7258_UART_FIFO_CONFIG_OFFSET);
}

/****************************************************************************
 * Name: bk7258_lowsetup
 ****************************************************************************/

void bk7258_lowsetup(void)
{
#ifdef HAVE_CONSOLE
  bk7258_uart_configure(BK7258_CONSOLE_BASE, BK7258_CONSOLE_UART,
                        BK7258_CONSOLE_BAUD, BK7258_CONSOLE_BITS,
                        BK7258_CONSOLE_PARITY, BK7258_CONSOLE_2STOP != 0);
#endif
}

/****************************************************************************
 * Name: arm_lowputc
 *
 * Description:
 *   Output one byte on the console UART, busy-waiting for FIFO space.
 *
 ****************************************************************************/

void arm_lowputc(char ch)
{
#ifdef HAVE_CONSOLE
  while ((getreg32(BK7258_CONSOLE_BASE + BK7258_UART_FIFO_STATUS_OFFSET) &
          UART_FIFO_STATUS_WR_READY) == 0)
    {
    }

  putreg32((uint32_t)(unsigned char)ch,
           BK7258_CONSOLE_BASE + BK7258_UART_FIFO_PORT_OFFSET);
#endif
}
