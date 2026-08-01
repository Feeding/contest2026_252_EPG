/****************************************************************************
 * boards/arm/bk7258/contest_board/chip/bk7258_sdio.c
 *
 * Polled SDIO host lower-half for the on-board SD NAND.  First version:
 * PIO only, single-block transfers (pair with MMCSD_MULTIBLOCK_LIMIT=1),
 * no interrupts -- everything completes synchronously inside eventwait().
 *
 * Register recipe transcribed from bk_idk sdio_host_driver.c +
 * sd_card_driver.c with three lessons pre-applied:
 *  - the FIFOs at +0x3c/+0x40 are honest single-port registers (unlike
 *    the QSPI buffer RAM);
 *  - RX byte order is fixed in hardware via sd_byte_sel; TX is swapped
 *    by the ASIC on BK7258 -- software must NOT swap either direction;
 *  - the SDK's stale write to 0x448b0000 (a BK7256 address) is a trap,
 *    not a requirement.
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/arch.h>
#include <nuttx/sdio.h>
#include <nuttx/mutex.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <syslog.h>

#include "arm_internal.h"
#include "bk7258_memorymap.h"
#include "bk7258_clockconfig.h"
#include "bk7258_gpio.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SDIO_BASE             0x458d0000ul

#define SDIO_REG_CMD          (SDIO_BASE + 0x10)  /* index/flags/start */
#define SDIO_REG_ARG          (SDIO_BASE + 0x14)
#define SDIO_REG_RSPTO        (SDIO_BASE + 0x18)  /* response timeout */
#define SDIO_REG_DATA         (SDIO_BASE + 0x1c)  /* data path control */
#define SDIO_REG_DATATO       (SDIO_BASE + 0x20)  /* data timeout */
#define SDIO_REG_RSP0         (SDIO_BASE + 0x24)
#define SDIO_REG_RSP1         (SDIO_BASE + 0x28)
#define SDIO_REG_RSP2         (SDIO_BASE + 0x2c)
#define SDIO_REG_RSP3         (SDIO_BASE + 0x30)
#define SDIO_REG_STATUS       (SDIO_BASE + 0x34)  /* W1C */
#define SDIO_REG_INTMASK      (SDIO_BASE + 0x38)
#define SDIO_REG_TXFIFO       (SDIO_BASE + 0x3c)
#define SDIO_REG_RXFIFO       (SDIO_BASE + 0x40)
#define SDIO_REG_FIFOCTRL     (SDIO_BASE + 0x44)
#define SDIO_REG_SENDCNT      (SDIO_BASE + 0x48)

/* SDIO_REG_CMD */

#define CMD_START             (1u << 0)           /* self-clearing */
#define CMD_RSP               (1u << 1)
#define CMD_LONG              (1u << 2)
#define CMD_CRC_CHECK         (1u << 3)
#define CMD_INDEX(n)          ((uint32_t)(n) << 4)

/* SDIO_REG_DATA */

#define DATA_EN               (1u << 0)           /* start RX engine */
#define DATA_BUS_4BIT         (1u << 2)
#define DATA_MUL_BLK          (1u << 3)
#define DATA_BLKSIZE(n)       ((uint32_t)(n) << 4)
#define DATA_START_WR         (1u << 16)
#define DATA_BYTE_SEL         (1u << 17)

/* SDIO_REG_STATUS (W1C) */

#define ST_NORSP_END          (1u << 0)
#define ST_RSP_END            (1u << 1)
#define ST_RSP_TIMEOUT        (1u << 2)
#define ST_RECV_END           (1u << 3)
#define ST_WR_END             (1u << 4)
#define ST_RSP_CRC_OK         (1u << 10)
#define ST_RSP_CRC_FAIL       (1u << 11)
#define ST_DATA_CRC_OK        (1u << 12)
#define ST_DATA_CRC_FAIL      (1u << 13)
#define ST_WR_STATUS_SHIFT    20
#define ST_WR_STATUS_MASK     (7u << ST_WR_STATUS_SHIFT)
#define ST_ALL                0xffffffffu

/* SDIO_REG_FIFOCTRL */

#define FIFO_RX_THRESH(n)     ((uint32_t)(n) << 0)
#define FIFO_TX_THRESH(n)     ((uint32_t)(n) << 8)
#define FIFO_RX_RESET         (1u << 16)
#define FIFO_TX_RESET         (1u << 17)
#define FIFO_RX_RD_READY      (1u << 18)
#define FIFO_TX_WR_READY      (1u << 19)
#define FIFO_STATE_RESET      (1u << 20)
#define FIFO_CLK_REC_SEL      (1u << 25)
#define FIFO_CLK_GATE_ON      (1u << 27)
#define FIFO_HOST_WR_BLK_EN   (1u << 28)

