/****************************************************************************
 * board/contest_board/chip/bk7258_timerisr.c
 *
 * System timer built on the Armv8-M SysTick counter.
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
#include <time.h>
#include <debug.h>

#include <nuttx/arch.h>
#include <nuttx/irq.h>

#include <arch/board/board.h>

#include "arm_internal.h"

#include "bk7258_wdt.h"
#include "nvic.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* SysTick counts down from the reload value to zero, so the reload needed
 * for a period of N input clocks is N - 1.
 *
 * SysTick is clocked from the core clock, which the Beken bootloader has
 * already configured by the time NuttX runs.  BOARD_CPU_FREQUENCY records
 * what that frequency is; if the tick rate is visibly wrong, that constant
 * is the thing to correct.
 */

#define SYSTICK_RELOAD ((BOARD_CPU_FREQUENCY / CLK_TCK) - 1)

/* The reload field is 24 bits wide. */

#if SYSTICK_RELOAD > 0x00ffffff
#  error BOARD_CPU_FREQUENCY/CLK_TCK does not fit in the SysTick reload field
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_timerisr
 ****************************************************************************/

static int bk7258_timerisr(int irq, uint32_t *regs, void *arg)
{
  /* Reading the control register clears the count flag. */

  /* Heartbeat: re-arm the watchdog on every 10th tick.  If ticks stop --
   * a spin with interrupts masked, a wedged handler, a runaway loop at
   * interrupt level -- the dog fires and the chip resets itself into the
   * bootloader's download window.  Decimated because the arm sequence
   * crosses into the slow AON bus: at the 1 kHz tick rate an every-tick
   * feed taxed the CPU hard enough to cost the eye animation ~1/3 of its
   * frame budget.
   */

  static unsigned int decimate = 0;

  if (++decimate >= 10)
    {
      decimate = 0;
      bk7258_wdt_arm(BK7258_WDT_PERIOD_RUN);
    }

  nxsched_process_timer();
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: up_timer_initialize
 *
 * Description:
 *   Start the system timer interrupt at the rate given by CLK_TCK.
 *
 ****************************************************************************/

void up_timer_initialize(void)
{
  uint32_t regval;

  /* Program the tick period and restart the counter. */

  putreg32(SYSTICK_RELOAD, NVIC_SYSTICK_RELOAD);
  putreg32(0, NVIC_SYSTICK_CURRENT);

  irq_attach(NVIC_IRQ_SYSTICK, (xcpt_t)bk7258_timerisr, NULL);

  /* Run SysTick from the core clock and enable its interrupt. */

  regval = NVIC_SYSTICK_CTRL_CLKSOURCE | NVIC_SYSTICK_CTRL_TICKINT |
           NVIC_SYSTICK_CTRL_ENABLE;
  putreg32(regval, NVIC_SYSTICK_CTRL);

  up_enable_irq(NVIC_IRQ_SYSTICK);
}
