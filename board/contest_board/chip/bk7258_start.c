/****************************************************************************
 * board/contest_board/chip/bk7258_start.c
 *
 * BK7258 reset entry point.
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

#include <nuttx/arch.h>
#include <nuttx/board.h>
#include <nuttx/init.h>

#include "arm_internal.h"
#include "nvic.h"

#include "bk7258_lowputc.h"
#include "bk7258_clockconfig.h"
#include "bk7258_gpio.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define HEAP_BASE  ((uintptr_t)_ebss + CONFIG_IDLETHREAD_STACKSIZE)

#ifdef CONFIG_DEBUG_FEATURES
#  define showprogress(c) arm_lowputc(c)
#else
#  define showprogress(c)
#endif

/* Bring-up marker.
 *
 * These push a byte straight into the UART0 TX FIFO.
 *
 * Do not expect them to work before bk7258_lowsetup() has run.  Disassembling
 * the Beken bootloader shows it tears UART0 down on the way out: the call at
 * 0x02000ac2, reached unconditionally just before the jump, ends up in a
 * routine that writes both the UART0 block (0x44820000) and the system clock
 * registers (0x44010000).  An earlier version of this port assumed the
 * bootloader left the console running and put markers at the very top of
 * __start(); those markers can only have written into an ungated peripheral.
 *
 * Nothing here may touch .data or .bss: both are still uninitialised when the
 * first marker runs.
 */

#ifdef CONFIG_BK7258_EARLY_MARKERS
#  define earlymark_at(base, c) \
     do \
       { \
         volatile uint32_t *_st = \
           (volatile uint32_t *)((base) + BK7258_UART_FIFO_STATUS_OFFSET); \
         volatile uint32_t *_tx = \
           (volatile uint32_t *)((base) + BK7258_UART_FIFO_PORT_OFFSET); \
         uint32_t _spin = 100000; \
         while ((*_st & UART_FIFO_STATUS_WR_READY) == 0 && --_spin); \
         *_tx = (uint32_t)(unsigned char)(c); \
       } \
     while (0)
#  define earlymark(c) earlymark_at(BK7258_UART0_BASE, c)
#else
#  define earlymark_at(base, c)
#  define earlymark(c)
#endif

/****************************************************************************
 * Public Data
 ****************************************************************************/

/* The IDLE thread stack sits immediately above .bss; the heap starts above
 * it.  up_allocate_heap() in bk7258_allocateheap.c uses this value.
 */

const uintptr_t g_idle_topstack = HEAP_BASE;

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: __start
 *
 * Description:
 *   This is the reset entry point.  It is reached from the reset vector in
 *   arch/arm/src/arm_m/arm_vectors.c after the stack pointers have been
 *   initialised.
 *
 *   The BK7258 boot ROM and the Beken second-stage bootloader have already
 *   brought up the flash controller and mapped this image for
 *   execute-in-place before control arrives here, so the only memory setup
 *   still required is the usual .data/.bss preparation.
 *
 ****************************************************************************/

