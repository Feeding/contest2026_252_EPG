/****************************************************************************
 * board/contest_board/chip/bk7258_spi.c
 *
 * Polled SPI master on the BK7258 general-purpose SPI block.
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
#include <errno.h>

#include <nuttx/arch.h>
#include <nuttx/spi/spi.h>
#include <nuttx/mutex.h>

#include "arm_internal.h"
#include "bk7258_memorymap.h"
#include "bk7258_clockconfig.h"
#include "bk7258_gpio.h"

#ifdef CONFIG_BK7258_SPI1

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The register model, from the vendor spi_struct.h.  SPI1 sits at
 * 0x45880000 (SPI0 + 0x1010000); its pins are the screen-1 wires on this
 * board: SCK=GPIO2, CSN=GPIO3, MOSI=GPIO4, all alternate index 0.  The
 * panel is write-only, so GPIO5 (nominally MISO) stays free for the
 * panel's D/C line.
 */

#define BK7258_SPI1_BASE        0x45880000ul

#define SPI_GLOBAL_OFFSET       0x0008
#define SPI_CTRL_OFFSET         0x0010
#define SPI_CFG_OFFSET          0x0014
#define SPI_INT_OFFSET          0x0018
#define SPI_DATA_OFFSET         0x001c

#define SPI_GLOBAL_SOFT_RESET   (1 << 0)   /* Level: 1 = released.  Same
                                            * convention as the UART and the
                                            * I2C blocks; both bit this port
                                            * once each. */
#define SPI_GLOBAL_CLK_BYPASS   (1 << 1)

#define SPI_CTRL_CLK_RATE(n)    ((uint32_t)((n) & 0xff) << 8)
#define SPI_CTRL_WIRE3_EN       (1 << 17)
#define SPI_CTRL_BIT_WIDTH16    (1 << 18)
#define SPI_CTRL_LSB_FIRST      (1 << 19)
#define SPI_CTRL_CPOL           (1 << 20)
#define SPI_CTRL_CPHA           (1 << 21)
#define SPI_CTRL_MASTER_EN      (1 << 22)
#define SPI_CTRL_ENABLE         (1 << 23)
#define SPI_CTRL_BYTE_INTERVAL(n) ((uint32_t)((n) & 0x3f) << 24)

#define SPI_CFG_TX_EN           (1 << 0)
#define SPI_CFG_RX_EN           (1 << 1)
#define SPI_CFG_TX_LEN(n)       ((uint32_t)((n) & 0xfff) << 8)
#define SPI_CFG_RX_LEN(n)       ((uint32_t)((n) & 0xfff) << 20)

#define SPI_INT_TX_WR_READY     (1 << 1)   /* RO: FIFO will take a byte */
#define SPI_INT_RX_RD_READY     (1 << 2)   /* RO: FIFO holds a byte */
#define SPI_INT_TX_FINISH       (1 << 13)  /* W1C: tx_trans_len consumed */
#define SPI_INT_RX_FINISH       (1 << 14)  /* W1C */
#define SPI_INT_TX_FIFO_CLR     (1 << 16)  /* Strobe */
#define SPI_INT_RX_FIFO_CLR     (1 << 17)  /* Strobe */

/* All W1C event bits, for targeted clearing.  The FIFO-clear strobes live
 * in the same word, so a read-modify-writeback of the whole register would
 * also flush the FIFOs -- events are cleared by writing exactly the event
 * mask instead.
 */

#define SPI_INT_EVENTS_MASK     0x7f00

/* One transfer moves at most 4095 units (12-bit length field). */

