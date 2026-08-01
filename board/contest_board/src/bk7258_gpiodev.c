/****************************************************************************
 * board/contest_board/src/bk7258_gpiodev.c
 *
 * /dev/gpio0: the vibration motor's drive transistor on GPIO9.
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

#include <nuttx/ioexpander/gpio.h>

#include "bk7258_gpio.h"

#ifdef CONFIG_DEV_GPIO

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The motor transistor's base hangs off GPIO9 (net "P9" on the schematic;
 * the vendor firmware drives it with PWM3 at 1 kHz / 30%).  Full on-off
 * through /dev/gpio0 is enough to run the motor; PWM speed control can come
 * later.  Power for the motor comes from the shared 3.3 V rail that
 * board_late_initialize() switches on through GPIO52.
 */

#define MOTOR_PIN  9

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int gpout_read(struct gpio_dev_s *dev, bool *value);
static int gpout_write(struct gpio_dev_s *dev, bool value);
static int gpout_setpintype(struct gpio_dev_s *dev,
                            enum gpio_pintype_e pintype);

static const struct gpio_operations_s g_gpout_ops =
{
  .go_read       = gpout_read,
  .go_write      = gpout_write,
  .go_attach     = NULL,
  .go_enable     = NULL,
  .go_setpintype = gpout_setpintype,
};

static struct gpio_dev_s g_motor_gpio =
{
  .gp_pintype = GPIO_OUTPUT_PIN,
  .gp_ops     = &g_gpout_ops,
};

static bool g_motor_state;

static int gpout_read(struct gpio_dev_s *dev, bool *value)
{
  *value = g_motor_state;
  return OK;
}

static int gpout_write(struct gpio_dev_s *dev, bool value)
{
  g_motor_state = value;
  bk7258_gpio_write(MOTOR_PIN, value);
  return OK;
}

static int gpout_setpintype(struct gpio_dev_s *dev,
                            enum gpio_pintype_e pintype)
{
  return (pintype == GPIO_OUTPUT_PIN) ? OK : -ENOTSUP;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_gpiodev_initialize
 ****************************************************************************/

int bk7258_gpiodev_initialize(void)
{
  bk7258_gpio_config(MOTOR_PIN, true, false, false);
  bk7258_gpio_write(MOTOR_PIN, false);

  return gpio_pin_register(&g_motor_gpio, 0);
}

#endif /* CONFIG_DEV_GPIO */
