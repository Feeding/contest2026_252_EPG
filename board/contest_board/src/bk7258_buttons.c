/****************************************************************************
 * board/contest_board/src/bk7258_buttons.c
 *
 * The three side keys: S1 volume-up GPIO13, S2 power GPIO12, S3 volume-down
 * GPIO8.  All active low with the internal pull-up.
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
#include <stdbool.h>

#include <nuttx/arch.h>
#include <nuttx/board.h>
#include <nuttx/irq.h>
#include <arch/board/board.h>

#include "bk7258_gpio.h"

#ifdef CONFIG_ARCH_BUTTONS

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const int g_button_pins[NUM_BUTTONS] =
{
  13,   /* BUTTON_S1, volume up   */
  12,   /* BUTTON_S2, power       */
  8,    /* BUTTON_S3, volume down */
};

static xcpt_t g_button_isr;
static void  *g_button_arg;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* All three keys funnel into the one attached handler, mirroring how the
 * upper half expects a shared button interrupt.  The trigger is both edges
 * -- the hardware has no both-edge type, so the edge is flipped on each
 * interrupt: wait for falling while released, for rising while pressed.
 * The vendor firmware never uses button interrupts at all (it polls at
 * 6 ms), so this is one of the few places this port cannot mirror it.
 */

static void bk7258_button_interrupt(int pin, void *arg)
{
  bool pressed = !bk7258_gpio_read(pin);

  bk7258_gpio_setint(pin,
                     pressed ? GPIO_INT_RISING_EDGE : GPIO_INT_FALLING_EDGE,
                     bk7258_button_interrupt, NULL);

  if (g_button_isr != NULL)
    {
      g_button_isr(0, NULL, g_button_arg);
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: board_button_initialize
 ****************************************************************************/

uint32_t board_button_initialize(void)
{
  int i;

  for (i = 0; i < NUM_BUTTONS; i++)
    {
      bk7258_gpio_config(g_button_pins[i], false, true, false);
    }

  return NUM_BUTTONS;
}

/****************************************************************************
 * Name: board_buttons
 ****************************************************************************/

uint32_t board_buttons(void)
{
  uint32_t state = 0;
  int i;

  for (i = 0; i < NUM_BUTTONS; i++)
    {
      if (!bk7258_gpio_read(g_button_pins[i]))
        {
          state |= (1 << i);
        }
    }

  return state;
}

/****************************************************************************
 * Name: board_button_irq
 ****************************************************************************/

#ifdef CONFIG_ARCH_IRQBUTTONS
int board_button_irq(int id, xcpt_t irqhandler, void *arg)
{
  int i;

  UNUSED(id);

  g_button_isr = irqhandler;
  g_button_arg = arg;

  for (i = 0; i < NUM_BUTTONS; i++)
    {
      if (irqhandler != NULL)
        {
          bk7258_gpio_setint(g_button_pins[i], GPIO_INT_FALLING_EDGE,
                             bk7258_button_interrupt, NULL);
        }
      else
        {
          bk7258_gpio_setint(g_button_pins[i], 0, NULL, NULL);
        }
    }

  return OK;
}
#endif

#endif /* CONFIG_ARCH_BUTTONS */
