/****************************************************************************
 * board/contest_board/src/bk7258_gc9d01.c
 *
 * GC9D01 160x160 SPI panel -- screen 1 of the two robot eyes.
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
#include <string.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/arch.h>
#include <nuttx/board.h>
#include <nuttx/lcd/lcd.h>
#include <nuttx/spi/spi.h>

#include "bk7258_gpio.h"

#if defined(CONFIG_LCD) && defined(CONFIG_BK7258_SPI1)

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Wiring, cross-checked between the board schematic (sheet 5, LCD1) and
 * the SoC pin map, and anchored by the already-verified key pins on the
 * same sheet: SPI1 carries SCK/CS/MOSI on GPIO2/3/4, the panel's D/C line
 * is GPIO5 (SPI1's MISO pad -- the panel is write-only), reset is GPIO45,
 * and the shared backlight transistor is driven by GPIO25, active high.
 * Panel power rides the GPIO52-gated 3.3 V rail that boot switches on.
 */

#define PIN_BACKLIGHT 25   /* Shared by both panels (one transistor) */

#define GC9D01_XRES   160
#define GC9D01_YRES   160

#define GC9D01_CMD_CASET   0x2a
#define GC9D01_CMD_RASET   0x2b
#define GC9D01_CMD_RAMWR   0x2c

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct gc9d01_dev_s
{
  struct lcd_dev_s dev;
  struct spi_dev_s *spi;
  uint8_t dc_pin;
  uint8_t rst_pin;
  uint8_t power;

  /* One line of RGB565 pixels, byte-swapped for the wire. */

  uint8_t runbuf[GC9D01_XRES * 2];
};

/****************************************************************************
 * Private Data (init table)
 ****************************************************************************/

/* The manufacturer initialisation sequence for the GC9D01(N), taken from a
 * same-panel reference driver (LilyGO T-Circle-S3, esp_lcd_gc9d01n.c) --
 * the vendor firmware for this board keeps its copy in a repository that
 * is not public.  Bytes are verbatim; the entries are mostly analogue
 * tuning behind the 0xFE/0xEF register-access unlock, plus COLMOD 0x3A =
 * 0x05 (RGB565), MADCTL 0x36 = 0x00, sleep-out 0x11 with the mandatory
 * 200 ms settle, then display-on 0x29.
 */

struct gc9d01_cmd_s
{
  uint8_t cmd;
  uint8_t len;
  uint8_t delay_ms;
  const uint8_t *data;
};

static const uint8_t d_74[] = { 0x02, 0x0e, 0x00, 0x00, 0x00, 0x00, 0x00 };
static const uint8_t d_b5[] = { 0x0d, 0x0d };
static const uint8_t d_60[] = { 0x38, 0x0f, 0x79, 0x67 };
static const uint8_t d_61[] = { 0x38, 0x11, 0x79, 0x67 };
static const uint8_t d_64[] = { 0x38, 0x17, 0x71, 0x5f, 0x79, 0x67 };
static const uint8_t d_65[] = { 0x38, 0x13, 0x71, 0x5b, 0x79, 0x67 };
static const uint8_t d_6a[] = { 0x00, 0x00 };
static const uint8_t d_6c[] = { 0x22, 0x02, 0x22, 0x02, 0x22, 0x22, 0x50 };
static const uint8_t d_6e[] =
{
  0x03, 0x03, 0x01, 0x01, 0x00, 0x00, 0x0f, 0x0f,
  0x0d, 0x0d, 0x0b, 0x0b, 0x09, 0x09, 0x00, 0x00,
  0x00, 0x00, 0x0a, 0x0a, 0x0c, 0x0c, 0x0e, 0x0e,
  0x10, 0x10, 0x00, 0x00, 0x02, 0x02, 0x04, 0x04
};
static const uint8_t d_9b[] = { 0x3b, 0x93, 0x33, 0x7f, 0x00 };
static const uint8_t d_70[] = { 0x0d, 0x02, 0x08, 0x0d, 0x02, 0x08 };
static const uint8_t d_71[] = { 0x0d, 0x02, 0x08 };
static const uint8_t d_91[] = { 0x0e, 0x09 };
static const uint8_t d_c3[] = { 0x19, 0xc4, 0x19, 0xc9, 0x3c };
static const uint8_t d_f0[] = { 0x53, 0x15, 0x0a, 0x04, 0x00, 0x3e };
static const uint8_t d_f1[] = { 0x56, 0xa8, 0x7f, 0x33, 0x34, 0x5f };
static const uint8_t d_f2[] = { 0x53, 0x15, 0x0a, 0x04, 0x00, 0x3a };
static const uint8_t d_f3[] = { 0x52, 0xa4, 0x7f, 0x33, 0x34, 0xdf };
static const uint8_t d_one_ff[] = { 0xff };
static const uint8_t d_3a[] = { 0x05 };
static const uint8_t d_ec[] = { 0x01 };
static const uint8_t d_98[] = { 0x3e };
static const uint8_t d_bf[] = { 0x01 };
static const uint8_t d_f9[] = { 0x40 };
static const uint8_t d_7e[] = { 0x30 };
static const uint8_t d_36[] = { 0x00 };

