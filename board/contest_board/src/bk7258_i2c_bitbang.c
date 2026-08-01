/****************************************************************************
 * board/contest_board/src/bk7258_i2c_bitbang.c
 *
 * Bit-banged I2C on GPIO42 (SCL) / GPIO43 (SDA).
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

#include <nuttx/i2c/i2c_bitbang.h>

#include "bk7258_gpio.h"

#ifdef CONFIG_I2C_BITBANG

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The vendor project declares a software I2C on exactly these two pins
 * (CONFIG_SIM_I2C_SCL_GPIO_ID=42 / SDA=43 in both CPU configs) without
 * naming the slave.  This board has two unaccounted-for I2C parts -- the
 * SC7A20 accelerometer and the NFC front end -- and probing is cheaper
 * than guessing: this bus exists so a scan can say who actually lives
 * here.
 *
 * Open drain is emulated the only way this GPIO block allows: driving low
 * is a real output, "high" is the output turned off with the pull-up left
 * on, which also keeps the input path readable for clock stretching.
 */

#define PIN_SCL  42
#define PIN_SDA  43

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void bb_pin_release(int pin)
{
  /* Input with pull-up: the bus (or the slave) owns the line. */

  bk7258_gpio_config(pin, false, true, false);
}

static void bb_pin_low(int pin)
{
  bk7258_gpio_config(pin, true, false, false);
  bk7258_gpio_write(pin, false);
}

static void bb_initialize(struct i2c_bitbang_lower_dev_s *lower)
{
  bb_pin_release(PIN_SCL);
  bb_pin_release(PIN_SDA);
}

static void bb_set_scl(struct i2c_bitbang_lower_dev_s *lower, bool high)
{
  if (high)
    {
      bb_pin_release(PIN_SCL);
    }
  else
    {
      bb_pin_low(PIN_SCL);
    }
}

static void bb_set_sda(struct i2c_bitbang_lower_dev_s *lower, bool high)
{
  if (high)
    {
      bb_pin_release(PIN_SDA);
    }
  else
    {
      bb_pin_low(PIN_SDA);
    }
}

static bool bb_get_scl(struct i2c_bitbang_lower_dev_s *lower)
{
  return bk7258_gpio_read(PIN_SCL);
}

static bool bb_get_sda(struct i2c_bitbang_lower_dev_s *lower)
{
  return bk7258_gpio_read(PIN_SDA);
}

static const struct i2c_bitbang_lower_ops_s g_bb_ops =
{
  .initialize = bb_initialize,
  .set_scl    = bb_set_scl,
  .set_sda    = bb_set_sda,
  .get_scl    = bb_get_scl,
  .get_sda    = bb_get_sda,
};

static struct i2c_bitbang_lower_dev_s g_bb_lower =
{
  .ops  = &g_bb_ops,
  .priv = NULL,
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_i2c_bitbang_initialize
 ****************************************************************************/

struct i2c_master_s *bk7258_i2c_bitbang_initialize(void)
{
  return i2c_bitbang_initialize(&g_bb_lower);
}

#endif /* CONFIG_I2C_BITBANG */
