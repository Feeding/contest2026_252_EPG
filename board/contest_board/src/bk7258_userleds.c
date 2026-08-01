/****************************************************************************
 * board/contest_board/src/bk7258_userleds.c
 *
 * The bi-colour LED: red on GPIO40, green on GPIO41, driven high to light.
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

#include <nuttx/board.h>
#include <arch/board/board.h>

#include "bk7258_gpio.h"

#ifdef CONFIG_USERLED

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Pin numbers straight from the vendor firmware's own LED driver for this
 * board (led_blink.h): the power-on mux table labels these pads LCD_G4/G3,
 * but that is template residue -- the board's screens are SPI panels and
 * the pads really do drive the two LED halves.
 */

static const int g_led_pins[BOARD_NLEDS] =
{
  40,   /* BOARD_LED_RED */
  41,   /* BOARD_LED_GREEN */
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: board_userled_initialize
 ****************************************************************************/

uint32_t board_userled_initialize(void)
{
  int i;

  for (i = 0; i < BOARD_NLEDS; i++)
    {
      bk7258_gpio_config(g_led_pins[i], true, false, false);
      bk7258_gpio_write(g_led_pins[i], false);
    }

  return BOARD_NLEDS;
}

/****************************************************************************
 * Name: board_userled
 ****************************************************************************/

void board_userled(int led, bool ledon)
{
  if (led >= 0 && led < BOARD_NLEDS)
    {
      bk7258_gpio_write(g_led_pins[led], ledon);
    }
}

/****************************************************************************
 * Name: board_userled_all
 ****************************************************************************/

void board_userled_all(uint32_t ledset)
{
  int i;

  for (i = 0; i < BOARD_NLEDS; i++)
    {
      bk7258_gpio_write(g_led_pins[i], (ledset & (1 << i)) != 0);
    }
}

#endif /* CONFIG_USERLED */
