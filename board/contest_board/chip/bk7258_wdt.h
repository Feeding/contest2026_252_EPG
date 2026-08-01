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

#endif /* __BOARD_CONTEST_BOARD_CHIP_BK7258_WDT_H */
