/****************************************************************************
 * board/contest_board/chip/bk7258_i2c.c
 *
 * Polled I2C master for the BK7258 "SM bus" block.
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
#include <debug.h>

#include <nuttx/arch.h>
#include <nuttx/i2c/i2c_master.h>
#include <nuttx/mutex.h>

#include "arm_internal.h"
#include "bk7258_memorymap.h"
#include "bk7258_gpio.h"
#include "bk7258_clockconfig.h"

#if defined(CONFIG_I2C) && defined(CONFIG_BK7258_I2C1)

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The register model, from the vendor i2c_struct.h for this SoC.  One
 * peculiarity drives the whole driver: the "status" word at offset 0x14
 * mixes read-only event flags with control bits, and the control bits --
 * ack, stop, start, int_mode -- only take effect when written TOGETHER with
 * the write-1-to-clear of the event flag.  The vendor ISR ends every event
 * with exactly one combined write and its source carries a warning that
 * setting the bits one at a time does not work.  This driver follows the
 * same rule: every bus event is answered by a single composed write.
 */

#define BK7258_I2C0_BASE        0x45850000ul
#define BK7258_I2C1_BASE        0x45860000ul

#define I2C_GLOBAL_OFFSET       0x0008  /* bit0 soft_reset, bit1 clk bypass */
#define I2C_CFG_OFFSET          0x0010
#define I2C_STATUS_OFFSET       0x0014
#define I2C_DATA_OFFSET         0x0018

#define I2C_GLOBAL_SOFT_RESET   (1 << 0)
#define I2C_GLOBAL_CLK_BYPASS   (1 << 1)

#define I2C_CFG_IDLE_CR(n)      ((uint32_t)(n) << 0)   /* Bits 0-2 */
#define I2C_CFG_SCL_CR(n)       ((uint32_t)(n) << 3)   /* Bits 3-5 */
#define I2C_CFG_FREQ_DIV(n)     ((uint32_t)(n) << 6)   /* Bits 6-15 */
#define I2C_CFG_CLK_SRC(n)      ((uint32_t)(n) << 26)  /* Bits 26-27 */
#define I2C_CFG_EN              (1u << 31)

#define I2C_ST_SM_INT           (1 << 0)   /* Event flag, W1C */
#define I2C_ST_SCL_TIMEOUT      (1 << 1)
#define I2C_ST_ARB_LOST         (1 << 3)
#define I2C_ST_INT_MODE(n)      ((uint32_t)(n) << 6)   /* Bits 6-7 */
#define I2C_ST_ACK              (1 << 8)
#define I2C_ST_STOP             (1 << 9)
#define I2C_ST_START            (1 << 10)
#define I2C_ST_TX_MODE          (1 << 13)
#define I2C_ST_BUSY             (1 << 15)

/* Divisor per the vendor formula: div = ceil(ceil(26M / rate) - 6, 3) - 1.
 * clk_src is 3, copied from the vendor HAL's only call site
 * (i2c_hal_configure -> i2c_ll_set_clk_src(hw, id, 0x3)); with the field at
 * 0 the engine never ticked and every wait in this driver timed out, which
 * scanned as an empty bus.
 */

#define I2C_SRC_CLOCK           26000000

/* Poll granularity and budget for one bus event.  A 100 kHz byte takes
 * ~90 us; 20 ms covers clock stretching by slow slaves many times over.
 */

