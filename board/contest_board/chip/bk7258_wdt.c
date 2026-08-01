/****************************************************************************
 * board/contest_board/chip/bk7258_wdt.c
 *
 * Watchdog control for the BK7258.
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

#include "bk7258_memorymap.h"
#include "bk7258_wdt.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_wdt_arm
 ****************************************************************************/

void bk7258_wdt_arm(uint32_t period)
{
  period &= 0xffff;

  /* Only the always-on watchdog is written.  The first build of this file
   * also wrote the peripheral-domain counter at 0x44800010 -- copying the
   * bootloader routine -- and placing that write at the top of __start()
   * bricked the board into total silence: no marker, no trace, no reset,
   * which is the signature of an APB access to an unclocked block stalling
   * the bus.  The bootloader can write it because its own init has run; at
   * application entry nothing guarantees that block a clock.  The AON block
   * lives in the always-on domain, is the one the vendor's force-feed
   * routine treats as primary, and is sufficient to reset the SoC.
   */

  putreg32(0x5a0000 | period, BK7258_AON_WDT_BASE);
  putreg32(0xa50000 | period, BK7258_AON_WDT_BASE);
}

/****************************************************************************
 * Name: bk7258_wdt_reboot
 ****************************************************************************/

void bk7258_wdt_reboot(void)
{
  /* Mask interrupts so nothing can re-arm a longer period underneath us,
   * then let the shortest period expire.
   */

  __asm__ __volatile__ ("cpsid i" : : : "memory");

  bk7258_wdt_arm(BK7258_WDT_PERIOD_BOOT);

  for (; ; );
}
