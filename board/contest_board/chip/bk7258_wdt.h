/****************************************************************************
 * board/contest_board/chip/bk7258_wdt.h
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

#ifndef __BOARD_CONTEST_BOARD_CHIP_BK7258_WDT_H
#define __BOARD_CONTEST_BOARD_CHIP_BK7258_WDT_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/clock.h>

#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The watchdog is the only reset mechanism that works on this SoC: the
 * vendor SDK never touches AIRCR, and both its bk_reboot() and the Beken
 * bootloader restart the chip by arming a short watchdog period and
 * spinning.  SYSRESETREQ was tried on real hardware and does nothing.
 *
 * Two watchdogs are written in tandem, mirroring the bootloader routine at
 * 0x02000fa0 exactly: the always-on block at 0x44000600 and the peripheral
 * block's counter at 0x44800010.  Each write is unlock (0x5A in the top
 * byte) then commit (0xA5), period in the low 16 bits.
 */

#define BK7258_WDT_PERIOD_RUN    0xfffc  /* Generous heartbeat period */
#define BK7258_WDT_PERIOD_BOOT   6       /* Immediate reset, bootloader value */

/* How often the SysTick handler services the dog, in ticks.  The arm
 * sequence crosses onto the slow AON bus, so feeding on every tick cost the
 * eye animation about a third of its frame budget; decimating by ten made
 * that back.  This is also the granularity of any deadline built on top of
 * the heartbeat, which is why the /dev/watchdog0 lower half reads it here
 * instead of assuming a number.
 */

#define BK7258_WDT_HEARTBEAT_TICKS 10

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_wdt_arm
 *
 * Description:
 *   Arm (or re-arm) both watchdogs with the given period.  Feeding is the
 *   same operation as arming.
 *
 ****************************************************************************/

void bk7258_wdt_arm(uint32_t period);

/****************************************************************************
 * Name: bk7258_wdt_reboot
 *
 * Description:
 *   Reset the chip through the watchdog.  Does not return.  On the way back
 *   up the Beken bootloader reopens its UART download window, so a reboot
 *   is also how the board is made flashable without anyone pressing RST.
 *
 ****************************************************************************/

void bk7258_wdt_reboot(void) noreturn_function;

/****************************************************************************
 * Name: bk7258_wdt_service
 *
 * Description:
 *   The heartbeat, called from the SysTick handler.  Feeds the dog while
 *   the system is healthy, and stops feeding -- in fact arms the shortest
 *   period, so the reset is immediate rather than up to a hardware period
 *   away -- once a deadline handed over by the /dev/watchdog0 driver has
 *   passed.
 *
 *   With no deadline set this is exactly the original unconditional feed,
 *   which is the safety net that recovers the board from a wedged handler
 *   or a spin with interrupts masked.  That net is never removed; the
 *   userspace watchdog only adds a second, earlier reason to bite.
 *
 ****************************************************************************/

void bk7258_wdt_service(void);

/****************************************************************************
 * Name: bk7258_wdt_deadline_set
 *
 * Description:
 *   Hand the heartbeat a deadline, in system ticks, past which it must stop
 *   feeding.  Passing zero releases the claim and restores the plain
 *   always-feed behaviour.  Safe to call from any context.
 *
 ****************************************************************************/

void bk7258_wdt_deadline_set(clock_t deadline);

/****************************************************************************
 * Name: bk7258_wdt_lowerhalf_initialize
 *
 * Description:
 *   Register the AON watchdog with the NuttX watchdog upper half, normally
 *   as /dev/watchdog0.
 *
 * Returned Value:
 *   Zero on success; a negated errno value on failure.
 *
 ****************************************************************************/

#ifdef CONFIG_BK7258_WDT
int bk7258_wdt_lowerhalf_initialize(FAR const char *devpath);
#endif

#endif /* __BOARD_CONTEST_BOARD_CHIP_BK7258_WDT_H */
