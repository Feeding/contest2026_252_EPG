/****************************************************************************
 * board/contest_board/chip/bk7258_clockconfig.c
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

#include <stdint.h>

#include "arm_internal.h"
#include "bk7258_clockconfig.h"

#ifdef CONFIG_BK7258_TXPIN_TRACE
/* Defined in bk7258_start.c; see CONFIG_BK7258_TXPIN_TRACE. */
void bk7258_txpin_mark(void);
#  define txmark() bk7258_txpin_mark()
#else
#  define txmark()
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_clock_settle
 *
 * Description:
 *   Wait after re-sourcing or ungating a module clock before anything touches
 *   the module.
 *
 *   This is not decoration.  Without it the boot stopped here: the marker
 *   placed immediately after bk7258_clockconfig() never appeared, and every
 *   later stage was dead.  Adding two instrumentation calls inside this
 *   function -- whose only effect was to burn about half a second between the
 *   register writes -- made the same image, same layout and same config run
 *   all the way to nx_start().  So what the code needed was time, not a
 *   different register value; the values were checked bit by bit against the
 *   vendor headers and are correct.
 *
 *   How much time is actually required is not known -- the working figure came
 *   from a probe, not from a datasheet -- so this errs high.  It costs a few
 *   milliseconds once per UART at boot.
 *
 ****************************************************************************/

static void bk7258_clock_settle(void)
{
  volatile uint32_t spin;

  for (spin = 0; spin < 200000; spin++)
    {
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_uart_clockenable
 ****************************************************************************/

void bk7258_uart_clockenable(int uart)
{
  uint32_t divsel;
  uint32_t cken;
  uint32_t regval;

  switch (uart)
    {
      case 0:
        divsel = SYS_CLKDIV_UART0_MASK | SYS_CLKSEL_UART0;
        cken   = SYS_CKEN_UART0;
        break;

      case 1:
        divsel = SYS_CLKDIV_UART1_MASK | SYS_CLKSEL_UART1;
        cken   = SYS_CKEN_UART1;
        break;

      case 2:
        divsel = SYS_CLKDIV_UART2_MASK | SYS_CLKSEL_UART2;
        cken   = SYS_CKEN_UART2;
        break;

      default:
        return;
    }

  /* Source the UART from the 26MHz crystal with no further division, which
   * is the clock BK7258_UART_CLOCK assumes when computing the baud divisor.
   * Clearing both the divider and the select bit selects XTAL/1.
   */

  txmark();   /* trace 3a: entered, about to touch the divider */

  regval  = getreg32(BK7258_SYS_CPU_CLKDIV_MODE1);
  regval &= ~divsel;
  putreg32(regval, BK7258_SYS_CPU_CLKDIV_MODE1);

  bk7258_clock_settle();

  txmark();   /* trace 3b: divider written, clock still gated */

  /* Ungate the module clock. */

  regval  = getreg32(BK7258_SYS_CPU_DEVICE_CKEN);
  regval |= cken;
  putreg32(regval, BK7258_SYS_CPU_DEVICE_CKEN);

  bk7258_clock_settle();
}

/****************************************************************************
 * Name: bk7258_clockconfig
 ****************************************************************************/

void bk7258_clockconfig(void)
{
  /* The bootloader leaves the core at 480MHz/4 = 120 MHz (reg0x08
   * cksel_core=3, clkdiv_core=3) -- discovered only after JPEG decode
   * clocked in at 1.9 s/frame.  The vendor runs CPU0 at 240 MHz, so
   * take their divider: 480/2.  BOARD_CPU_FREQUENCY and LOOPSPERMSEC
   * must agree with this value.
   */

  modifyreg32(BK7258_SYS_BASE + (0x08 << 2), 0xf, 1);

  /* Flash controller and PSRAM are already running by the time we get
   * here: the boot ROM and the Beken second-stage bootloader configure
   * them in order to fetch this image over XIP.  Enabling the clocks
   * for the UARTs we actually use is all that remains.
   */

#ifdef CONFIG_BK7258_UART0
  bk7258_uart_clockenable(0);
#endif
#ifdef CONFIG_BK7258_UART1
  bk7258_uart_clockenable(1);
#endif
#ifdef CONFIG_BK7258_UART2
  bk7258_uart_clockenable(2);
#endif
}
