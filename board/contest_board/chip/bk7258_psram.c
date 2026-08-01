/****************************************************************************
 * boards/arm/bk7258/contest_board/chip/bk7258_psram.c
 *
 * Bring-up for the 16 MB APS128XXO_OB9 octal PSRAM.  The sequence is a
 * faithful transcription of the vendor SDK (bk_idk psram_hal.c /
 * psram_driver.c), which is the only register-level documentation that
 * exists: LDO first, then the AHBP power domain, 80 MHz while the die is
 * configured, and 120 MHz only after the mode registers are set.
 *
 * Two SoC quirks matter here:
 *  - ana_reg13 (0x44010134) is an "analog" register: every write must be
 *    followed by polling busy bit 13 of 0x440100e8 until it clears, or
 *    the value silently never lands.
 *  - The controller's sf_reset (REG2 bit0) follows the family convention:
 *    it is a LEVEL, 1 = released.
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/arch.h>
#include <stdint.h>

#include "arm_internal.h"
#include "bk7258_memorymap.h"
#include "bk7258_clockconfig.h"
#include "bk7258_psram.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define PSRAM_CTRL_BASE       0x46080000ul
#define PSRAM_DATA_BASE       0x60000000ul
#define PSRAM_SIZE            (16ul * 1024 * 1024)

/* Controller registers (word offsets from the SDK psram_ll macros) */

#define PSRAM_REG2            (PSRAM_CTRL_BASE + 0x08)  /* sf_reset, bypass */
#define PSRAM_REG4_MODE       (PSRAM_CTRL_BASE + 0x10)  /* die mode word */
#define PSRAM_REG5_DRIVE      (PSRAM_CTRL_BASE + 0x14)  /* drive strength */
#define PSRAM_REG8_CMD        (PSRAM_CTRL_BASE + 0x20)  /* 1=wr 2=rd 4=reset */
#define PSRAM_REG9_ADDR       (PSRAM_CTRL_BASE + 0x24)
#define PSRAM_REGA_WDATA      (PSRAM_CTRL_BASE + 0x28)
#define PSRAM_REGB_RDATA      (PSRAM_CTRL_BASE + 0x2c)

#define PSRAM_CMD_WRITE       (1u << 0)
#define PSRAM_CMD_READ        (1u << 1)
#define PSRAM_CMD_RESET       (1u << 2)

#define PSRAM_MODE7_OB9       0xd8054049ul   /* APS128XXO_OB9 mode word */
#define PSRAM_DRIVE_OB9       0x380ul
#define PSRAM_ID_OB9          0x8d08ul       /* MR0 identification */

/* System registers */

#define SYS_ANA_REG13         (BK7258_SYS_BASE + (0x4d << 2)) /* PSRAM LDO */
#define SYS_ANA_BUSY          (BK7258_SYS_BASE + (0x3a << 2)) /* bit13 */
#define SYS_POWER_SLEEP       (BK7258_SYS_BASE + (0x10 << 2)) /* AHBP bit5 */
#define SYS_CLK_DIV_MODE2     (BK7258_SYS_BASE + (0x09 << 2)) /* psram 4/5 */

#define ANA_PSLDO_SWB         (1u << 28)
#define ANA_VPSRAMSEL_MASK    (3u << 29)
#define ANA_ENPSRAM           (1u << 31)
#define ANA_BUSY_REG13        (1u << 13)     /* (0x4d - 0x40) */

#define PWD_AHBP              (1u << 5)

#define CKDIV_PSRAM           (1u << 4)      /* /2 within the recipe */
#define CKSEL_PSRAM_480M      (1u << 5)      /* 0 = 320M, 1 = 480M */

#define PSRAM_CKEN            (1u << 19)     /* device clk enable word */

#define PSRAM_POLL_BUDGET     50000

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* Write an analog register and wait for the hardware to latch it. */

static int psram_ana_write(uint32_t value)
{
  uint32_t budget = PSRAM_POLL_BUDGET;

  putreg32(value, SYS_ANA_REG13);

  while ((getreg32(SYS_ANA_BUSY) & ANA_BUSY_REG13) != 0)
    {
      if (--budget == 0)
        {
          return -1;
        }
    }

  return 0;
}

static int psram_cmd(uint32_t cmd)
{
  uint32_t budget = PSRAM_POLL_BUDGET;

  putreg32(cmd, PSRAM_REG8_CMD);

  while ((getreg32(PSRAM_REG8_CMD) & cmd) != 0)
    {
      if (--budget == 0)
        {
          return -1;
        }
    }

  return 0;
}

/* Read one PSRAM die mode register through the command engine. */

