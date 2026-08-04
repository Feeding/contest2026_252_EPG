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

#include <nuttx/irq.h>
#include <nuttx/spinlock.h>

#ifdef CONFIG_BK7258_WDT_NMI
#  include <assert.h>
#  include <debug.h>
#endif

#include "arm_internal.h"
#include "nvic.h"

#include "bk7258_memorymap.h"
#include "bk7258_reset_reason.h"
#include "bk7258_wdt.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifdef CONFIG_BK7258_WDT_NMI

/* The peripheral-domain watchdog, the one that raises NMI.  Layout from the
 * vendor SDK's wdt_struct.h: word 2 is a global control word whose bit 1
 * bypasses the block's clock gate, word 4 is the counter, taking the same
 * unlock/commit key pair as the always-on block.
 */

#define BK7258_NMI_WDT_GLOBAL_CTRL  (BK7258_WDT_BASE + (2 << 2))
#define BK7258_NMI_WDT_CTRL         (BK7258_WDT_BASE + (4 << 2))
#define BK7258_NMI_WDT_CLKGATE_BYP  (1 << 1)

/* Device clock enable in the system block; bit 31 is the CPU watchdog.  The
 * bit number is CLK_PWR_ID_WDG_CPU's position in the vendor's
 * dev_clk_pwr_id_t (sys_types.h), and the register is the one
 * sys_ll_set_cpu_device_clk_enable_value() writes -- SOC_SYS_REG_BASE +
 * (0xc << 2).  bk7258_audio.c and bk7258_pwm.c already drive other bits of
 * it, so the address is proven reachable.
 */

#define BK7258_SYS_CLK_ENABLE       (BK7258_SYS_BASE + (0xc << 2))
#define BK7258_SYS_CLK_EN_WDG_CPU   (1u << 31)

/* Counter rate.  The vendor ships CONFIG_INT_WDT_PERIOD_MS=8000 for this
 * part and wdt_ll_set_period() doubles the millisecond count when the clock
 * divider is /16 (the value bk_wdt_driver_init() programs), so a period
 * register of 16000 means 8 s: the block counts at 2 kHz.  Everything below
 * is derived from that, not measured -- see the verification note in
 * PORTING_NOTES.
 */

#define BK7258_NMI_WDT_HZ           2000
#define BK7258_NMI_WDT_MS(ms)       ((ms) * BK7258_NMI_WDT_HZ / 1000)

/* Normal period.  Has to clear the 100 ms heartbeat by a wide margin so a
 * momentarily late feed cannot fire it, and stay under the always-on
 * block's ~65 s so this stage is always the one that bites first.  8 s is
 * what the vendor uses.
 */

#define BK7258_NMI_WDT_PERIOD_RUN   BK7258_NMI_WDT_MS(8000)

/* Period armed once a /dev/watchdog0 deadline has passed: short enough that
 * the panic follows the missed ping promptly.
 */

#define BK7258_NMI_WDT_PERIOD_BITE  BK7258_NMI_WDT_MS(100)

#endif /* CONFIG_BK7258_WDT_NMI */

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Deadline handed over by the /dev/watchdog0 lower half, in system ticks.
 * Zero means nobody has claimed the dog and the heartbeat just feeds.
 */

static volatile clock_t g_wdt_deadline;
static volatile bool    g_wdt_claimed;

#ifdef CONFIG_BK7258_WDT_NMI

/* Set once bk7258_wdt_nmi_initialize() has clocked the peripheral block.
 * Until then nothing may touch 0x44800000 -- see the brick note below.
 */

static volatile bool g_nmi_wdt_live;

/* WDIOC_CAPTURE handler, or NULL for the default record-and-panic path. */

static xcpt_t g_nmi_wdt_capture;

#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

#ifdef CONFIG_BK7258_WDT_NMI

/****************************************************************************
 * Name: bk7258_nmi_wdt_arm
 *
 * Description:
 *   Arm or feed the NMI watchdog.  Same unlock/commit pair as the always-on
 *   block.  A period of zero stops it.
 *
 ****************************************************************************/

static void bk7258_nmi_wdt_arm(uint32_t period)
{
  if (!g_nmi_wdt_live)
    {
      return;
    }

  period &= 0xffff;

  putreg32(0x5a0000 | period, BK7258_NMI_WDT_CTRL);
  putreg32(0xa50000 | period, BK7258_NMI_WDT_CTRL);
}

/****************************************************************************
 * Name: bk7258_nmi_wdt_handler
 *
 * Description:
 *   The NMI exception.  Reached when the heartbeat has stopped for longer
 *   than the NMI period, or when a /dev/watchdog0 deadline has passed.
 *
 *   NMI is what makes this stage worth having: on Cortex-M it is outside
 *   PRIMASK and BASEPRI, so it lands even from inside a critical section or
 *   a spin with interrupts masked -- exactly the case the xTS watchdog case
 *   requires a driver to survive.
 *
 *   Order matters here.  The bite is recorded before anything can fail, the
 *   NMI block is stopped so a second exception cannot arrive mid-dump, and
 *   the always-on block is re-armed to a period that outlasts the dump.  The
 *   reset then comes from the always-on watchdog, which is the truth: it
 *   really was a watchdog reset, and going through board_reset() instead
 *   would overwrite the reason with "reboot".
 *
 ****************************************************************************/

