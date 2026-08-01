/****************************************************************************
 * board/contest_board/src/bk7258_swspi.c
 *
 * Software SPI master for screen 2, whose wires land on QSPI0-only pads.
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

#include <nuttx/spi/spi.h>
#include <nuttx/mutex.h>

#include "arm_internal.h"
#include "bk7258_gpio.h"

#if defined(CONFIG_LCD) && defined(CONFIG_BK7258_SPI1)

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Screen 2's SCK/CS/MOSI sit on GPIO22/23/24, whose only hardware serial
 * function is the QSPI0 controller -- a block this port has not brought up.
 * All three pads work as plain GPIOs, so this is a bit-banged SPI mode-0
 * master instead: transmit-only, like its hardware sibling.
 *
 * Speed comes from skipping the GPIO layer on the inner loop.  Each pin's
 * config register address and its precomputed high/low words are cached at
 * setup, so a bit costs three raw stores (data, clock up, clock down) with
 * no read-modify-write.  That clocks a full 160x160 frame in well under a
 * second -- fine for a face; the QSPI controller is the eventual fast path.
 */

#define PIN_SCK   22
#define PIN_CS    23
#define PIN_MOSI  24

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct swspi_dev_s
{
  struct spi_dev_s dev;
  mutex_t lock;

  volatile uint32_t *sck;
  volatile uint32_t *cs;
  volatile uint32_t *mosi;
  uint32_t sck_hi,  sck_lo;
  uint32_t cs_hi,   cs_lo;
  uint32_t mosi_hi, mosi_lo;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void swspi_cache_pin(int pin, volatile uint32_t **reg,
                            uint32_t *hi, uint32_t *lo)
{
  uint32_t cfg;

  bk7258_gpio_config(pin, true, false, false);
  bk7258_gpio_write(pin, false);

  cfg  = getreg32(BK7258_GPIO_CFG(pin));
  *reg = (volatile uint32_t *)BK7258_GPIO_CFG(pin);
  *hi  = cfg | GPIO_CFG_OUTPUT;
  *lo  = cfg & ~GPIO_CFG_OUTPUT;
}

static int swspi_lock(struct spi_dev_s *dev, bool lock)
{
  struct swspi_dev_s *priv = (struct swspi_dev_s *)dev;

  return lock ? nxmutex_lock(&priv->lock) : nxmutex_unlock(&priv->lock);
}

static void swspi_select(struct spi_dev_s *dev, uint32_t devid,
                         bool selected)
{
}

static uint32_t swspi_setfrequency(struct spi_dev_s *dev, uint32_t frequency)
{
  return frequency;   /* As fast as the stores go. */
}

static void swspi_setmode(struct spi_dev_s *dev, enum spi_mode_e mode)
{
}

static void swspi_setbits(struct spi_dev_s *dev, int nbits)
{
}

static void swspi_exchange(struct spi_dev_s *dev, const void *txbuffer,
                           void *rxbuffer, size_t nwords)
{
  struct swspi_dev_s *priv = (struct swspi_dev_s *)dev;
  const uint8_t *tx = txbuffer;
  size_t n;
  uint8_t byte;
  int bit;

  if (tx == NULL)
    {
      return;
    }

  *priv->cs = priv->cs_lo;

  for (n = 0; n < nwords; n++)
    {
      byte = tx[n];

      for (bit = 7; bit >= 0; bit--)
        {
          *priv->mosi = (byte & (1 << bit)) ? priv->mosi_hi : priv->mosi_lo;
          *priv->sck  = priv->sck_hi;
          *priv->sck  = priv->sck_lo;
        }
    }

  *priv->cs = priv->cs_hi;
}

static uint32_t swspi_send(struct spi_dev_s *dev, uint32_t wd)
{
  uint8_t byte = (uint8_t)wd;

  swspi_exchange(dev, &byte, NULL, 1);
  return 0;
}

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct spi_ops_s g_swspi_ops =
{
  .lock         = swspi_lock,
  .select       = swspi_select,
  .setfrequency = swspi_setfrequency,
  .setmode      = swspi_setmode,
  .setbits      = swspi_setbits,
  .send         = swspi_send,
  .exchange     = swspi_exchange,
};

static struct swspi_dev_s g_swspi =
{
  .dev  = { .ops = &g_swspi_ops },
  .lock = NXMUTEX_INITIALIZER,
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_swspi_initialize
 ****************************************************************************/

struct spi_dev_s *bk7258_swspi_initialize(void)
{
  swspi_cache_pin(PIN_SCK,  &g_swspi.sck,  &g_swspi.sck_hi,  &g_swspi.sck_lo);
  swspi_cache_pin(PIN_CS,   &g_swspi.cs,   &g_swspi.cs_hi,   &g_swspi.cs_lo);
  swspi_cache_pin(PIN_MOSI, &g_swspi.mosi, &g_swspi.mosi_hi, &g_swspi.mosi_lo);

  /* Idle: clock low (mode 0), chip deselected. */

  *g_swspi.sck = g_swspi.sck_lo;
  *g_swspi.cs  = g_swspi.cs_hi;

  return &g_swspi.dev;
}

#endif /* CONFIG_LCD && CONFIG_BK7258_SPI1 */