static int psram_mr_read(uint32_t addr, uint32_t *value)
{
  putreg32(addr, PSRAM_REG9_ADDR);

  if (psram_cmd(PSRAM_CMD_READ) < 0)
    {
      return -1;
    }

  *value = getreg32(PSRAM_REGB_RDATA);
  return 0;
}

static int psram_mr_write(uint32_t addr, uint32_t value)
{
  putreg32(addr, PSRAM_REG9_ADDR);
  putreg32(value, PSRAM_REGA_WDATA);
  return psram_cmd(PSRAM_CMD_WRITE);
}

/* Sanity: distinct patterns at spread-out addresses, written before any
 * is read back, so aliasing (address wrap) shows up as a mismatch.
 */

static int psram_selftest(void)
{
  static const uint32_t off[] =
  {
    0x0, 0x4, 0x100, 0x00800000, 0x00fffffc
  };

  volatile uint32_t *p;
  uint32_t i;

  for (i = 0; i < sizeof(off) / sizeof(off[0]); i++)
    {
      p  = (volatile uint32_t *)(PSRAM_DATA_BASE + off[i]);
      *p = off[i] ^ 0xa5a5a5a5u;
    }

  for (i = 0; i < sizeof(off) / sizeof(off[0]); i++)
    {
      p = (volatile uint32_t *)(PSRAM_DATA_BASE + off[i]);
      if (*p != (off[i] ^ 0xa5a5a5a5u))
        {
          return -1;
        }
    }

  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

size_t bk7258_psram_init(void)
{
  uint32_t mr;
  uint32_t regval;
  int tries;

  /* LDO: voltage select first, then enable, each write latched. */

  regval  = getreg32(SYS_ANA_REG13);
  regval |= ANA_PSLDO_SWB;
  regval &= ~ANA_VPSRAMSEL_MASK;

  if (psram_ana_write(regval) < 0)
    {
      return 0;
    }

  up_udelay(100);

  if (psram_ana_write(regval | ANA_ENPSRAM) < 0)
    {
      return 0;
    }

  up_udelay(1000);

  /* AHBP power domain on (0 = powered). */

  modifyreg32(SYS_POWER_SLEEP, PWD_AHBP, 0);

  /* 80 MHz while configuring: 320 MHz source, recipe divider. */

  modifyreg32(SYS_CLK_DIV_MODE2, CKSEL_PSRAM_480M, CKDIV_PSRAM);

  /* Controller clock on. */

  modifyreg32(BK7258_SYS_CPU_DEVICE_CKEN, 0, PSRAM_CKEN);

  up_udelay(200);

  /* Controller: release reset, bypass, OB9 mode word, drive strength. */

  putreg32(1, PSRAM_REG2);
  modifyreg32(PSRAM_REG2, 0, 1u << 1);
  putreg32(PSRAM_MODE7_OB9, PSRAM_REG4_MODE);
  putreg32(PSRAM_DRIVE_OB9, PSRAM_REG5_DRIVE);

  psram_cmd(PSRAM_CMD_RESET);
  up_udelay(100);

  /* The die must identify before anything else is trusted. */

  for (tries = 0; tries < 5; tries++)
    {
      if (psram_mr_read(0x0, &mr) == 0 && mr == PSRAM_ID_OB9)
        {
          break;
        }

      putreg32(0, PSRAM_REG2);
      up_udelay(10);
      putreg32(1, PSRAM_REG2);
      modifyreg32(PSRAM_REG2, 0, 1u << 1);
      putreg32(PSRAM_MODE7_OB9, PSRAM_REG4_MODE);
      putreg32(PSRAM_DRIVE_OB9, PSRAM_REG5_DRIVE);
      psram_cmd(PSRAM_CMD_RESET);
      up_udelay(100);
    }

  if (tries >= 5)
    {
      return 0;
    }

  /* Latency tuning per the SDK: MR0[4:2]=100b (read latency),
   * MR4[7:5]=110b (write latency), MR8 |= 0x40.
   */

  if (psram_mr_write(0x0, (mr & ~(7u << 2)) | (4u << 2)) < 0)
    {
      return 0;
    }

  if (psram_mr_read(0x4, &mr) < 0 ||
      psram_mr_write(0x4, (mr & ~(7u << 5)) | (6u << 5)) < 0)
    {
      return 0;
    }

  if (psram_mr_read(0x8, &mr) < 0 ||
      psram_mr_write(0x8, mr | 0x40) < 0)
    {
      return 0;
    }

  up_udelay(1000);

  /* Full speed: 480 MHz source with the same divider -> 120 MHz. */

  modifyreg32(SYS_CLK_DIV_MODE2, 0, CKSEL_PSRAM_480M | CKDIV_PSRAM);

  up_udelay(100);

  if (psram_selftest() < 0)
    {
      return 0;
    }

  return PSRAM_SIZE;
}