static const struct gc9d01_cmd_s g_init_cmds[] =
{
  { 0xfe, 0, 0,   NULL },       /* Inter-register enable 1 */
  { 0xef, 0, 0,   NULL },       /* Inter-register enable 2 */
  { 0x80, 1, 0,   d_one_ff },
  { 0x81, 1, 0,   d_one_ff },
  { 0x82, 1, 0,   d_one_ff },
  { 0x84, 1, 0,   d_one_ff },
  { 0x85, 1, 0,   d_one_ff },
  { 0x86, 1, 0,   d_one_ff },
  { 0x87, 1, 0,   d_one_ff },
  { 0x88, 1, 0,   d_one_ff },
  { 0x89, 1, 0,   d_one_ff },
  { 0x8a, 1, 0,   d_one_ff },
  { 0x8b, 1, 0,   d_one_ff },
  { 0x8c, 1, 0,   d_one_ff },
  { 0x8d, 1, 0,   d_one_ff },
  { 0x8e, 1, 0,   d_one_ff },
  { 0x8f, 1, 0,   d_one_ff },
  { 0x3a, 1, 0,   d_3a },       /* COLMOD: RGB565 */
  { 0xec, 1, 0,   d_ec },
  { 0x74, 7, 0,   d_74 },
  { 0x98, 1, 0,   d_98 },
  { 0x99, 1, 0,   d_98 },
  { 0xb5, 2, 0,   d_b5 },
  { 0x60, 4, 0,   d_60 },
  { 0x61, 4, 0,   d_61 },
  { 0x64, 6, 0,   d_64 },
  { 0x65, 6, 0,   d_65 },
  { 0x6a, 2, 0,   d_6a },
  { 0x6c, 7, 0,   d_6c },
  { 0x6e, 32, 0,  d_6e },
  { 0xbf, 1, 0,   d_bf },
  { 0xf9, 1, 0,   d_f9 },
  { 0x9b, 5, 0,   d_9b },
  { 0x7e, 1, 0,   d_7e },
  { 0x70, 6, 0,   d_70 },
  { 0x71, 3, 0,   d_71 },
  { 0x91, 2, 0,   d_91 },
  { 0xc3, 5, 0,   d_c3 },
  { 0xf0, 6, 0,   d_f0 },
  { 0xf1, 6, 0,   d_f1 },
  { 0xf2, 6, 0,   d_f2 },
  { 0xf3, 6, 0,   d_f3 },
  { 0x36, 1, 0,   d_36 },       /* MADCTL */
  { 0x11, 0, 200, NULL },       /* Sleep out + mandatory settle */
  { 0x29, 0, 0,   NULL },       /* Display on */
  { 0x2c, 0, 20,  NULL },       /* Prime RAMWR */
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* Panel 0: the right eye, on hardware SPI1 (SCK/CS/MOSI = GPIO2/3/4),
 * D/C on GPIO5, reset on GPIO45.  Panel 1: the left eye, on the software
 * SPI (GPIO22/23/24), D/C on GPIO7, reset on GPIO6.  Wiring per the board
 * schematic sheet 5, cross-anchored by the verified key pins.
 */

static struct gc9d01_dev_s g_gc9d01[2] =
{
  { .dc_pin = 5, .rst_pin = 45 },
  { .dc_pin = 7, .rst_pin = 6  },
};

/****************************************************************************
 * Name: gc9d01_cmd / gc9d01_data
 *
 * Description:
 *   4-line SPI: the D/C GPIO selects between command (low) and data (high)
 *   bytes.  Each SPI transfer is CS-framed by the hardware, which this
 *   panel accepts even mid-sequence.
 *
 ****************************************************************************/

static void gc9d01_cmd(struct gc9d01_dev_s *priv, uint8_t cmd)
{
  bk7258_gpio_write(priv->dc_pin, false);
  SPI_SNDBLOCK(priv->spi, &cmd, 1);
}

static void gc9d01_data(struct gc9d01_dev_s *priv, const uint8_t *data,
                        size_t len)
{
  if (len > 0)
    {
      bk7258_gpio_write(priv->dc_pin, true);
      SPI_SNDBLOCK(priv->spi, data, len);
    }
}

/****************************************************************************
 * Name: gc9d01_setwindow
 ****************************************************************************/

static void gc9d01_setwindow(struct gc9d01_dev_s *priv, uint16_t x0,
                             uint16_t y0, uint16_t x1, uint16_t y1)
{
  uint8_t win[4];

  win[0] = x0 >> 8;
  win[1] = x0 & 0xff;
  win[2] = x1 >> 8;
  win[3] = x1 & 0xff;
  gc9d01_cmd(priv, GC9D01_CMD_CASET);
  gc9d01_data(priv, win, 4);

  win[0] = y0 >> 8;
  win[1] = y0 & 0xff;
  win[2] = y1 >> 8;
  win[3] = y1 & 0xff;
  gc9d01_cmd(priv, GC9D01_CMD_RASET);
  gc9d01_data(priv, win, 4);

  gc9d01_cmd(priv, GC9D01_CMD_RAMWR);
}

/****************************************************************************
 * LCD lower-half operations
 ****************************************************************************/

static int gc9d01_putrun(struct lcd_dev_s *dev, fb_coord_t row,
                         fb_coord_t col, const uint8_t *buffer,
                         size_t npixels)
{
  struct gc9d01_dev_s *priv = (struct gc9d01_dev_s *)dev;
  const uint16_t *src = (const uint16_t *)buffer;
  size_t i;

  if (npixels == 0 || row >= GC9D01_YRES || col + npixels > GC9D01_XRES)
    {
      return -EINVAL;
    }

  /* The panel wants each RGB565 pixel high byte first; the framebuffer is
   * little-endian.  Swap while staging one run.
   */

  for (i = 0; i < npixels; i++)
    {
      priv->runbuf[2 * i]     = src[i] >> 8;
      priv->runbuf[2 * i + 1] = src[i] & 0xff;
    }

  gc9d01_setwindow(priv, col, row, col + npixels - 1, row);
  bk7258_gpio_write(priv->dc_pin, true);
  SPI_SNDBLOCK(priv->spi, priv->runbuf, npixels * 2);

  return OK;
}

/****************************************************************************
 * Name: gc9d01_putarea
 *
 * Description:
 *   Blast a whole rectangle in one panel transaction: window and RAMWR go
 *   out once, then nothing but pixel bytes.  The per-row path pays five
 *   small command transfers per line -- 160x per full screen -- which is
 *   what made the first bring-up visibly crawl.
 *
 ****************************************************************************/

static int gc9d01_putarea(struct lcd_dev_s *dev, fb_coord_t row_start,
                          fb_coord_t row_end, fb_coord_t col_start,
                          fb_coord_t col_end, const uint8_t *buffer,
                          fb_coord_t stride)
{
  struct gc9d01_dev_s *priv = (struct gc9d01_dev_s *)dev;
  fb_coord_t row;
  size_t width = col_end - col_start + 1;
  const uint16_t *src;
  size_t i;

  gc9d01_setwindow(priv, col_start, row_start, col_end, row_end);
  bk7258_gpio_write(priv->dc_pin, true);

  for (row = row_start; row <= row_end; row++)
    {
      src = (const uint16_t *)(buffer + (row - row_start) * stride);

      for (i = 0; i < width; i++)
        {
          priv->runbuf[2 * i]     = src[i] >> 8;
          priv->runbuf[2 * i + 1] = src[i] & 0xff;
        }

      SPI_SNDBLOCK(priv->spi, priv->runbuf, width * 2);
    }

  return OK;
}

static int gc9d01_getrun(struct lcd_dev_s *dev, fb_coord_t row,
                         fb_coord_t col, uint8_t *buffer, size_t npixels)
{
  return -ENOSYS;   /* Write-only wiring: the panel has no MISO. */
}

static int gc9d01_getvideoinfo(struct lcd_dev_s *dev,
                               struct fb_videoinfo_s *vinfo)
{
  vinfo->fmt     = FB_FMT_RGB16_565;
  vinfo->xres    = GC9D01_XRES;
  vinfo->yres    = GC9D01_YRES;
  vinfo->nplanes = 1;
  return OK;
}

static int gc9d01_getplaneinfo(struct lcd_dev_s *dev, unsigned int planeno,
                               struct lcd_planeinfo_s *pinfo)
{
  struct gc9d01_dev_s *priv = (struct gc9d01_dev_s *)dev;

  memset(pinfo, 0, sizeof(*pinfo));
  pinfo->putrun  = gc9d01_putrun;
  pinfo->putarea = gc9d01_putarea;
  pinfo->getrun  = gc9d01_getrun;
  pinfo->buffer = priv->runbuf;
  pinfo->bpp    = 16;
  pinfo->dev    = dev;
  return OK;
}

static int gc9d01_getpower(struct lcd_dev_s *dev)
{
  struct gc9d01_dev_s *priv = (struct gc9d01_dev_s *)dev;

  return priv->power;
}

static int gc9d01_setpower(struct lcd_dev_s *dev, int power)
{
  struct gc9d01_dev_s *priv = (struct gc9d01_dev_s *)dev;

  /* One transistor lights both panels: the backlight goes off only when
   * neither panel wants power.
   */

  priv->power = power;
  bk7258_gpio_write(PIN_BACKLIGHT,
                    g_gc9d01[0].power > 0 || g_gc9d01[1].power > 0);
  return OK;
}

static int gc9d01_getcontrast(struct lcd_dev_s *dev)
{
  return -ENOSYS;
}

static int gc9d01_setcontrast(struct lcd_dev_s *dev, unsigned int contrast)
{
  return -ENOSYS;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: board_lcd_initialize
 ****************************************************************************/

int board_lcd_initialize(void)
{
  extern struct spi_dev_s *bk7258_spibus_initialize(int port);
  extern struct spi_dev_s *bk7258_swspi_initialize(void);
  extern struct spi_dev_s *bk7258_qspibus_initialize(int port);
  const struct gc9d01_cmd_s *c;
  struct gc9d01_dev_s *priv;
  unsigned int i;
  int panel;

  /* Both eyes prefer their QSPI unit -- the vendor's own architecture --
   * and each has a fallback that keeps the eye lit if the unit does not
   * answer: the GSPI block for the right eye (functional but with a
   * still-undiagnosed slow clock), the bit-banged master for the left.
   */

  g_gc9d01[0].spi = bk7258_qspibus_initialize(1);

  if (g_gc9d01[0].spi == NULL)
    {
      g_gc9d01[0].spi = bk7258_spibus_initialize(1);
    }

  g_gc9d01[1].spi = bk7258_qspibus_initialize(0);

  if (g_gc9d01[1].spi == NULL)
    {
      g_gc9d01[1].spi = bk7258_swspi_initialize();
    }

  bk7258_gpio_config(PIN_BACKLIGHT, true, false, false);
  bk7258_gpio_write(PIN_BACKLIGHT, false);

  for (panel = 0; panel < 2; panel++)
    {
      priv = &g_gc9d01[panel];

      priv->dev.getvideoinfo = gc9d01_getvideoinfo;
      priv->dev.getplaneinfo = gc9d01_getplaneinfo;
      priv->dev.getpower     = gc9d01_getpower;
      priv->dev.setpower     = gc9d01_setpower;
      priv->dev.getcontrast  = gc9d01_getcontrast;
      priv->dev.setcontrast  = gc9d01_setcontrast;

      if (priv->spi == NULL)
        {
          return -ENODEV;
        }

      bk7258_gpio_config(priv->dc_pin, true, false, false);
      bk7258_gpio_config(priv->rst_pin, true, false, false);

      /* Hardware reset: high 10 ms, low 10 ms, high, long settle. */

      bk7258_gpio_write(priv->rst_pin, true);
      up_mdelay(10);
      bk7258_gpio_write(priv->rst_pin, false);
      up_mdelay(10);
      bk7258_gpio_write(priv->rst_pin, true);
      up_mdelay(120);

      /* Manufacturer initialisation. */

      for (i = 0; i < sizeof(g_init_cmds) / sizeof(g_init_cmds[0]); i++)
        {
          c = &g_init_cmds[i];
          gc9d01_cmd(priv, c->cmd);
          gc9d01_data(priv, c->data, c->len);

          if (c->delay_ms != 0)
            {
              up_mdelay(c->delay_ms);
            }
        }
    }

  lcdinfo("GC9D01 x2 initialised\n");
  return OK;
}

/****************************************************************************
 * Name: board_lcd_getdev
 ****************************************************************************/

struct lcd_dev_s *board_lcd_getdev(int lcddev)
{
  if (lcddev >= 0 && lcddev < 2)
    {
      return &g_gc9d01[lcddev].dev;
    }

  return NULL;
}

/****************************************************************************
 * Name: board_lcd_uninitialize
 ****************************************************************************/

void board_lcd_uninitialize(void)
{
  gc9d01_setpower(&g_gc9d01[0].dev, 0);
  gc9d01_setpower(&g_gc9d01[1].dev, 0);
}

#endif /* CONFIG_LCD && CONFIG_BK7258_SPI1 */
