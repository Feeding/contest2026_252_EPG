/****************************************************************************
 * board/contest_board/chip/bk7258_idle.c
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

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: up_idle
 *
 * Description:
 *   Idle without WFI.
 *
 *   The generic ARM idle loop executes WFI, and on this SoC that is not a
 *   plain core halt: the interactive console died reproducibly at the same
 *   point once the system first went idle -- two characters echoed, then
 *   nothing was ever received again -- and the UART's wake controls
 *   (wake_config, which this port zeroes the way the bootloader does) decide
 *   what can bring the fabric back.  The bootloader itself never sleeps; it
 *   busy-polls, so its configuration is only known to be safe for a machine
 *   that never executes WFI.
 *
 *   Until the sleep and wake plumbing is brought up deliberately (via the
 *   vendor pm driver's register set), idling must not power anything down:
 *   spin instead.  This costs power, not correctness, and this board is
 *   USB-fed on a bench.
 *
 ****************************************************************************/

void up_idle(void)
{
}