void __start(void)
{
  const uint32_t *src;
  uint32_t *dest;

  /* First instructions that produce observable output: they prove the
   * bootloader actually transferred control to this image, and they tell us
   * which security state it handed over in.
   *
   * 'N' goes to the non-secure alias of UART0 and 'S' to the secure one.  A
   * non-secure handover makes the secure alias fault, so seeing only 'N'
   * means this port is wrong to assume CONFIG_SPE-style addressing; 'NS'
   * means secure, and neither means the bootloader never jumped here.
   */

  earlymark_at(BK7258_UART0_BASE + BK7258_S_NS_ADDR_DIFF, 'N');
  earlymark_at(BK7258_UART0_BASE, 'S');

#ifdef CONFIG_BK7258_LED_PROBE
  /* Bring-up probe: does the bootloader actually hand control to this image?
   *
   * Drives three pins the board schematic ties to things a person can see or
   * feel, then loops.  GPIO9 is the one that removes all guesswork: the motor
   * driver on sheet 4 is wired to a net literally named "P9", so there is no
   * inference between net label and pin number.  GPIO40/GPIO41 are the likely
   * LED1/LED2 pins, but that mapping is read off label ordering rather than an
   * explicit pin number, so it is the weaker signal of the two.
   *
   * Uses only the always-on GPIO block: no clock gate, no pin mux, no UART,
   * and no assumption about whether SYSRESETREQ reboots this SoC.  All three
   * pins drive passive loads (LED resistors, a transistor base), so nothing
   * here can contend with another device.
   */

  {
    volatile uint32_t *motor = (volatile uint32_t *)BK7258_GPIO_CFG(9);
    volatile uint32_t *led1  = (volatile uint32_t *)BK7258_GPIO_CFG(40);
    volatile uint32_t *led2  = (volatile uint32_t *)BK7258_GPIO_CFG(41);
    uint32_t phase = 0;

    for (; ; )
      {
        volatile uint32_t spin;
        uint32_t hi = GPIO_CFG_OUTPUT_EN | GPIO_CFG_OUTPUT;
        uint32_t lo = GPIO_CFG_OUTPUT_EN;

        *motor = phase ? hi : lo;
        *led1  = phase ? hi : lo;
        *led2  = phase ? lo : hi;

        for (spin = 0; spin < 2000000; spin++)
          {
          }

        phase ^= 1;
      }
  }
#endif

#ifdef CONFIG_BK7258_SELFRESET_PROBE
  /* Bring-up probe: does the bootloader actually hand control to this image?
   *
   * Nothing that depends on a working UART can answer that, because a silent
   * console is exactly the symptom under investigation.  Instead, sit for
   * roughly half a second and then reset.  If this code runs, the board loops
   * through the bootloader about twice a second, so its UART download window
   * reopens continuously and tools/bk_flash.py links up without anyone
   * touching the RST button.  If a manual reset is still required, this code
   * never ran.
   *
   * Only the ARMv8-M reset register is touched: no pin is driven, so there is
   * no way for this to fight another device on the board.
   */

  for (; ; )
    {
      volatile uint32_t spin;

      for (spin = 0; spin < 3000000; spin++)
        {
        }

      putreg32(NVIC_AIRCR_VECTKEY | NVIC_AIRCR_SYSRESETREQ, NVIC_AIRCR);

      __asm__ __volatile__ ("dsb 0xf" : : : "memory");
    }
#endif

  /* Keep interrupts masked until nx_start() is ready to take them. */

  __asm__ __volatile__ ("cpsid i" : : : "memory");

  /* Point the vector table at our own vectors.  The bootloader will have
   * left VTOR pointing at its own table.
   */

  putreg32((uint32_t)_vectors, NVIC_VECTAB);

  earlymark('2');

  /* Enable the FPU before any code that might touch floating point. */

#ifdef CONFIG_ARCH_FPU
  arm_fpuconfig();
#endif

  earlymark('3');

  /* Clear .bss.  The linker guarantees both bounds are 4-byte aligned. */

  for (dest = (uint32_t *)_sbss; dest < (uint32_t *)_ebss; )
    {
      *dest++ = 0;
    }

  /* Copy .data from its load address in flash into SRAM. */

  for (src = (const uint32_t *)_eronly, dest = (uint32_t *)_sdata;
       dest < (uint32_t *)_edata;
      )
    {
      *dest++ = *src++;
    }

  earlymark('4');

  /* Bring up the clocks we depend on, then the debug console so that any
   * failure after this point is visible on the serial port.
   */

  bk7258_clockconfig();

  earlymark('5');

  bk7258_lowsetup();

  earlymark('6');

  showprogress('A');

  /* Perform board-specific initialisation that has to happen before the
   * NuttX subsystems come up.
   */

#ifdef CONFIG_BOARD_EARLY_INITIALIZE
  board_early_initialize();
#endif

  showprogress('B');

  /* Start NuttX.  This does not return. */

  nx_start();

  showprogress('X');

  for (; ; );
}
