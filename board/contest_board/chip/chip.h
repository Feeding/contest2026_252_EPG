/****************************************************************************
 * board/contest_board/chip/chip.h
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

#ifndef __BOARD_CONTEST_BOARD_CHIP_CHIP_H
#define __BOARD_CONTEST_BOARD_CHIP_CHIP_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <arch/irq.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifdef CONFIG_DEV_CONSOLE
#  undef USE_SERIALDRIVER
#  define USE_SERIALDRIVER 1
#endif

/* The BK7258 uses an Armv8-M STAR-MC1 core, which is architecturally
 * equivalent to a Cortex-M33.  The number of peripheral interrupts drives
 * the size of the vector table built by arch/arm/src/arm_m/arm_vectors.c.
 */

#define ARMV8M_PERIPHERAL_INTERRUPTS (NR_IRQS - NVIC_IRQ_FIRST)

#endif /* __BOARD_CONTEST_BOARD_CHIP_CHIP_H */
