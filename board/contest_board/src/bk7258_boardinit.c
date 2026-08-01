/****************************************************************************
 * board/contest_board/src/bk7258_boardinit.c
 *
 * Board bring-up for the Agora ConvoAI Kit R1 (Beken BK7258).
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

#include <nuttx/arch.h>
#include <nuttx/board.h>

#include "bk7258_wdt.h"

#include "arm_internal.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: board_early_initialize
 *
 * Description:
 *   Called from __start() once .data and .bss are in place and the console
 *   is alive, but before nx_start().
 *
 ****************************************************************************/

#ifdef CONFIG_BOARD_EARLY_INITIALIZE
void board_early_initialize(void)
{
  /* The console pins and clocks are set up by the chip layer.  Nothing else
   * on this board has to be touched this early.
   */
}
#endif

/****************************************************************************
 * Name: board_late_initialize
 *
 * Description:
 *   Called by NuttX after the scheduler is running, so it may use the full
 *   driver API.
 *
 ****************************************************************************/

#ifdef CONFIG_BOARD_LATE_INITIALIZE
void bk7258_serial_monitor_start(void);

void board_late_initialize(void)
{
  bk7258_serial_monitor_start();

#ifdef CONFIG_FS_PROCFS
  /* Mounting procfs is handled by the init script, not here. */
#endif
}
#endif

/****************************************************************************
 * Name: board_reset
 *
 * Description:
 *   Reset the board.  Wired to boardctl(BOARDIOC_RESET) and therefore to
 *   the NSH `reboot` command.
 *
 *   The reset goes through the watchdog because that is the only mechanism
 *   that actually resets this SoC -- the vendor SDK never writes AIRCR and
 *   SYSRESETREQ was tried on hardware and does nothing.  A reboot lands in
 *   the Beken bootloader, whose UART download window is what the flashing
 *   tools wait for, so `reboot` doubles as the no-hands way to make the
 *   board flashable.
 *
 ****************************************************************************/

#ifdef CONFIG_BOARDCTL_RESET
int board_reset(int status)
{
  UNUSED(status);
  bk7258_wdt_reboot();
  return 0;
}
#endif