#define I2C_EVENT_TIMEOUT_US    20000
#define I2C_POLL_STEP_US        5

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct bk7258_i2c_dev_s
{
  struct i2c_master_s dev;
  uintptr_t           base;
  mutex_t             lock;
  uint32_t            frequency;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline uint32_t i2c_getreg(struct bk7258_i2c_dev_s *priv,
                                  unsigned int offset)
{
  return getreg32(priv->base + offset);
}

static inline void i2c_putreg(struct bk7258_i2c_dev_s *priv,
                              unsigned int offset, uint32_t value)
{
  putreg32(value, priv->base + offset);
}

/****************************************************************************
 * Name: bk7258_i2c_setfreq
 ****************************************************************************/

static void bk7258_i2c_setfreq(struct bk7258_i2c_dev_s *priv, uint32_t freq)
{
  uint32_t div;

  if (freq == 0)
    {
      freq = 100000;
    }

  div = ((I2C_SRC_CLOCK + freq - 1) / freq - 6 + 2) / 3 - 1;

  i2c_putreg(priv, I2C_CFG_OFFSET,
             I2C_CFG_EN | I2C_CFG_FREQ_DIV(div) |
             I2C_CFG_IDLE_CR(3) | I2C_CFG_SCL_CR(7) | I2C_CFG_CLK_SRC(3));

  priv->frequency = freq;
}

/****************************************************************************
 * Name: bk7258_i2c_wait_event
 *
 * Description:
 *   Poll for the next bus event (SM_INT).  Returns the full status word,
 *   or zero on timeout.
 *
 ****************************************************************************/

static uint32_t bk7258_i2c_wait_event(struct bk7258_i2c_dev_s *priv)
{
  uint32_t status;
  int budget = I2C_EVENT_TIMEOUT_US / I2C_POLL_STEP_US;

  while (budget-- > 0)
    {
      status = i2c_getreg(priv, I2C_STATUS_OFFSET);

      if ((status & I2C_ST_SM_INT) != 0)
        {
          return status;
        }

      up_udelay(I2C_POLL_STEP_US);
    }

  return 0;
}

/****************************************************************************
 * Name: bk7258_i2c_abort
 *
 * Description:
 *   Answer the current event with a STOP and give the bus up.
 *
 ****************************************************************************/

static void bk7258_i2c_abort(struct bk7258_i2c_dev_s *priv)
{
  i2c_putreg(priv, I2C_STATUS_OFFSET, I2C_ST_SM_INT | I2C_ST_STOP);
}

/****************************************************************************
 * Name: bk7258_i2c_transfer
 ****************************************************************************/

static int bk7258_i2c_transfer(struct i2c_master_s *dev,
                               struct i2c_msg_s *msgs, int count)
{
  struct bk7258_i2c_dev_s *priv = (struct bk7258_i2c_dev_s *)dev;
  uint32_t status;
  int ret = OK;
  int i;

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  for (i = 0; i < count && ret == OK; i++)
    {
      struct i2c_msg_s *msg = &msgs[i];
      bool reading = (msg->flags & I2C_M_READ) != 0;
      bool laststop = (i == count - 1) ||
                      (msgs[i + 1].flags & I2C_M_NOSTART) == 0;
      int n;

      if (msg->frequency != priv->frequency && msg->frequency != 0)
        {
          bk7258_i2c_setfreq(priv, msg->frequency);
        }

      if ((msg->flags & I2C_M_NOSTART) == 0)
        {
          /* Address phase.  The address byte goes into the data register
           * FIRST and start is raised afterwards -- the other order
           * transmits a zero address (vendor driver comment, kept because
           * it is exactly the kind of fact the next person would delete).
           */

          i2c_putreg(priv, I2C_DATA_OFFSET,
                     (uint32_t)(msg->addr << 1) | (reading ? 1 : 0));
          i2c_putreg(priv, I2C_STATUS_OFFSET,
                     I2C_ST_SM_INT | I2C_ST_START | I2C_ST_TX_MODE);

          status = bk7258_i2c_wait_event(priv);

          if (status == 0)
            {
              ret = -ETIMEDOUT;
              break;
            }

          if ((status & I2C_ST_ACK) == 0)
            {
              /* Nobody home at this address. */

              bk7258_i2c_abort(priv);
              ret = -ENXIO;
              break;
            }
        }

      if (!reading)
        {
          for (n = 0; n < msg->length; n++)
            {
              /* Answer the previous event: byte out, keep the transfer
               * rolling.
               */

              i2c_putreg(priv, I2C_DATA_OFFSET, msg->buffer[n]);
              i2c_putreg(priv, I2C_STATUS_OFFSET,
                         I2C_ST_SM_INT | I2C_ST_TX_MODE);

              status = bk7258_i2c_wait_event(priv);

              if (status == 0)
                {
                  ret = -ETIMEDOUT;
                  break;
                }

              if ((status & I2C_ST_ACK) == 0)
                {
                  bk7258_i2c_abort(priv);
                  ret = -EIO;
                  break;
                }
            }

          if (ret == OK)
            {
              if (laststop)
                {
                  i2c_putreg(priv, I2C_STATUS_OFFSET,
                             I2C_ST_SM_INT | I2C_ST_STOP);
                }

              /* On a repeated start the pending event is answered by the
               * next message's address phase.
               */
            }
        }
      else
        {
          for (n = 0; n < msg->length; n++)
            {
              bool last = (n == msg->length - 1);

              /* Answer the previous event: switch to receive, and tell the
               * engine whether to ACK the byte it is about to take in.
               * The ACK decision rides with the byte, so the last byte's
               * NACK is set up before that byte is clocked.
               */

              i2c_putreg(priv, I2C_STATUS_OFFSET,
                         I2C_ST_SM_INT | (last ? 0 : I2C_ST_ACK));

              status = bk7258_i2c_wait_event(priv);

              if (status == 0)
                {
                  ret = -ETIMEDOUT;
                  break;
                }

              msg->buffer[n] = i2c_getreg(priv, I2C_DATA_OFFSET) & 0xff;
            }

          if (ret == OK && laststop)
            {
              i2c_putreg(priv, I2C_STATUS_OFFSET,
                         I2C_ST_SM_INT | I2C_ST_STOP);
            }
        }
    }

  nxmutex_unlock(&priv->lock);
  return ret;
}

/****************************************************************************
 * Name: bk7258_i2c_reset
 ****************************************************************************/

#ifdef CONFIG_I2C_RESET
static int bk7258_i2c_reset(struct i2c_master_s *dev)
{
  struct bk7258_i2c_dev_s *priv = (struct bk7258_i2c_dev_s *)dev;

  i2c_putreg(priv, I2C_GLOBAL_OFFSET,
             I2C_GLOBAL_SOFT_RESET | I2C_GLOBAL_CLK_BYPASS);
  bk7258_i2c_setfreq(priv, priv->frequency);
  return OK;
}
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct i2c_ops_s g_i2c_ops =
{
  .transfer = bk7258_i2c_transfer,
#ifdef CONFIG_I2C_RESET
  .reset    = bk7258_i2c_reset,
#endif
};

static struct bk7258_i2c_dev_s g_i2c0_dev =
{
  .dev  = { .ops = &g_i2c_ops },
  .base = BK7258_I2C0_BASE,
  .lock = NXMUTEX_INITIALIZER,
};

static struct bk7258_i2c_dev_s g_i2c1_dev =
{
  .dev  = { .ops = &g_i2c_ops },
  .base = BK7258_I2C1_BASE,
  .lock = NXMUTEX_INITIALIZER,
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_i2cbus_initialize
 ****************************************************************************/

struct i2c_master_s *bk7258_i2cbus_initialize(int port)
{
  struct bk7258_i2c_dev_s *priv;

  /* Clock the block (i2c0_cken bit 0 / i2c1_cken bit 8 of the device
   * clock-enable word) and hand the pins over with the input path on for
   * both: this bus reads its own pins.  I2C0 is the DVP camera's SCCB
   * configuration bus on GPIO20/21 (alternate index 0); I2C1 reaches the
   * expansion header on GPIO0/1 (alternate index 1).
   *
   * The global-control write is ONE combined write.  soft_reset is a
   * LEVEL, 1 = released -- the very same convention that cost this port a
   * day on the UART -- so writing the bypass bit alone would write
   * soft_reset back to 0 and hold the block in reset forever.  The first
   * version of this driver did exactly that, and every address on the bus
   * read as absent.  The bypass itself matters too: without it the ACK
   * flag cannot be read back, per the vendor low-level driver.
   */

  if (port == 0)
    {
      priv = &g_i2c0_dev;
      modifyreg32(BK7258_SYS_CPU_DEVICE_CKEN, 0, 1 << 0);
      i2c_putreg(priv, I2C_GLOBAL_OFFSET,
                 I2C_GLOBAL_SOFT_RESET | I2C_GLOBAL_CLK_BYPASS);
      bk7258_gpio_setaf(20, 0, true);
      bk7258_gpio_setaf(21, 0, true);
    }
  else if (port == 1)
    {
      priv = &g_i2c1_dev;
      modifyreg32(BK7258_SYS_CPU_DEVICE_CKEN, 0, 1 << 8);
      i2c_putreg(priv, I2C_GLOBAL_OFFSET,
                 I2C_GLOBAL_SOFT_RESET | I2C_GLOBAL_CLK_BYPASS);

      /* I2C1 reaches two pad sets: GPIO0/1 (alternate 1) and GPIO42/43
       * (alternate 0).  The camera's SCCB lives on 42/43 -- the vendor
       * firmware bit-bangs exactly these pins and the SDK's DVP driver
       * defaults to hardware I2C1 here.  Nothing ever answered on 0/1.
       */

      bk7258_gpio_setaf(42, 0, true);
      bk7258_gpio_setaf(43, 0, true);
    }
  else
    {
      return NULL;
    }

  bk7258_i2c_setfreq(priv, 100000);

  return &priv->dev;
}

#endif /* CONFIG_I2C && CONFIG_BK7258_I2C1 */