static int bk7258_nmi_wdt_handler(int irq, FAR void *context, FAR void *arg)
{
  xcpt_t capture = g_nmi_wdt_capture;

  if (capture != NULL)
    {
      /* Somebody asked to be told rather than reset -- WDIOC_CAPTURE.  Feed
       * both blocks first so the notification cannot turn into a reset while
       * the handler runs, then hand over.
       */

      bk7258_nmi_wdt_arm(BK7258_NMI_WDT_PERIOD_RUN);
      bk7258_wdt_arm(BK7258_WDT_PERIOD_RUN);

      return capture(irq, context, arg);
    }

  UNUSED(arg);

  bk7258_reset_reason_set(BK7258_RESET_WATCHDOG);

  bk7258_nmi_wdt_arm(0);
  bk7258_wdt_arm(BK7258_WDT_PERIOD_DUMP);

  PANIC();

  return OK;
}

#endif /* CONFIG_BK7258_WDT_NMI */

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

/****************************************************************************
 * Name: bk7258_wdt_deadline_set
 ****************************************************************************/

void bk7258_wdt_deadline_set(clock_t deadline)
{
  irqstate_t flags;

  flags = enter_critical_section();

  if (deadline == 0)
    {
      g_wdt_claimed  = false;
      g_wdt_deadline = 0;
    }
  else
    {
      g_wdt_deadline = deadline;
      g_wdt_claimed  = true;
    }

  leave_critical_section(flags);
}

/****************************************************************************
 * Name: bk7258_wdt_service
 ****************************************************************************/

void bk7258_wdt_service(void)
{
  if (g_wdt_claimed &&
      (sclock_t)(clock_systime_ticks() - g_wdt_deadline) >= 0)
    {
      /* Userspace stopped pinging.  Arming a short period rather than
       * merely withholding the feed is what makes the timeout mean what the
       * caller asked for: the running period is ~65 s, so a silent stop
       * would defer the reset by up to that long past a 5 s timeout.
       */

#ifdef CONFIG_BK7258_WDT_NMI
      /* Let the NMI stage take it: it fires in ~100 ms, records the bite and
       * dumps, and the always-on block -- re-armed from the handler -- does
       * the reset.  The always-on period stays long here on purpose, so it
       * cannot cut the dump short.
       */

      bk7258_nmi_wdt_arm(BK7258_NMI_WDT_PERIOD_BITE);
      bk7258_wdt_arm(BK7258_WDT_PERIOD_DUMP);
#else
      bk7258_wdt_arm(BK7258_WDT_PERIOD_BOOT);
#endif
      return;
    }

#ifdef CONFIG_BK7258_WDT_NMI
  bk7258_nmi_wdt_arm(BK7258_NMI_WDT_PERIOD_RUN);
#endif

  bk7258_wdt_arm(BK7258_WDT_PERIOD_RUN);
}

/****************************************************************************
 * Name: bk7258_wdt_nmi_initialize
 ****************************************************************************/

void bk7258_wdt_nmi_initialize(void)
{
#ifdef CONFIG_BK7258_WDT_NMI
  uint32_t regval;

  if (g_nmi_wdt_live)
    {
      return;
    }

  /* Clock the block before addressing it.  The first build of the always-on
   * path also wrote 0x44800010 from the top of __start() and bricked the
   * board into total silence -- no marker, no trace, no reset -- which is
   * what an APB access to an unclocked block looks like.  The clock-enable
   * register is in the system block, which is reachable by this point.
   */

  regval = getreg32(BK7258_SYS_CLK_ENABLE);
  putreg32(regval | BK7258_SYS_CLK_EN_WDG_CPU, BK7258_SYS_CLK_ENABLE);

  regval = getreg32(BK7258_NMI_WDT_GLOBAL_CTRL);
  putreg32(regval | BK7258_NMI_WDT_CLKGATE_BYP, BK7258_NMI_WDT_GLOBAL_CTRL);

  /* Attaching before the first arm also pre-allocates the vector slot.  The
   * minimal vector table is in dynamic mode, where an unmapped IRQ would
   * otherwise claim its slot from inside the exception -- a bad place to
   * discover the table is full (PORTING_NOTES chapter 15).
   */

  irq_attach(NVIC_IRQ_NMI, bk7258_nmi_wdt_handler, NULL);

  g_nmi_wdt_live = true;

  bk7258_nmi_wdt_arm(BK7258_NMI_WDT_PERIOD_RUN);

  binfo("NMI watchdog armed, period %u counts at %u Hz\n",
        (unsigned)BK7258_NMI_WDT_PERIOD_RUN, (unsigned)BK7258_NMI_WDT_HZ);
#endif
}

/****************************************************************************
 * Name: bk7258_wdt_capture_set
 ****************************************************************************/

xcpt_t bk7258_wdt_capture_set(xcpt_t handler)
{
#ifdef CONFIG_BK7258_WDT_NMI
  irqstate_t flags = enter_critical_section();
  xcpt_t previous = g_nmi_wdt_capture;

  g_nmi_wdt_capture = handler;

  leave_critical_section(flags);
  return previous;
#else
  UNUSED(handler);
  return NULL;
#endif
}
