/****************************************************************************
 * board/contest_board/chip/bk7258_qspi.c
 *
 * QSPI0 as a plain transmit-only SPI master, for screen 2.
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

/* Screen 2's wires belong to the QSPI0 controller (CLK=GPIO22, CSN=GPIO23,
 * IO0=GPIO24, alternate index 3).  Its IO1/IO2/IO3 pads are NOT mapped --
 * on this board they serve as the backlight (GPIO25), a spare input
 * (GPIO26) and the camera clock (GPIO27) -- so the controller runs
 * strictly 1-wire.
 *
 * The controller has no notion of "just clock these bytes out", but its
 * indirect command engine can fake it: a command block sends up to eight
 * opaque "command" bytes followed by a data phase from the FIFO, all on
 * one wire with CS framed around the operation.  Loading the first buffer
 * byte as the command and the rest as data puts a byte-exact copy of the
 * buffer on the wire -- which is all a 4-line panel needs, since its D/C
 * discrimination is a separate GPIO handled by the panel driver.
 */

#define BK7258_QSPI0_BASE       0x46040000ul
#define BK7258_QSPI1_BASE       0x46060000ul

#define QSPI_GLB_CTRL_OFFSET    (0x02 << 2)
#define QSPI_CMD_C_L_OFFSET     (0x10 << 2)
#define QSPI_CMD_C_H_OFFSET     (0x11 << 2)
#define QSPI_CMD_C_CFG1_OFFSET  (0x12 << 2)
#define QSPI_CMD_C_CFG2_OFFSET  (0x13 << 2)
#define QSPI_CONFIG_OFFSET      (0x18 << 2)
#define QSPI_RST_FIFO_OFFSET    (0x19 << 2)
#define QSPI_STATUS_CLR_OFFSET  (0x1b << 2)
#define QSPI_STATUS_OFFSET      (0x1c << 2)
#define QSPI_FIFO_OFFSET        (0x40 << 2)

#define QSPI_GLB_SOFT_RESET     (1 << 0)   /* Level, 1 = released (family) */
#define QSPI_GLB_CLK_BYPASS     (1 << 1)

#define QSPI_CONFIG_EN          (1 << 0)
#define QSPI_CONFIG_CLK_RATE(n) ((uint32_t)((n) & 0xff) << 8)

#define QSPI_CFG1_ONE_CMD_BYTE  0x0000000c /* cmd1 on 1 wire, then stop */

#define QSPI_CFG2_START         (1 << 0)
#define QSPI_CFG2_DATA_LEN(n)   ((uint32_t)((n) & 0x3ff) << 2)
#define QSPI_CFG2_DUMMY_MODE_WR ((uint32_t)4 << 24)

#define QSPI_STATUS_CMD_DONE    (1 << 2)

#define QSPI_RST_SW_FIFO        (1 << 1)

/* One indirect operation: 1 command byte + up to 240 data bytes (60 FIFO
 * words -- the FIFO register file is 61 words deep and the vendor's page
 * program pushes 64, so 60 is comfortably inside).
 */

#define QSPI_CHUNK_DATA         240

/* Memory-mapped frame-blast plumbing (vendor MAPPING_MODE): the config
 * register's three magic bits hand the serializer to the DAHB port, and
 * a plain mem-to-mem GDMA write into the unit's data window streams out
 * on 1 wire with AHB backpressure pacing -- zero CPU byte pushing.
 */

#define QSPI_CMD_A_CFG2_OFFSET  (0x0b << 2)
#define QSPI_CFG_FORCE_CS_LOW   (1u << 6)
#define QSPI_CFG_DIS_CMD_SCK    (1u << 16)
#define QSPI_CFG_IO_MEM_SEL     (1u << 22)
#define QSPI_STATUS_TX_BUSY     (1u << 14)
#define QSPI_STATUS_FIFO_EMPTY  (1u << 16)

#define QSPI_WINDOW(port)       ((port) ? 0x68000000ul : 0x64000000ul)

#define GDMA_BASE               0x55020000ul
#define GDMA_CH(n)              (GDMA_BASE + 0x40 + (n) * 0x40)
#define GDMA_CH_CTRL(n)         (GDMA_CH(n) + 0x00)
#define GDMA_CH_DST(n)          (GDMA_CH(n) + 0x04)
#define GDMA_CH_SRC(n)          (GDMA_CH(n) + 0x08)
#define GDMA_CH_MUX(n)          (GDMA_CH(n) + 0x1c)

