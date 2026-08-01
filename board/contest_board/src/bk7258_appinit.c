/****************************************************************************
 * board/contest_board/src/bk7258_appinit.c
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

#include <sys/mount.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/board.h>

#ifdef CONFIG_USERLED_LOWER
#  include <nuttx/leds/userled.h>
#endif
#ifdef CONFIG_INPUT_BUTTONS_LOWER
#  include <nuttx/input/buttons.h>
#endif
#ifdef CONFIG_BK7258_I2C1
#  include <nuttx/i2c/i2c_master.h>
#endif
#ifdef CONFIG_VIDEO_FB
#  include <nuttx/video/fb.h>
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: board_app_initialize
 *
 * Description:
 *   Perform application-level initialisation.  Called by boardctl() with
 *   BOARDIOC_INIT, which NSH issues at start of day.
 *
 ****************************************************************************/

int board_app_initialize(uintptr_t arg)
{
  int ret;

  UNUSED(ret);

#ifdef CONFIG_FS_PROCFS
  ret = nx_mount(NULL, "/proc", "procfs", 0, NULL);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to mount procfs at /proc: %d\n", ret);
    }
#endif

#ifdef CONFIG_USERLED_LOWER
  ret = userled_lower_initialize("/dev/userleds");
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: userled_lower_initialize: %d\n", ret);
    }
#endif

#ifdef CONFIG_INPUT_BUTTONS_LOWER
  ret = btn_lower_initialize("/dev/buttons");
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: btn_lower_initialize: %d\n", ret);
    }
#endif

#if defined(CONFIG_BK7258_I2C1) && defined(CONFIG_I2C_DRIVER)
    {
      extern struct i2c_master_s *bk7258_i2cbus_initialize(int port);
      extern void bk7258_gpio_config(int pin, bool output, bool pullup,
                                     bool pulldown);
      extern void bk7258_gpio_write(int pin, bool value);
      struct i2c_master_s *i2c;
      int port;

      /* The camera's SCCB interface only answers with the sensor powered:
       * GPIO49 gates its supply (vendor: CONFIG_CAMERA_CTRL_POWER_GPIO_ID).
       */

      bk7258_gpio_config(49, true, false, false);
      bk7258_gpio_write(49, true);

      for (port = 0; port <= 1; port++)
        {
          i2c = bk7258_i2cbus_initialize(port);

          if (i2c == NULL)
            {
              syslog(LOG_ERR, "ERROR: i2c port %d init failed\n", port);
            }
          else
            {
              ret = i2c_register(i2c, port);
              if (ret < 0)
                {
                  syslog(LOG_ERR, "ERROR: i2c_register(%d): %d\n",
                         port, ret);
                }
            }
        }
    }
#endif

#if defined(CONFIG_I2C_BITBANG) && defined(CONFIG_I2C_DRIVER)
    {
      extern struct i2c_master_s *bk7258_i2c_bitbang_initialize(void);
      struct i2c_master_s *i2c = bk7258_i2c_bitbang_initialize();

      if (i2c != NULL)
        {
          ret = i2c_register(i2c, 2);
          if (ret < 0)
            {
              syslog(LOG_ERR, "ERROR: i2c_register(bitbang): %d\n", ret);
            }
        }
    }
#endif

#ifdef CONFIG_VIDEO_FB
  ret = fb_register(0, 0);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: fb_register(0): %d\n", ret);
    }

  ret = fb_register(1, 0);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: fb_register(1): %d\n", ret);
    }
#endif

#ifdef CONFIG_DEV_GPIO
  extern int bk7258_gpiodev_initialize(void);

  ret = bk7258_gpiodev_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: bk7258_gpiodev_initialize: %d\n", ret);
    }
#endif

  return OK;
}
