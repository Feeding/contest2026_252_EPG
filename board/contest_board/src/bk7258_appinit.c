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

#include <nuttx/arch.h>
#include <nuttx/board.h>

#include "arm_internal.h"
#include "bk7258_memorymap.h"

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
#ifdef CONFIG_MMCSD
#  include <nuttx/sdio.h>
#  include <nuttx/mmcsd.h>
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
      extern void bk7258_gpio_setaf(int pin, uint8_t af, bool input);
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
      up_mdelay(5);

      /* DVP sensors also need MCLK before they will talk.  GPIO27
       * alternate 1 = CLK_AUXS_CIS; 480 MHz source (sel 3) divided by
       * (19 + 1) = 24 MHz, gated by cis_auxs cken (reg0x0d bit 9).
       * The SDK always powers the whole video path first (VIDP domain,
       * JPEG/yuv/h264 clocks) -- copied wholesale, cheap insurance.
       */

        {
          extern bool bk7258_gpio_read(int pin);
          int hits;
          int n;
          bool last;
          bool cur;

          /* First calibrate the meter itself: software toggling of the
           * pad must be visible through the input path, or every later
           * reading is meaningless.
           */

          bk7258_gpio_config(27, true, false, false);

          /* gpio_config strips INPUT_EN on outputs; the loopback meter
           * needs it back or the input latch never samples the pad.
           */

          modifyreg32(0x44000400 + 27 * 4, 0, 1u << 2);
          hits = 0;
          last = bk7258_gpio_read(27);

          for (n = 0; n < 200; n++)
            {
              bk7258_gpio_write(27, (n & 1) != 0);
              cur = bk7258_gpio_read(27);
              if (cur != last)
                {
                  hits++;
                  last = cur;
                }
            }

          syslog(LOG_INFO, "camera: probe-selftest=%d/200\n", hits);

          /* Video power + every clock the SDK touches.  cksel_jpeg /
           * clkdiv_jpeg live in clk_div_mode1 (reg0x08 bits 30 / 26-29)
           * -- the identically named pair in reg0x0a is something else.
           */

          modifyreg32(BK7258_SYS_BASE + (0x10 << 2), 1u << 7, 0);
          modifyreg32(BK7258_SYS_BASE + (0x0c << 2), 0,
                      (1u << 28) | (1u << 23));
          modifyreg32(BK7258_SYS_BASE + (0x0d << 2), 0,
                      (1u << 0) | (1u << 3) | (1u << 8) | (1u << 9));
          modifyreg32(BK7258_SYS_BASE + (0x08 << 2),
                      (0xfu << 26) | (1u << 30),
                      (1u << 26) | (1u << 30));
          modifyreg32(BK7258_SYS_BASE + (0x0a << 2),
                      (3u << 15) | (0x1fu << 17),
                      (3u << 15) | (19u << 17));

          /* yuv_buf: release reset, then YUV mode with mclk_div=DIV_6
           * -- the SDK's step 4 "encode start" is the only plausible
           * gate left for the func-0 MCLK route.
           */

          putreg32(0, 0x48020008);
          putreg32(3, 0x48020008);      /* release + clk_gate_bypass */
          modifyreg32(0x48020010, 3u << 10, (1u << 0) | (1u << 10));

          /* Route A: pad function 0 = JPEG_MCLK (yuv_buf divided). */

          bk7258_gpio_setaf(27, 0, true);
          hits = 0;
          last = bk7258_gpio_read(27);

          for (n = 0; n < 20000; n++)
            {
              cur = bk7258_gpio_read(27);
              if (cur != last)
                {
                  hits++;
                  last = cur;
                }
            }

          syslog(LOG_INFO, "camera: routeA(jpeg_mclk)=%d/20000\n", hits);

          /* Route B: pad function 1 = CLK_AUXS_CIS.  Sweep all four
           * source selects with the big divider: a slow source (crystal
           * 26M / 20 = 1.3 MHz) is unambiguously CPU-sampleable, so any
           * nonzero row proves the path end-to-end; all-zero rows mean
           * the clock never reaches the pad at all.
           */

          bk7258_gpio_setaf(27, 1, true);

            {
              int sel;

              for (sel = 0; sel < 4; sel++)
                {
                  modifyreg32(BK7258_SYS_BASE + (0x0a << 2),
                              (3u << 15) | (0x1fu << 17),
                              ((uint32_t)sel << 15) | (19u << 17));
                  up_udelay(100);

                  hits = 0;
                  last = bk7258_gpio_read(27);

                  for (n = 0; n < 20000; n++)
                    {
                      cur = bk7258_gpio_read(27);
                      if (cur != last)
                        {
                          hits++;
                          last = cur;
                        }
                    }

                  syslog(LOG_INFO, "camera: auxs sel=%d hits=%d/20000\n",
                         sel, hits);
                }
            }

          /* Park on sel=3 (480M/20 = 24 MHz), the SDK's choice. */

          modifyreg32(BK7258_SYS_BASE + (0x0a << 2),
                      (3u << 15) | (0x1fu << 17),
                      (3u << 15) | (19u << 17));
        }

      /* GPIO28: identity uncertain (reset vs power-down).  A DVP PWDN
       * is active high, so hold it LOW this round; the reset hypothesis
       * had its chance and produced silence.
       */

      bk7258_gpio_config(28, true, false, false);
      bk7258_gpio_write(28, false);
      up_mdelay(10);

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

#ifdef CONFIG_MMCSD
    {
      extern struct sdio_dev_s *bk7258_sdio_initialize(void);
      struct sdio_dev_s *sdio = bk7258_sdio_initialize();

      ret = mmcsd_slotinitialize(0, sdio);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: mmcsd_slotinitialize: %d\n", ret);
        }
    }
#endif

  return OK;
}
