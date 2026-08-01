/****************************************************************************
 * board/contest_board/chip/bk7258_uart.h
 *
 * BK7258 UART register definitions.
 *
 * The register layout mirrors uart_hw_t in the Beken SDK
 * (bk_idk release/v2.0.1, middleware/soc/bk7258/soc/uart_struct.h).  The SDK
 * describes the registers by word index (REG_0x00..REG_0x09); the byte
 * offsets below are those indices multiplied by four.
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

#ifndef __BOARD_CONTEST_BOARD_CHIP_BK7258_UART_H
#define __BOARD_CONTEST_BOARD_CHIP_BK7258_UART_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "bk7258_memorymap.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The UART clock is the 26 MHz high-speed crystal (XTALH).  This matches
 * UART_CLOCK == CONFIG_XTAL_FREQ == 26000000 in the Beken SDK.
 */

#define BK7258_UART_CLOCK       26000000

/* Register offsets *********************************************************/

#define BK7258_UART_DEVID_OFFSET        0x0000  /* Device ID */
#define BK7258_UART_VERSION_OFFSET      0x0004  /* Device version */
#define BK7258_UART_GLOBAL_CTRL_OFFSET  0x0008  /* Global control */
#define BK7258_UART_DEVSTATUS_OFFSET    0x000c  /* Device status */
#define BK7258_UART_CONFIG_OFFSET       0x0010  /* Line configuration */
#define BK7258_UART_FIFO_CONFIG_OFFSET  0x0014  /* FIFO configuration */
#define BK7258_UART_FLOW_CTRL_OFFSET    0x0028  /* Flow control config */
#define BK7258_UART_WAKE_CONFIG_OFFSET  0x002c  /* Wake configuration */
#define BK7258_UART_FIFO_STATUS_OFFSET  0x0018  /* FIFO status */
#define BK7258_UART_FIFO_PORT_OFFSET    0x001c  /* FIFO data port */
#define BK7258_UART_INT_ENABLE_OFFSET   0x0020  /* Interrupt enable */
#define BK7258_UART_INT_STATUS_OFFSET   0x0024  /* Interrupt status */

/* Global control register **************************************************/

#define UART_GLOBAL_SOFT_RESET      (1 << 0)  /* Bit 0:  Soft reset */
#define UART_GLOBAL_CLKGATE_BYPASS  (1 << 1)  /* Bit 1:  Bypass clock gate */

/* Line configuration register **********************************************/

#define UART_CONFIG_TX_ENABLE       (1 << 0)  /* Bit 0:  Transmit enable */
#define UART_CONFIG_RX_ENABLE       (1 << 1)  /* Bit 1:  Receive enable */
#define UART_CONFIG_IRDA_MODE       (1 << 2)  /* Bit 2:  0=UART, 1=IrDA */

#define UART_CONFIG_DATABITS_SHIFT  (3)       /* Bits 3-4: Data bits */
#define UART_CONFIG_DATABITS_MASK   (3 << UART_CONFIG_DATABITS_SHIFT)
#  define UART_CONFIG_DATABITS_5    (0 << UART_CONFIG_DATABITS_SHIFT)
#  define UART_CONFIG_DATABITS_6    (1 << UART_CONFIG_DATABITS_SHIFT)
#  define UART_CONFIG_DATABITS_7    (2 << UART_CONFIG_DATABITS_SHIFT)
#  define UART_CONFIG_DATABITS_8    (3 << UART_CONFIG_DATABITS_SHIFT)

#define UART_CONFIG_PARITY_ENABLE   (1 << 5)  /* Bit 5:  Parity enable */
#define UART_CONFIG_PARITY_ODD      (1 << 6)  /* Bit 6:  0=even, 1=odd */
#define UART_CONFIG_STOPBITS_2      (1 << 7)  /* Bit 7:  0=1 stop, 1=2 stop */

#define UART_CONFIG_CLKDIV_SHIFT    (8)       /* Bits 8-23: Clock divider */
#define UART_CONFIG_CLKDIV_MASK     (0xffff << UART_CONFIG_CLKDIV_SHIFT)
#define UART_CONFIG_CLKDIV(n)       ((uint32_t)(n) << UART_CONFIG_CLKDIV_SHIFT)

/* Baud rate is UART_CLOCK / (clk_div + 1); round to nearest to minimise
 * error (uart_ll.h in the Beken SDK derives the baud the same way).
 */

#define UART_CLKDIV(baud) \
  (((BK7258_UART_CLOCK + ((baud) / 2)) / (baud)) - 1)

/* FIFO configuration register **********************************************/

#define UART_FIFO_CONFIG_TXTHR_SHIFT  (0)     /* Bits 0-7:  TX threshold */
#define UART_FIFO_CONFIG_TXTHR_MASK   (0xff << UART_FIFO_CONFIG_TXTHR_SHIFT)
#define UART_FIFO_CONFIG_TXTHR(n) \
  ((uint32_t)(n) << UART_FIFO_CONFIG_TXTHR_SHIFT)

