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
        bk7258_gpio_setaf(BK7258_GPIO_UART0_RX, BK7258_GPIO_UART0_AF, true);
        bk7258_gpio_setaf(BK7258_GPIO_UART0_TX, BK7258_GPIO_UART0_AF, false);
        break;

      case 1:
        bk7258_gpio_setaf(BK7258_GPIO_UART1_RX, BK7258_GPIO_UART1_AF, true);
        bk7258_gpio_setaf(BK7258_GPIO_UART1_TX, BK7258_GPIO_UART1_AF, false);
        break;

      case 2:
        bk7258_gpio_setaf(BK7258_GPIO_UART2_RX, BK7258_GPIO_UART2_AF, true);
        bk7258_gpio_setaf(BK7258_GPIO_UART2_TX, BK7258_GPIO_UART2_AF, false);
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

  /* Write 1 to global_ctrl bit 0 and leave it there.
   *
   * This bit is not a self-clearing reset pulse, and writing 0 afterwards is
   * what an earlier version of this port did -- it holds the transmit engine
   * in reset permanently.  The failure is deceptive: the FIFO logic still
   * runs, so write-ready stays asserted and every register reads back
   * correctly, but nothing the FIFO swallows ever reaches the wire.
   *
   * Three independent sources agree the bit is a persistent state, 1 =
   * released: the Beken bootloader writes 1 here and never writes 0, with the
   * UART demonstrably working afterwards (it carries the download protocol);
   * the vendor SDK's uart_ll_soft_reset() only ever writes 1; and the SDK's
   * suspend/resume code saves and restores this register's value, which would
   * be meaningless for a momentary trigger.
   */

  putreg32(UART_GLOBAL_SOFT_RESET, base + BK7258_UART_GLOBAL_CTRL_OFFSET);

  /* Mask every interrupt source and clear anything already latched. */

  putreg32(0, base + BK7258_UART_INT_ENABLE_OFFSET);
  putreg32(0xffffffff, base + BK7258_UART_INT_STATUS_OFFSET);

  /* Turn hardware flow control and the wake-up logic off.
   *
   * Neither is wanted, and leaving flow control alone is not safe on this
   * board: with it enabled the transmitter accepts bytes into the FIFO and
   * then waits for CTS before putting any of them on the wire, and the CH340
   * bridge here has its RTS#/CTS# pins unconnected, so CTS never arrives.
   * The symptom is a console that reports itself ready and stays silent.
   * The vendor driver clears both registers on every init for the same
   * reason; this port did not, and inherited whatever the ROM downloader
   * left behind.
   */

  putreg32(0, base + BK7258_UART_FLOW_CTRL_OFFSET);
  putreg32(0, base + BK7258_UART_WAKE_CONFIG_OFFSET);

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
  /* Give up rather than wait forever.
   *
   * An unbounded wait here makes a console that never asserts write-ready
   * freeze the entire boot, and it freezes it inside the one routine that
   * would otherwise have said something about it: the board goes silent with
   * no way to tell a dead UART from a dead kernel.  Dropping the character
   * keeps that failure visible somewhere else instead of hiding it here.
   *
   * The bound is generous -- a character at 115200 needs under 90us, and this
   * is several milliseconds of spinning even at the slowest plausible core
   * clock.
   */

  uint32_t timeout = 1000000;

  while ((getreg32(BK7258_CONSOLE_BASE + BK7258_UART_FIFO_STATUS_OFFSET) &
          UART_FIFO_STATUS_WR_READY) == 0)
    {
      if (--timeout == 0)
        {
          return;
        }
    }

  putreg32((uint32_t)(unsigned char)ch,
           BK7258_CONSOLE_BASE + BK7258_UART_FIFO_PORT_OFFSET);
#endif
}