#define SPI_MAX_CHUNK           4095

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct bk7258_spi_dev_s
{
  struct spi_dev_s dev;
  uintptr_t        base;
  mutex_t          lock;
  uint32_t         frequency;
  uint8_t          mode;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline uint32_t spi_getreg(struct bk7258_spi_dev_s *priv,
                                  unsigned int offset)
{
  return getreg32(priv->base + offset);
}

static inline void spi_putreg(struct bk7258_spi_dev_s *priv,
                              unsigned int offset, uint32_t value)
{
  putreg32(value, priv->base + offset);
}

/****************************************************************************
 * Name: bk7258_spi_reconfig
 *
 * Description:
 *   Program the control register from the cached frequency and mode.  The
 *   divider comes from the 26 MHz crystal: SCK = 26M / (2 * div), so 13 MHz
 *   at div 1 is the fastest XTAL-sourced clock (a div of zero is undefined
 *   per the vendor driver).
 *
 ****************************************************************************/

static void bk7258_spi_reconfig(struct bk7258_spi_dev_s *priv)
{
  uint32_t div;
  uint32_t ctrl;

  div = 26000000 / (2 * priv->frequency);

  if (div < 1)
    {
      div = 1;
    }

  if (div > 255)
    {
      div = 255;
    }

  ctrl = SPI_CTRL_CLK_RATE(div) | SPI_CTRL_MASTER_EN | SPI_CTRL_ENABLE |
         SPI_CTRL_BYTE_INTERVAL(1);

  if ((priv->mode & 0x2) != 0)
    {
      ctrl |= SPI_CTRL_CPOL;
    }

  if ((priv->mode & 0x1) != 0)
    {
      ctrl |= SPI_CTRL_CPHA;
    }

  spi_putreg(priv, SPI_CTRL_OFFSET, ctrl);
}

/****************************************************************************
 * Name: bk7258_spi_sendchunk
 *
 * Description:
 *   Clock out up to SPI_MAX_CHUNK bytes, polled.  Hardware CSN frames the
 *   chunk: it drops when tx_en starts the engine and rises when the length
 *   counter is consumed, which the GC9D01 is happy with -- the vendor's own
 *   bit-banged path toggles CS around every single byte.
 *
 ****************************************************************************/

static void bk7258_spi_dump_once(struct bk7258_spi_dev_s *priv)
{
  static bool dumped;
  static const char hex[] = "0123456789abcdef";
  uint32_t regs[3];
  int r;
  int i;

  if (dumped)
    {
      return;
    }

  dumped  = true;
  regs[0] = spi_getreg(priv, SPI_CTRL_OFFSET);
  regs[1] = spi_getreg(priv, SPI_CFG_OFFSET);
  regs[2] = spi_getreg(priv, SPI_INT_OFFSET);

  up_putc('T');

  for (r = 0; r < 3; r++)
    {
      up_putc(' ');

      for (i = 28; i >= 0; i -= 4)
        {
          up_putc(hex[(regs[r] >> i) & 0xf]);
        }
    }

  up_putc('\n');
}

static int bk7258_spi_sendchunk(struct bk7258_spi_dev_s *priv,
                                const uint8_t *data, size_t nbytes)
{
  size_t i;
  uint32_t budget;

  spi_putreg(priv, SPI_INT_OFFSET, SPI_INT_TX_FIFO_CLR);
  spi_putreg(priv, SPI_CFG_OFFSET, SPI_CFG_TX_LEN(nbytes) | SPI_CFG_TX_EN);

  for (i = 0; i < nbytes; i++)
    {
      budget = 20000;

      while ((spi_getreg(priv, SPI_INT_OFFSET) & SPI_INT_TX_WR_READY) == 0)
        {
          if (--budget == 0)
            {
              bk7258_spi_dump_once(priv);
              spi_putreg(priv, SPI_CFG_OFFSET, 0);
              return -ETIMEDOUT;
            }
        }

      spi_putreg(priv, SPI_DATA_OFFSET, data[i]);
    }

  budget = 20000;

  while ((spi_getreg(priv, SPI_INT_OFFSET) & SPI_INT_TX_FINISH) == 0)
    {
      if (--budget == 0)
        {
          spi_putreg(priv, SPI_CFG_OFFSET, 0);
          return -ETIMEDOUT;
        }
    }

  spi_putreg(priv, SPI_INT_OFFSET, SPI_INT_TX_FINISH);
  spi_putreg(priv, SPI_CFG_OFFSET, 0);

  return OK;
}

/****************************************************************************
 * SPI operations
 ****************************************************************************/

static int bk7258_spi_lock(struct spi_dev_s *dev, bool lock)
{
  struct bk7258_spi_dev_s *priv = (struct bk7258_spi_dev_s *)dev;

  return lock ? nxmutex_lock(&priv->lock) : nxmutex_unlock(&priv->lock);
}

static void bk7258_spi_select(struct spi_dev_s *dev, uint32_t devid,
                              bool selected)
{
  /* Hardware CSN frames each transfer on its own. */
}

static uint32_t bk7258_spi_setfrequency(struct spi_dev_s *dev,
                                        uint32_t frequency)
{
  struct bk7258_spi_dev_s *priv = (struct bk7258_spi_dev_s *)dev;

  if (frequency != 0 && frequency != priv->frequency)
    {
      priv->frequency = frequency;
      bk7258_spi_reconfig(priv);
    }

  return priv->frequency;
}

static void bk7258_spi_setmode(struct spi_dev_s *dev, enum spi_mode_e mode)
{
  struct bk7258_spi_dev_s *priv = (struct bk7258_spi_dev_s *)dev;

  if ((uint8_t)mode != priv->mode)
    {
      priv->mode = (uint8_t)mode;
      bk7258_spi_reconfig(priv);
    }
}

static void bk7258_spi_setbits(struct spi_dev_s *dev, int nbits)
{
  /* 8-bit only in this bring-up driver. */
}

static uint32_t bk7258_spi_send(struct spi_dev_s *dev, uint32_t wd)
{
  struct bk7258_spi_dev_s *priv = (struct bk7258_spi_dev_s *)dev;
  uint8_t byte = (uint8_t)wd;

  bk7258_spi_sendchunk(priv, &byte, 1);
  return 0;
}

static void bk7258_spi_exchange(struct spi_dev_s *dev, const void *txbuffer,
                                void *rxbuffer, size_t nwords)
{
  struct bk7258_spi_dev_s *priv = (struct bk7258_spi_dev_s *)dev;
  const uint8_t *tx = txbuffer;
  size_t chunk;

  /* Transmit-only: the one consumer is a write-only panel.  A read path
   * can be added when a bidirectional device shows up.
   */

  if (tx == NULL)
    {
      return;
    }

  while (nwords > 0)
    {
      chunk = (nwords > SPI_MAX_CHUNK) ? SPI_MAX_CHUNK : nwords;

      if (bk7258_spi_sendchunk(priv, tx, chunk) != OK)
        {
          return;
        }

      tx     += chunk;
      nwords -= chunk;
    }
}

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct spi_ops_s g_spi_ops =
{
  .lock         = bk7258_spi_lock,
  .select       = bk7258_spi_select,
  .setfrequency = bk7258_spi_setfrequency,
  .setmode      = bk7258_spi_setmode,
  .setbits      = bk7258_spi_setbits,
  .send         = bk7258_spi_send,
  .exchange     = bk7258_spi_exchange,
};

static struct bk7258_spi_dev_s g_spi1_dev =
{
  .dev       = { .ops = &g_spi_ops },
  .base      = BK7258_SPI1_BASE,
  .lock      = NXMUTEX_INITIALIZER,
  .frequency = 13000000,
  .mode      = 0,
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_spibus_initialize
 ****************************************************************************/

struct spi_dev_s *bk7258_spibus_initialize(int port)
{
  struct bk7258_spi_dev_s *priv;

  if (port != 1)
    {
      return NULL;
    }

  priv = &g_spi1_dev;

  /* Clock the block: spi1_cken is bit 9 of the device clock-enable word,
   * and the SPI1 clock source select is bit 5 of the 26M/WDT divider word
   * (0 = 26 MHz crystal).  Then one combined global-control write --
   * soft_reset is the family's usual level, 1 = released.
   */

  modifyreg32(BK7258_SYS_CPU_DEVICE_CKEN, 0, 1 << 9);
  modifyreg32(BK7258_SYS_BASE + (0x0a << 2), 1 << 5, 0);

  spi_putreg(priv, SPI_GLOBAL_OFFSET,
             SPI_GLOBAL_SOFT_RESET | SPI_GLOBAL_CLK_BYPASS);

  bk7258_gpio_setaf(2, 0, false);   /* SCK  */
  bk7258_gpio_setaf(3, 0, false);   /* CSN  */
  bk7258_gpio_setaf(4, 0, false);   /* MOSI */

  bk7258_spi_reconfig(priv);

  return &priv->dev;
}

#endif /* CONFIG_BK7258_SPI1 */