#define UART_FIFO_CONFIG_RXTHR_SHIFT  (8)     /* Bits 8-15: RX threshold */
#define UART_FIFO_CONFIG_RXTHR_MASK   (0xff << UART_FIFO_CONFIG_RXTHR_SHIFT)
#define UART_FIFO_CONFIG_RXTHR(n) \
  ((uint32_t)(n) << UART_FIFO_CONFIG_RXTHR_SHIFT)

#define UART_FIFO_CONFIG_STOPDET_SHIFT (16)   /* Bits 16-17: RX stop detect */
#define UART_FIFO_CONFIG_STOPDET_MASK  (3 << UART_FIFO_CONFIG_STOPDET_SHIFT)
#  define UART_FIFO_CONFIG_STOPDET_32  (0 << UART_FIFO_CONFIG_STOPDET_SHIFT)
#  define UART_FIFO_CONFIG_STOPDET_64  (1 << UART_FIFO_CONFIG_STOPDET_SHIFT)
#  define UART_FIFO_CONFIG_STOPDET_128 (2 << UART_FIFO_CONFIG_STOPDET_SHIFT)
#  define UART_FIFO_CONFIG_STOPDET_256 (3 << UART_FIFO_CONFIG_STOPDET_SHIFT)

/* FIFO status register *****************************************************/

#define UART_FIFO_STATUS_TXCNT_SHIFT  (0)     /* Bits 0-7:  TX FIFO count */
#define UART_FIFO_STATUS_TXCNT_MASK   (0xff << UART_FIFO_STATUS_TXCNT_SHIFT)
#define UART_FIFO_STATUS_RXCNT_SHIFT  (8)     /* Bits 8-15: RX FIFO count */
#define UART_FIFO_STATUS_RXCNT_MASK   (0xff << UART_FIFO_STATUS_RXCNT_SHIFT)

#define UART_FIFO_STATUS_TX_FULL      (1 << 16) /* Bit 16: TX FIFO full */
#define UART_FIFO_STATUS_TX_EMPTY     (1 << 17) /* Bit 17: TX FIFO empty */
#define UART_FIFO_STATUS_RX_FULL      (1 << 18) /* Bit 18: RX FIFO full */
#define UART_FIFO_STATUS_RX_EMPTY     (1 << 19) /* Bit 19: RX FIFO empty */
#define UART_FIFO_STATUS_RXCNT_SHIFT  (8)       /* Bits 8-15: RX FIFO count */
#define UART_FIFO_STATUS_RXCNT_MASK   (0xff << UART_FIFO_STATUS_RXCNT_SHIFT)
#define UART_FIFO_STATUS_WR_READY     (1 << 20) /* Bit 20: Write ready */
#define UART_FIFO_STATUS_RD_READY     (1 << 21) /* Bit 21: Read ready */

/* FIFO data port register **************************************************/

#define UART_FIFO_PORT_TXDATA_SHIFT   (0)     /* Bits 0-7:  TX data in */
#define UART_FIFO_PORT_TXDATA_MASK    (0xff << UART_FIFO_PORT_TXDATA_SHIFT)
#define UART_FIFO_PORT_RXDATA_SHIFT   (8)     /* Bits 8-15: RX data out */
#define UART_FIFO_PORT_RXDATA_MASK    (0xff << UART_FIFO_PORT_RXDATA_SHIFT)

/* Interrupt enable / interrupt status registers ****************************/

#define UART_INT_TX_NEED_WRITE      (1 << 0)  /* Bit 0: TX FIFO below thresh */
#define UART_INT_RX_NEED_READ       (1 << 1)  /* Bit 1: RX FIFO above thresh */
#define UART_INT_RX_OVERFLOW        (1 << 2)  /* Bit 2: RX FIFO overflow */
#define UART_INT_RX_PARITY_ERR      (1 << 3)  /* Bit 3: RX parity error */
#define UART_INT_RX_STOPBITS_ERR    (1 << 4)  /* Bit 4: RX stop bit error */
#define UART_INT_TX_FINISH          (1 << 5)  /* Bit 5: TX complete */
#define UART_INT_RX_FINISH          (1 << 6)  /* Bit 6: RX complete */
#define UART_INT_RXD_WAKEUP         (1 << 7)  /* Bit 7: RX wakeup */

/* All receive-side interrupt sources */

#define UART_INT_RX_ALL \
  (UART_INT_RX_NEED_READ | UART_INT_RX_FINISH)

/* All error interrupt sources */

#define UART_INT_ERROR_ALL \
  (UART_INT_RX_OVERFLOW | UART_INT_RX_PARITY_ERR | UART_INT_RX_STOPBITS_ERR)

#endif /* __BOARD_CONTEST_BOARD_CHIP_BK7258_UART_H */
