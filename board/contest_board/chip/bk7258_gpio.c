/****************************************************************************
 * board/contest_board/chip/bk7258_gpio.c
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

#include <stdbool.h>
#include <stdint.h>

#include "arm_internal.h"
#include "bk7258_gpio.h"
#include "chip.h"

#include <nuttx/irq.h>
#include <assert.h>

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_gpio_setaf
 ****************************************************************************/

void bk7258_gpio_setaf(int pin, uint8_t af, bool input)
{
  uint32_t regval;

  if (pin < 0 || pin >= BK7258_NGPIOS)
    {
      return;
    }

  /* Select which peripheral function the pin carries.  This field lives in
   * the system register block.
   */

  regval  = getreg32(BK7258_GPIO_FUNCMODE(pin));
  regval &= ~GPIO_FUNCMODE_MASK(pin);
  regval |= ((uint32_t)af << GPIO_FUNCMODE_SHIFT(pin)) &
            GPIO_FUNCMODE_MASK(pin);
  putreg32(regval, BK7258_GPIO_FUNCMODE(pin));

  /* Hand the pin over to the peripheral.  The GPIO input/output drivers must
   * be off, otherwise they fight the peripheral for the pad.
   */

  /* Match the pad state the Beken bootloader proves out: its UART0 setup
   * writes 0x7C to the RX pad and 0x78 to TX.  Both hand the pad to the
   * peripheral with the GPIO output driver off and a pull-up on; RX also
   * keeps the input path enabled, without which the peripheral can drive the
   * pad but never see it.  An earlier version cleared INPUT_EN on every AF
   * pad, which silently made every UART transmit-only.
   */

  regval  = getreg32(BK7258_GPIO_CFG(pin));
  regval &= ~GPIO_CFG_INPUT_EN;
  regval |= GPIO_CFG_OUTPUT_DIS | GPIO_CFG_FUNC_EN |
            GPIO_CFG_PULL_EN | GPIO_CFG_PULL_UP;

  if (input)
    {
      regval |= GPIO_CFG_INPUT_EN;
    }

  putreg32(regval, BK7258_GPIO_CFG(pin));
}

/****************************************************************************
 * Name: bk7258_gpio_config
 ****************************************************************************/

void bk7258_gpio_config(int pin, bool output, bool pullup, bool pulldown)
{
  uint32_t regval;

  if (pin < 0 || pin >= BK7258_NGPIOS)
    {
      return;
    }

  regval = getreg32(BK7258_GPIO_CFG(pin));

  /* Take the pin back from any peripheral that was driving it. */

  regval &= ~(GPIO_CFG_FUNC_EN | GPIO_CFG_INPUT_EN | GPIO_CFG_OUTPUT_DIS |
              GPIO_CFG_PULL_EN | GPIO_CFG_PULL_UP);

  /* Drive the pad by leaving GPIO_CFG_OUTPUT_DIS clear; an input pin gets the
   * opposite of both bits.  This mirrors the vendor io_mode field, which is
   * bits 3:2 taken together: 0b00 selects output, 0b11 selects input.
   */

  regval |= output ? 0 : (GPIO_CFG_OUTPUT_DIS | GPIO_CFG_INPUT_EN);

  if (pullup)
    {
      regval |= GPIO_CFG_PULL_EN | GPIO_CFG_PULL_UP;
    }
  else if (pulldown)
    {
      regval |= GPIO_CFG_PULL_EN;
    }

  putreg32(regval, BK7258_GPIO_CFG(pin));
}

/****************************************************************************
 * Name: bk7258_gpio_write
 ****************************************************************************/

void bk7258_gpio_write(int pin, bool value)
{
  uint32_t regval;

  if (pin < 0 || pin >= BK7258_NGPIOS)
    {
      return;
    }

  regval = getreg32(BK7258_GPIO_CFG(pin));

  if (value)
    {
      regval |= GPIO_CFG_OUTPUT;
    }
  else
    {
      regval &= ~GPIO_CFG_OUTPUT;
    }

  putreg32(regval, BK7258_GPIO_CFG(pin));
}

/****************************************************************************
 * Name: bk7258_gpio_read
 ****************************************************************************/

bool bk7258_gpio_read(int pin)
{
  if (pin < 0 || pin >= BK7258_NGPIOS)
    {
      return false;
    }

  return (getreg32(BK7258_GPIO_CFG(pin)) & GPIO_CFG_INPUT) != 0;
}

/****************************************************************************
 * GPIO interrupts
 *
 * Every pin interrupt arrives on one NVIC line.  The latched status lives
 * in two write-1-to-clear words (REG_0x40/0x41 of the always-on GPIO
 * block); the dispatcher reads them, clears exactly what it saw, and calls
 * the handlers for the set bits.  Clearing before dispatch means an edge
 * that lands during a handler latches again and re-enters -- the same
 * clear-then-drain discipline the UART interrupt settled on.
 ****************************************************************************/

static struct
{
  void (*handler)(int pin, void *arg);
  void *arg;
} g_gpio_isr[BK7258_NGPIOS];

static int bk7258_gpio_dispatch(int irq, void *context, void *arg)
{
  uint32_t lo = getreg32(BK7258_GPIO_INTST_0_31);
  uint32_t hi = getreg32(BK7258_GPIO_INTST_32_55);
  int pin;

  if (lo != 0)
    {
      putreg32(lo, BK7258_GPIO_INTST_0_31);
    }

  if (hi != 0)
    {
      putreg32(hi, BK7258_GPIO_INTST_32_55);
    }

  for (pin = 0; pin < BK7258_NGPIOS; pin++)
    {
      bool hit = (pin < 32) ? ((lo >> pin) & 1) : ((hi >> (pin - 32)) & 1);

      if (hit && g_gpio_isr[pin].handler != NULL)
        {
          g_gpio_isr[pin].handler(pin, g_gpio_isr[pin].arg);
        }
    }

  return OK;
}

/****************************************************************************
 * Name: bk7258_gpio_setint
 ****************************************************************************/

int bk7258_gpio_setint(int pin, uint8_t type,
                       void (*handler)(int pin, void *arg), void *arg)
{
  static bool dispatcher_ready;
  irqstate_t flags;
  uint32_t regval;

  if (pin < 0 || pin >= BK7258_NGPIOS)
    {
      return -EINVAL;
    }

  flags = up_irq_save();

  if (!dispatcher_ready)
    {
      irq_attach(BK7258_IRQ_GPIO, bk7258_gpio_dispatch, NULL);
      up_enable_irq(BK7258_IRQ_GPIO);
      dispatcher_ready = true;
    }

  g_gpio_isr[pin].handler = handler;
  g_gpio_isr[pin].arg     = arg;

  regval  = getreg32(BK7258_GPIO_CFG(pin));
  regval &= ~(GPIO_CFG_INT_TYPE_MASK | GPIO_CFG_INT_EN);

  if (handler != NULL)
    {
      regval |= ((uint32_t)type << GPIO_CFG_INT_TYPE_SHIFT) & GPIO_CFG_INT_TYPE_MASK;
      regval |= GPIO_CFG_INT_EN;
    }

  putreg32(regval | GPIO_CFG_INT_CLEAR, BK7258_GPIO_CFG(pin));

  up_irq_restore(flags);
  return OK;
}