#define FIFO_BASE_CONFIG      (FIFO_RX_THRESH(1) | FIFO_TX_THRESH(1) | \
                               FIFO_CLK_REC_SEL | FIFO_CLK_GATE_ON | \
                               FIFO_HOST_WR_BLK_EN)

/* System plumbing: cken bit22; clock recipe in reg0x09 bits[17:14]
 * (whole enum value into the field: 7 = 26M/256 for enumeration,
 * 0 = 26M/2 = 13 MHz for data).
 */

#define SDIO_CKEN             (1u << 22)
#define SYS_CLKDIV_MODE2      (BK7258_SYS_BASE + (0x09 << 2))
#define SDIO_CLK_FIELD_MASK   (0xfu << 14)
#define SDIO_CLK_ENUM         (7u << 14)         /* ~100 kHz */
#define SDIO_CLK_DATA         (0u << 14)         /* 13 MHz */

#define SDIO_PIN_POWER        13                 /* SD NAND LDO, high */
#define SDIO_PIN_CLK          14                 /* func 0 on all six */
#define SDIO_PIN_CMD          15

#define RSPTO_ENUM            2500
#define RSPTO_DATA            13000
#define DATATO_DATA           416000

#define POLL_BUDGET           2000000

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct bk7258_sdio_s
{
  struct sdio_dev_s dev;          /* Must be first */
  sdio_eventset_t waitevents;
  int rspresult;                  /* Outcome of the last waitresponse */
  bool widebus;
  bool enumclk;
  uint32_t blocklen;
  uint32_t nblocks;

  /* Armed transfer, executed inside eventwait() */

  uint8_t *rxbuffer;
  const uint8_t *txbuffer;
  size_t xfrbytes;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void sdio_hw_reset_engine(void)
{
  putreg32(FIFO_BASE_CONFIG | FIFO_RX_RESET | FIFO_TX_RESET |
           FIFO_STATE_RESET, SDIO_REG_FIFOCTRL);
  putreg32(FIFO_BASE_CONFIG, SDIO_REG_FIFOCTRL);
  putreg32(ST_ALL, SDIO_REG_STATUS);
}

/* sdio_dev_s methods */

static void bk_reset(struct sdio_dev_s *dev)
{
  struct bk7258_sdio_s *priv = (struct bk7258_sdio_s *)dev;

  /* SD NAND LDO on, then give the die time to wake. */

  bk7258_gpio_config(SDIO_PIN_POWER, true, false, false);
  bk7258_gpio_write(SDIO_PIN_POWER, true);
  up_mdelay(10);

  /* Clock domain first -- the register block stalls the bus otherwise. */

  modifyreg32(BK7258_SYS_CPU_DEVICE_CKEN, 0, SDIO_CKEN);
  modifyreg32(SYS_CLKDIV_MODE2, SDIO_CLK_FIELD_MASK, SDIO_CLK_ENUM);
  priv->enumclk = true;

  /* Six pins, all function index 0.  CLK is push-only; CMD and D0-D3
   * need the input path for the bidirectional phases.
   */

  bk7258_gpio_setaf(SDIO_PIN_CLK, 0, false);
  bk7258_gpio_setaf(SDIO_PIN_CMD, 0, true);
  bk7258_gpio_setaf(16, 0, true);
  bk7258_gpio_setaf(17, 0, true);
  bk7258_gpio_setaf(18, 0, true);
  bk7258_gpio_setaf(19, 0, true);

  putreg32(0, SDIO_REG_INTMASK);
  putreg32(FIFO_TX_THRESH(128) & 0xff00, SDIO_REG_SENDCNT);
  sdio_hw_reset_engine();

  priv->widebus  = false;
  priv->blocklen = 512;
  priv->nblocks  = 1;
  priv->rxbuffer = NULL;
  priv->txbuffer = NULL;
}

static sdio_capset_t bk_capabilities(struct sdio_dev_s *dev)
{
  return SDIO_CAPS_4BIT;
}

static sdio_statset_t bk_status(struct sdio_dev_s *dev)
{
  return SDIO_STATUS_PRESENT;     /* Soldered-down SD NAND */
}

static void bk_widebus(struct sdio_dev_s *dev, bool enable)
{
  ((struct bk7258_sdio_s *)dev)->widebus = enable;
}

static void bk_clock(struct sdio_dev_s *dev, enum sdio_clock_e rate)
{
  struct bk7258_sdio_s *priv = (struct bk7258_sdio_s *)dev;

  switch (rate)
    {
      case CLOCK_SD_TRANSFER_1BIT:
      case CLOCK_SD_TRANSFER_4BIT:
        modifyreg32(SYS_CLKDIV_MODE2, SDIO_CLK_FIELD_MASK, SDIO_CLK_DATA);
        priv->enumclk = false;
        break;

      case CLOCK_IDMODE:
      default:
        modifyreg32(SYS_CLKDIV_MODE2, SDIO_CLK_FIELD_MASK, SDIO_CLK_ENUM);
        priv->enumclk = true;
        break;
    }
}

static int bk_attach(struct sdio_dev_s *dev)
{
  return OK;                      /* Polled: nothing to attach */
}

static int bk_sendcmd(struct sdio_dev_s *dev, uint32_t cmd, uint32_t arg)
{
  struct bk7258_sdio_s *priv = (struct bk7258_sdio_s *)dev;
  uint32_t regval = CMD_INDEX((cmd & MMCSD_CMDIDX_MASK) >>
                              MMCSD_CMDIDX_SHIFT);
  uint32_t rsptype = cmd & MMCSD_RESPONSE_MASK;

  if (rsptype != MMCSD_NO_RESPONSE)
    {
      regval |= CMD_RSP;

      if (rsptype == MMCSD_R2_RESPONSE)
        {
          regval |= CMD_LONG;
        }

      /* The SDK checks response CRC everywhere except R3 (OCR) and the
       * long R2 responses, whose trailing CRC field is not a real CRC.
       */

      if (rsptype != MMCSD_R3_RESPONSE && rsptype != MMCSD_R2_RESPONSE)
        {
          regval |= CMD_CRC_CHECK;
        }
    }

  putreg32(ST_ALL, SDIO_REG_STATUS);
  putreg32(arg, SDIO_REG_ARG);
  putreg32(priv->enumclk ? RSPTO_ENUM : RSPTO_DATA, SDIO_REG_RSPTO);
  putreg32(regval | CMD_START, SDIO_REG_CMD);

  return OK;
}

#ifdef CONFIG_SDIO_BLOCKSETUP
static void bk_blocksetup(struct sdio_dev_s *dev, unsigned int blocklen,
                          unsigned int nblocks)
{
  struct bk7258_sdio_s *priv = (struct bk7258_sdio_s *)dev;

  priv->blocklen = blocklen;
  priv->nblocks  = nblocks;
}
#endif

/* Arm the RX engine BEFORE the read command goes out (the hardware
 * requirement the SDK encodes by calling config_data before CMD17/18).
 */

static int bk_recvsetup(struct sdio_dev_s *dev, uint8_t *buffer,
                        size_t nbytes)
{
  struct bk7258_sdio_s *priv = (struct bk7258_sdio_s *)dev;
  uint32_t regval;

  sdio_hw_reset_engine();

  regval = DATA_BLKSIZE(priv->blocklen) | DATA_BYTE_SEL;

  if (priv->widebus)
    {
      regval |= DATA_BUS_4BIT;
    }

  if (priv->nblocks > 1)
    {
      regval |= DATA_MUL_BLK;
    }

  putreg32(DATATO_DATA, SDIO_REG_DATATO);
  putreg32(regval | DATA_EN, SDIO_REG_DATA);

  priv->rxbuffer = buffer;
  priv->txbuffer = NULL;
  priv->xfrbytes = nbytes;
  return OK;
}

static int bk_sendsetup(struct sdio_dev_s *dev, const uint8_t *buffer,
                        size_t nbytes)
{
  struct bk7258_sdio_s *priv = (struct bk7258_sdio_s *)dev;

  priv->txbuffer = buffer;
  priv->rxbuffer = NULL;
  priv->xfrbytes = nbytes;
  return OK;
}

static int bk_cancel(struct sdio_dev_s *dev)
{
  struct bk7258_sdio_s *priv = (struct bk7258_sdio_s *)dev;

  priv->rxbuffer = NULL;
  priv->txbuffer = NULL;
  priv->xfrbytes = 0;
  sdio_hw_reset_engine();
  return OK;
}

static int bk_waitresponse(struct sdio_dev_s *dev, uint32_t cmd)
{
  struct bk7258_sdio_s *priv = (struct bk7258_sdio_s *)dev;
  uint32_t budget = POLL_BUDGET;
  uint32_t status;
  bool norsp = (cmd & MMCSD_RESPONSE_MASK) == MMCSD_NO_RESPONSE;
  uint32_t done = norsp ? ST_NORSP_END : ST_RSP_END;

  for (; ; )
    {
      status = getreg32(SDIO_REG_STATUS);

      if ((status & done) != 0)
        {
          putreg32(done | ST_RSP_CRC_OK, SDIO_REG_STATUS);
          priv->rspresult = OK;
          return OK;
        }

      if (!norsp && (status & ST_RSP_TIMEOUT) != 0)
        {
          putreg32(ST_RSP_TIMEOUT, SDIO_REG_STATUS);
          priv->rspresult = -ETIMEDOUT;
          return -ETIMEDOUT;
        }

      if (--budget == 0)
        {
          priv->rspresult = -ETIMEDOUT;
          return -ETIMEDOUT;
        }
    }
}

static int bk_recvshort(struct sdio_dev_s *dev, uint32_t cmd, uint32_t *r)
{
  struct bk7258_sdio_s *priv = (struct bk7258_sdio_s *)dev;
  uint32_t status = getreg32(SDIO_REG_STATUS);

  *r = getreg32(SDIO_REG_RSP0);

  /* A response that never arrived must be reported, not invented:
   * mmcsd's card-type detection trusts this return value alone.
   */

  if (priv->rspresult != OK)
    {
      return priv->rspresult;
    }

  if ((cmd & MMCSD_RESPONSE_MASK) == MMCSD_R1_RESPONSE &&
      (status & ST_RSP_CRC_FAIL) != 0)
    {
      putreg32(ST_RSP_CRC_FAIL, SDIO_REG_STATUS);
      return -EIO;
    }

  return OK;
}

static int bk_recvlong(struct sdio_dev_s *dev, uint32_t cmd, uint32_t r[4])
{
  struct bk7258_sdio_s *priv = (struct bk7258_sdio_s *)dev;

  if (priv->rspresult != OK)
    {
      return priv->rspresult;
    }

  r[0] = getreg32(SDIO_REG_RSP0);
  r[1] = getreg32(SDIO_REG_RSP1);
  r[2] = getreg32(SDIO_REG_RSP2);
  r[3] = getreg32(SDIO_REG_RSP3);
  return OK;
}

static void bk_waitenable(struct sdio_dev_s *dev, sdio_eventset_t eventset,
                          uint32_t timeout)
{
  ((struct bk7258_sdio_s *)dev)->waitevents = eventset;
}

/* All actual data movement happens here, synchronously. */

static sdio_eventset_t bk_eventwait(struct sdio_dev_s *dev)
{
  struct bk7258_sdio_s *priv = (struct bk7258_sdio_s *)dev;
  uint32_t budget = POLL_BUDGET;
  uint32_t status;
  uint32_t word;
  size_t i;

  if (priv->rxbuffer != NULL)
    {
      uint8_t *dst = priv->rxbuffer;
      size_t remaining = priv->xfrbytes;

      while (remaining >= 4)
        {
          if ((getreg32(SDIO_REG_FIFOCTRL) & FIFO_RX_RD_READY) == 0)
            {
              if (--budget == 0)
                {
                  priv->rxbuffer = NULL;
                  sdio_hw_reset_engine();
                  return SDIOWAIT_TIMEOUT;
                }

              continue;
            }

          word = getreg32(SDIO_REG_RXFIFO);
          memcpy(dst, &word, 4);
          dst       += 4;
          remaining -= 4;
        }

      priv->rxbuffer = NULL;

      budget = POLL_BUDGET;

      for (; ; )
        {
          status = getreg32(SDIO_REG_STATUS);

          if ((status & ST_DATA_CRC_FAIL) != 0)
            {
              putreg32(ST_ALL, SDIO_REG_STATUS);
              return SDIOWAIT_ERROR;
            }

          if ((status & ST_RECV_END) != 0 ||
              (status & ST_DATA_CRC_OK) != 0)
            {
              putreg32(ST_ALL, SDIO_REG_STATUS);
              return SDIOWAIT_TRANSFERDONE;
            }

          if (--budget == 0)
            {
              sdio_hw_reset_engine();
              return SDIOWAIT_TIMEOUT;
            }
        }
    }

  if (priv->txbuffer != NULL)
    {
      const uint8_t *src = priv->txbuffer;
      size_t remaining = priv->xfrbytes;
      uint32_t regval;

      putreg32(FIFO_BASE_CONFIG | FIFO_TX_RESET | FIFO_STATE_RESET,
               SDIO_REG_FIFOCTRL);
      putreg32(FIFO_BASE_CONFIG, SDIO_REG_FIFOCTRL);
      putreg32(ST_ALL, SDIO_REG_STATUS);

      regval = DATA_BLKSIZE(priv->blocklen) | DATA_BYTE_SEL;

      if (priv->widebus)
        {
          regval |= DATA_BUS_4BIT;
        }

      putreg32(regval, SDIO_REG_DATA);
      putreg32(DATATO_DATA, SDIO_REG_DATATO);

      /* Fill the whole (single) block, then pull the trigger. */

      for (i = 0; i < remaining; i += 4)
        {
          budget = POLL_BUDGET;

          while ((getreg32(SDIO_REG_FIFOCTRL) & FIFO_TX_WR_READY) == 0)
            {
              if (--budget == 0)
                {
                  priv->txbuffer = NULL;
                  sdio_hw_reset_engine();
                  return SDIOWAIT_TIMEOUT;
                }
            }

          memcpy(&word, src + i, 4);
          putreg32(word, SDIO_REG_TXFIFO);
        }

      putreg32(1u << 13, SDIO_REG_INTMASK);
      putreg32(regval | DATA_START_WR, SDIO_REG_DATA);

      priv->txbuffer = NULL;

      budget = POLL_BUDGET;

      for (; ; )
        {
          status = getreg32(SDIO_REG_STATUS);

          if ((status & ST_WR_END) != 0)
            {
              uint32_t wrst = (status & ST_WR_STATUS_MASK) >>
                              ST_WR_STATUS_SHIFT;

              putreg32(ST_ALL, SDIO_REG_STATUS);
              putreg32(0, SDIO_REG_INTMASK);

              /* wr_status 2 = accepted; BK7256 was seen reporting 5.
               * Treat everything except an explicit CRC error status
               * (3) as success, the SDK retries rather than trusts it.
               */

              return (wrst == 3) ? SDIOWAIT_ERROR : SDIOWAIT_TRANSFERDONE;
            }

          if (--budget == 0)
            {
              putreg32(0, SDIO_REG_INTMASK);
              sdio_hw_reset_engine();
              return SDIOWAIT_TIMEOUT;
            }
        }
    }

  /* No transfer armed: the wait was for a command/response event that
   * waitresponse() already consumed.
   */

  return priv->waitevents & (SDIOWAIT_CMDDONE | SDIOWAIT_RESPONSEDONE);
}

static void bk_callbackenable(struct sdio_dev_s *dev,
                              sdio_eventset_t eventset)
{
  /* Fixed media: no insertion/removal events will ever fire. */
}

#if defined(CONFIG_SCHED_WORKQUEUE) && defined(CONFIG_SCHED_HPWORK)
static int bk_registercallback(struct sdio_dev_s *dev, worker_t callback,
                               void *arg)
{
  /* Stored nowhere: with fixed media the event this would announce
   * never happens.
   */

  return OK;
}
#endif

static void bk_gotextcsd(struct sdio_dev_s *dev, const uint8_t *buffer)
{
}

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct bk7258_sdio_s g_sdio_dev =
{
  .dev =
  {
    .mutex          = NXMUTEX_INITIALIZER,
    .reset          = bk_reset,
    .capabilities   = bk_capabilities,
    .status         = bk_status,
    .widebus        = bk_widebus,
    .clock          = bk_clock,
    .attach         = bk_attach,
    .sendcmd        = bk_sendcmd,
#ifdef CONFIG_SDIO_BLOCKSETUP
    .blocksetup     = bk_blocksetup,
#endif
    .recvsetup      = bk_recvsetup,
    .sendsetup      = bk_sendsetup,
    .cancel         = bk_cancel,
    .waitresponse   = bk_waitresponse,
    .recv_r1        = bk_recvshort,
    .recv_r2        = bk_recvlong,
    .recv_r3        = bk_recvshort,
    .recv_r4        = bk_recvshort,
    .recv_r5        = bk_recvshort,
    .recv_r6        = bk_recvshort,
    .recv_r7        = bk_recvshort,
    .waitenable     = bk_waitenable,
    .eventwait      = bk_eventwait,
    .callbackenable = bk_callbackenable,
#if defined(CONFIG_SCHED_WORKQUEUE) && defined(CONFIG_SCHED_HPWORK)
    .registercallback = bk_registercallback,
#endif
    .gotextcsd      = bk_gotextcsd,
  },
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

struct sdio_dev_s *bk7258_sdio_initialize(void)
{
  bk_reset(&g_sdio_dev.dev);
  return &g_sdio_dev.dev;
}