#define QSPI_BLAST_CH(port)     ((port) ? 2 : 1)

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct bk7258_qspi_dev_s
{
  struct spi_dev_s dev;
  uintptr_t        base;
  mutex_t          lock;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline uint32_t qspi_getreg(struct bk7258_qspi_dev_s *priv,
                                   unsigned int offset)
{
  return getreg32(priv->base + offset);
}

static inline void qspi_putreg(struct bk7258_qspi_dev_s *priv,
                               unsigned int offset, uint32_t value)
{
  putreg32(value, priv->base + offset);
}

/****************************************************************************
 * Name: bk7258_qspi_op
 *
 * Description:
 *   One indirect operation: first byte as the command, the rest through
 *   the FIFO as 1-wire data.  Returns when the engine reports done.
 *
 ****************************************************************************/

static int bk7258_qspi_op(struct bk7258_qspi_dev_s *priv,
                          const uint8_t *buf, size_t nbytes)
{
  uint32_t word;
  uint32_t budget;
  size_t ndata = nbytes - 1;
  size_t i;

  /* Clear stale completion status. */

  qspi_putreg(priv, QSPI_STATUS_CLR_OFFSET, 0xff);
  qspi_putreg(priv, QSPI_STATUS_CLR_OFFSET, 0);

  /* Data first: words 0x40-0x7c are a 61-word buffer RAM, one word per
   * address -- NOT a single-port FIFO.  Writing every word to 0x40 only
   * ever fills slot 0, and the engine then streams whatever the rest of
   * the buffer happens to hold; that bug cost both panels a whole flash
   * cycle of darkness.
   */

  for (i = 0; i < ndata; i += 4)
    {
      word = buf[1 + i];

      if (i + 1 < ndata)
        {
          word |= (uint32_t)buf[2 + i] << 8;
        }

      if (i + 2 < ndata)
        {
          word |= (uint32_t)buf[3 + i] << 16;
        }

      if (i + 3 < ndata)
        {
          word |= (uint32_t)buf[4 + i] << 24;
        }

      qspi_putreg(priv, QSPI_FIFO_OFFSET + i, word);
    }

  /* Command block: one 1-wire command byte, then a plain write data phase.
   * The vendor LCD driver leaves dummy_mode at 0 for writes; extra dummy
   * cycles between command and data would shear every data byte.
   */

  qspi_putreg(priv, QSPI_CMD_C_L_OFFSET, 0);
  qspi_putreg(priv, QSPI_CMD_C_H_OFFSET, buf[0]);
  qspi_putreg(priv, QSPI_CMD_C_CFG1_OFFSET, QSPI_CFG1_ONE_CMD_BYTE);
  qspi_putreg(priv, QSPI_CMD_C_CFG2_OFFSET,
              QSPI_CFG2_DATA_LEN(ndata) |
              QSPI_CFG2_START);

  budget = 200000;

  while ((qspi_getreg(priv, QSPI_STATUS_OFFSET) & QSPI_STATUS_CMD_DONE) == 0)
    {
      if (--budget == 0)
        {
          return -ETIMEDOUT;
        }
    }

  qspi_putreg(priv, QSPI_STATUS_CLR_OFFSET, QSPI_STATUS_CMD_DONE);
  qspi_putreg(priv, QSPI_STATUS_CLR_OFFSET, 0);

  return OK;
}

/****************************************************************************
 * SPI operations
 ****************************************************************************/

static int qspi_lock(struct spi_dev_s *dev, bool lock)
{
  struct bk7258_qspi_dev_s *priv = (struct bk7258_qspi_dev_s *)dev;

  return lock ? nxmutex_lock(&priv->lock) : nxmutex_unlock(&priv->lock);
}

static void qspi_select(struct spi_dev_s *dev, uint32_t devid, bool sel)
{
}

static uint32_t qspi_setfrequency(struct spi_dev_s *dev, uint32_t frequency)
{
  return frequency;
}

static void qspi_setmode(struct spi_dev_s *dev, enum spi_mode_e mode)
{
}

static void qspi_setbits(struct spi_dev_s *dev, int nbits)
{
}

static void qspi_exchange(struct spi_dev_s *dev, const void *txbuffer,
                          void *rxbuffer, size_t nwords)
{
  struct bk7258_qspi_dev_s *priv = (struct bk7258_qspi_dev_s *)dev;
  const uint8_t *tx = txbuffer;
  size_t chunk;

  if (tx == NULL)
    {
      return;
    }

  while (nwords > 0)
    {
      chunk = (nwords > QSPI_CHUNK_DATA + 1) ? QSPI_CHUNK_DATA + 1 : nwords;

      if (bk7258_qspi_op(priv, tx, chunk) != OK)
        {
          return;
        }

      tx     += chunk;
      nwords -= chunk;
    }
}

static uint32_t qspi_send(struct spi_dev_s *dev, uint32_t wd)
{
  uint8_t byte = (uint8_t)wd;

  qspi_exchange(dev, &byte, NULL, 1);
  return 0;
}

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct spi_ops_s g_qspi_ops =
{
  .lock         = qspi_lock,
  .select       = qspi_select,
  .setfrequency = qspi_setfrequency,
  .setmode      = qspi_setmode,
  .setbits      = qspi_setbits,
  .send         = qspi_send,
  .exchange     = qspi_exchange,
};

static struct bk7258_qspi_dev_s g_qspi_devs[2] =
{
  {
    .dev  = { .ops = &g_qspi_ops },
    .base = BK7258_QSPI0_BASE,
    .lock = NXMUTEX_INITIALIZER,
  },
  {
    .dev  = { .ops = &g_qspi_ops },
    .base = BK7258_QSPI1_BASE,
    .lock = NXMUTEX_INITIALIZER,
  },
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_qspibus_initialize
 *
 * Description:
 *   Bring QSPI0 up in 1-wire mode.  Returns NULL if the block does not
 *   answer, so the caller can fall back to the software SPI and the eye
 *   stays lit either way.
 *
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_qspi_blast_start / bk7258_qspi_blast_wait
 *
 * Description:
 *   Stream len bytes from buf straight out of the QSPI unit's 1-wire
 *   data pin via memory-mapped mode and a mem-to-mem GDMA channel.
 *   The panel must already be in RAMWR with DC high; CS is forced low
 *   for the duration.  start returns immediately; wait blocks until
 *   the wire is idle and restores command mode.
 *
 ****************************************************************************/

void bk7258_qspi_map_enter(int port)
{
  uintptr_t base = port ? BK7258_QSPI1_BASE : BK7258_QSPI0_BASE;

  /* The vendor zeroes the whole cmd_c block before switching the
   * serializer to the memory path -- residue from the RAMWR just sent
   * (start/length bits) otherwise keeps the command engine attached.
   */

  putreg32(0, base + QSPI_CMD_C_L_OFFSET);
  putreg32(0, base + QSPI_CMD_C_H_OFFSET);
  putreg32(0, base + QSPI_CMD_C_CFG1_OFFSET);
  putreg32(0, base + QSPI_CMD_C_CFG2_OFFSET);

  putreg32(0x80000000, base + QSPI_CMD_A_CFG2_OFFSET);
  modifyreg32(base + QSPI_CONFIG_OFFSET, 0, QSPI_CFG_FORCE_CS_LOW);
  modifyreg32(base + QSPI_CONFIG_OFFSET, 0, QSPI_CFG_IO_MEM_SEL);
  modifyreg32(base + QSPI_CONFIG_OFFSET, 0, QSPI_CFG_DIS_CMD_SCK);
}

void bk7258_qspi_map_exit(int port)
{
  uintptr_t base = port ? BK7258_QSPI1_BASE : BK7258_QSPI0_BASE;
  uint32_t budget = 200000;

  while (budget-- > 0)
    {
      uint32_t sts = getreg32(base + QSPI_STATUS_OFFSET);

      if ((sts & QSPI_STATUS_FIFO_EMPTY) != 0 &&
          (sts & QSPI_STATUS_TX_BUSY) == 0)
        {
          break;
        }
    }

  up_udelay(30);
  modifyreg32(base + QSPI_CONFIG_OFFSET, QSPI_CFG_DIS_CMD_SCK, 0);
  modifyreg32(base + QSPI_CONFIG_OFFSET, QSPI_CFG_FORCE_CS_LOW, 0);
  modifyreg32(base + QSPI_CONFIG_OFFSET, QSPI_CFG_IO_MEM_SEL, 0);
}

void bk7258_qspi_blast_start(int port, const void *buf, size_t len)
{
  uintptr_t base = port ? BK7258_QSPI1_BASE : BK7258_QSPI0_BASE;
  int ch = QSPI_BLAST_CH(port);

  /* Data phase: memory-mapped mode, 1 wire. */

  extern void bk7258_qspi_map_enter(int port);
  bk7258_qspi_map_enter(port);

  putreg32(0, GDMA_CH_CTRL(ch));
  putreg32((uint32_t)(uintptr_t)buf, GDMA_CH_SRC(ch));
  putreg32(QSPI_WINDOW(port), GDMA_CH_DST(ch));
  /* m2m, SEC attrs; src bursts INC16 from PSRAM but the DESTINATION
   * must stay single-beat: the mapping window paces the DMA by AHB
   * backpressure, and burst writes overran it into the void (screens
   * dark while the CPU's single stores painted fine).
   */

  putreg32(0x03300000, GDMA_CH_MUX(ch));
  putreg32(((uint32_t)(len - 1) << 16) | (1u << 9) | (1u << 8) |
           (2u << 6) | (2u << 4) | 1u, GDMA_CH_CTRL(ch));
}

int bk7258_qspi_blast_wait(int port)
{
  uintptr_t base = port ? BK7258_QSPI1_BASE : BK7258_QSPI0_BASE;
  int ch = QSPI_BLAST_CH(port);
  uint32_t budget = 4000000;
  int ret = OK;

  while ((getreg32(GDMA_CH_CTRL(ch)) & 1u) != 0)
    {
      if (--budget == 0)
        {
          putreg32(0, GDMA_CH_CTRL(ch));
          ret = -ETIMEDOUT;
          break;
        }
    }

  /* Let the serializer drain, then hand the pins back. */

  budget = 200000;
  while (budget-- > 0)
    {
      uint32_t sts = getreg32(base + QSPI_STATUS_OFFSET);

      if ((sts & QSPI_STATUS_FIFO_EMPTY) != 0 &&
          (sts & QSPI_STATUS_TX_BUSY) == 0)
        {
          break;
        }
    }

  up_udelay(30);

  modifyreg32(base + QSPI_CONFIG_OFFSET, QSPI_CFG_DIS_CMD_SCK, 0);
  modifyreg32(base + QSPI_CONFIG_OFFSET, QSPI_CFG_FORCE_CS_LOW, 0);
  modifyreg32(base + QSPI_CONFIG_OFFSET, QSPI_CFG_IO_MEM_SEL, 0);
  return ret;
}

struct spi_dev_s *bk7258_qspibus_initialize(int port)
{
  struct bk7258_qspi_dev_s *priv;
  uint32_t id;

  /* Per-unit plumbing.  QSPI0: cken bit 20, clock fields in clkdiv mode2
   * (word 0x09); pins GPIO22/23/24, alternate 3.  QSPI1: cken bit 21,
   * clock fields in the 26M/WDT divider word (0x0a); pins GPIO2/3/4,
   * alternate 6 -- the same pads the GSPI SPI1 block reaches at alternate
   * 0, which stays available as a fallback.  Both clock recipes: 320 MHz
   * source (select bit 10 = 0), source divider 15, controller rate 2.
   */

  if (port == 0)
    {
      priv = &g_qspi_devs[0];
      modifyreg32(BK7258_SYS_CPU_DEVICE_CKEN, 0, 1 << 20);
      modifyreg32(BK7258_SYS_BASE + (0x09 << 2),
                  (0xf << 6) | (1 << 10), (7 << 6));
    }
  else if (port == 1)
    {
      priv = &g_qspi_devs[1];
      modifyreg32(BK7258_SYS_CPU_DEVICE_CKEN, 0, 1 << 21);
      modifyreg32(BK7258_SYS_BASE + (0x0a << 2),
                  (0xf << 6) | (1 << 10), (7 << 6));
    }
  else
    {
      return NULL;
    }

  /* Family convention: soft_reset is a level, 1 = released. */

  qspi_putreg(priv, QSPI_GLB_CTRL_OFFSET,
              QSPI_GLB_SOFT_RESET | QSPI_GLB_CLK_BYPASS);

  id = qspi_getreg(priv, 0);

  if (id == 0 || id == 0xffffffff)
    {
      return NULL;
    }

  qspi_putreg(priv, QSPI_CONFIG_OFFSET,
              QSPI_CONFIG_EN | QSPI_CONFIG_CLK_RATE(0));

  /* Only CLK/CSN/IO0 go to the controller; the IO1-IO3 pads of both units
   * have board-assigned day jobs.
   */

  if (port == 0)
    {
      bk7258_gpio_setaf(22, 3, false);
      bk7258_gpio_setaf(23, 3, false);
      bk7258_gpio_setaf(24, 3, false);
    }
  else
    {
      bk7258_gpio_setaf(2, 6, false);
      bk7258_gpio_setaf(3, 6, false);
      bk7258_gpio_setaf(4, 6, false);
    }

  return &priv->dev;
}

#endif /* CONFIG_BK7258_SPI1 */
