/****************************************************************************
 * board/contest_board/chip/bk7258_memorymap.h
 *
 * BK7258 memory map and peripheral base addresses.
 *
 * Addresses are taken from the Beken SDK (bk_idk release/v2.0.1),
 * include/soc/bk7258/reg_base.h.
 *
 * CPU0 runs as the Secure Processing Environment (CONFIG_SPE=1 in the
 * Beken SDK), so SOC_ADDR_OFFSET is 0 and the secure aliases below are the
 * addresses used directly.  The non-secure alias of any address is the
 * secure address + BK7258_S_NS_ADDR_DIFF.
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

#ifndef __BOARD_CONTEST_BOARD_CHIP_BK7258_MEMORYMAP_H
#define __BOARD_CONTEST_BOARD_CHIP_BK7258_MEMORYMAP_H

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Distance between the secure and non-secure alias of any address */

#define BK7258_S_NS_ADDR_DIFF   0x10000000ul

/* Tightly coupled memories (16 KB each) */

#define BK7258_ITCM_BASE        0x00000000ul
#define BK7258_DTCM_BASE        0x20000000ul

/* Execute-in-place flash and mask ROM */

#define BK7258_FLASH_BASE       0x02000000ul
#define BK7258_ROM_BASE         0x06000000ul

/* Shared SRAM.
 *
 * The six SRAM banks are physically contiguous and total 640 KB.  The same
 * physical memory is visible through four windows; 0x08000000/0x18000000 are
 * the instruction views and 0x28000000/0x38000000 the data views.
 */

#define BK7258_SRAM_IBASE       0x08000000ul  /* Instruction view */
#define BK7258_SRAM_BASE        0x28000000ul  /* Data view */
#define BK7258_SRAM_SIZE        0x000a0000ul  /* 640 KB */
#define BK7258_SRAM_END         (BK7258_SRAM_BASE + BK7258_SRAM_SIZE)

/* Offset from the SRAM data view to the SRAM instruction view */

#define BK7258_SRAM_IOFFSET     (BK7258_SRAM_BASE - BK7258_SRAM_IBASE)

/* Individual SRAM banks */

#define BK7258_SRAM0_BASE       0x28000000ul  /* 64 KB  */
#define BK7258_SRAM1_BASE       0x28010000ul  /* 64 KB  */
#define BK7258_SRAM2_BASE       0x28020000ul  /* 128 KB */
#define BK7258_SRAM3_BASE       0x28040000ul  /* 128 KB */
#define BK7258_SRAM4_BASE       0x28060000ul  /* 128 KB */
#define BK7258_SRAM5_BASE       0x28080000ul  /* 128 KB */

/* SiP PSRAM */

#define BK7258_PSRAM_BASE       0x60000000ul
#define BK7258_QSPI0_BASE       0x64000000ul
#define BK7258_QSPI1_BASE       0x68000000ul

/* Peripheral register blocks */

#define BK7258_AON_PMU_BASE     0x44000000ul
#define BK7258_AON_RTC_BASE     0x44000200ul
#define BK7258_AON_GPIO_BASE    0x44000400ul
#define BK7258_AON_WDT_BASE     0x44000600ul
#define BK7258_SYS_BASE         0x44010000ul

/* Per-CPU interrupt routing matrix inside the system block: one enable bit
 * per NVIC line, 32 lines per register starting at word 0x20.  A line only
 * reaches the NVIC while its bit here is set; the bit index equals the NVIC
 * line number.
 */

#define BK7258_SYS_CPU0_INT_EN(n)   (BK7258_SYS_BASE + (0x20 << 2) + (((n) >> 5) << 2))
#define BK7258_FLASH_REG_BASE   0x44030000ul
#define BK7258_WDT_BASE         0x44800000ul
#define BK7258_TIMER0_BASE      0x44810000ul
#define BK7258_UART0_BASE       0x44820000ul
#define BK7258_SPI0_BASE        0x44870000ul
#define BK7258_EFUSE_BASE       0x44880000ul
#define BK7258_TIMER1_BASE      0x45800000ul
#define BK7258_UART1_BASE       0x45830000ul
#define BK7258_UART2_BASE       0x45840000ul
#define BK7258_I2C0_BASE        0x45850000ul
#define BK7258_I2C1_BASE        0x45860000ul
#define BK7258_SPI1_BASE        0x45880000ul
#define BK7258_PWM_BASE         0x458a0000ul

#endif /* __BOARD_CONTEST_BOARD_CHIP_BK7258_MEMORYMAP_H */
