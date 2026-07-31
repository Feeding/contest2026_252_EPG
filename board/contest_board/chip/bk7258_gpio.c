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

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_gpio_setaf
 ****************************************************************************/

void bk7258_gpio_setaf(int pin, uint8_t af)
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

  regval  = getreg32(BK7258_GPIO_CFG(pin));
  regval &= ~(GPIO_CFG_INPUT_EN | GPIO_CFG_OUTPUT_EN);
  regval |= GPIO_CFG_FUNC_EN;
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

  regval &= ~(GPIO_CFG_FUNC_EN | GPIO_CFG_INPUT_EN | GPIO_CFG_OUTPUT_EN |
              GPIO_CFG_PULL_EN | GPIO_CFG_PULL_UP);

  regval |= output ? GPIO_CFG_OUTPUT_EN : GPIO_CFG_INPUT_EN;

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
