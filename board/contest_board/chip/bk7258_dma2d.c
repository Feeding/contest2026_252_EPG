/****************************************************************************
 * boards/arm/bk7258/contest_board/chip/bk7258_dma2d.c
 *
 * DMA2D (0x48080000) memory-to-memory pixel format conversion, polled.
 * One job here: split a 320x160 packed-YUV422 frame (as the hardware
 * JPEG decoder emits it, format code 0 on both blocks -- the vendor's
 * production pairing, whatever the documentation calls the byte order)
 * into two 160x160 RGB565 panel buffers using windowed conversions with
 * a source line offset.
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/arch.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>

#include "arm_internal.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define D2D_BASE              0x48080000ul

#define D2D_REG_ID            (D2D_BASE + 0x00)
#define D2D_REG_MODCTRL       (D2D_BASE + 0x08)   /* bit0 rst, bit1 gate */
#define D2D_REG_CTRL          (D2D_BASE + 0x10)   /* mode, start */
#define D2D_REG_INTSTS        (D2D_BASE + 0x14)
#define D2D_REG_INTCLR        (D2D_BASE + 0x18)
#define D2D_REG_FGADDR        (D2D_BASE + 0x1c)
#define D2D_REG_FGOFFS        (D2D_BASE + 0x20)
#define D2D_REG_FGCFG         (D2D_BASE + 0x2c)
#define D2D_REG_OUTCFG        (D2D_BASE + 0x44)
#define D2D_REG_OUTADDR       (D2D_BASE + 0x4c)
#define D2D_REG_OUTOFFS       (D2D_BASE + 0x50)
#define D2D_REG_SIZE          (D2D_BASE + 0x54)

#define D2D_CTRL_M2M_PFC      (1u << 16)          /* mode[18:16] = 001 */
#define D2D_CTRL_START        (1u << 0)

#define D2D_STS_DONE          (1u << 1)
#define D2D_STS_XFER_ERR      (1u << 0)
#define D2D_STS_CFG_ERR       (1u << 5)

static uint32_t g_d2d_fgcfg = 0x0000000bu;        /* calibrated at runtime */

void bk7258_dma2d_set_fgcfg(uint32_t fmt2, int reve)
{
  g_d2d_fgcfg = 0x0bu | (fmt2 << 6) | ((uint32_t)(reve != 0) << 22);
}
#define D2D_OUT_RGB565        0x00000002u

#define AVI_W                 320
#define PANEL_W               160
#define PANEL_H               160

/****************************************************************************
 * Private Data
 ****************************************************************************/

static bool g_d2d_ready;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int d2d_run(uint32_t src, uint32_t dst)
{
  uint32_t budget = 500000;

  putreg32(D2D_CTRL_M2M_PFC, D2D_REG_CTRL);
  putreg32(src, D2D_REG_FGADDR);
  putreg32(PANEL_W, D2D_REG_FGOFFS);        /* skip the other half-row */
  putreg32(g_d2d_fgcfg, D2D_REG_FGCFG);
  putreg32(D2D_OUT_RGB565, D2D_REG_OUTCFG);
  putreg32(dst, D2D_REG_OUTADDR);
  putreg32(0, D2D_REG_OUTOFFS);
  putreg32(((uint32_t)PANEL_W << 16) | PANEL_H, D2D_REG_SIZE);
  putreg32(0x3f, D2D_REG_INTCLR);

  /* Completion: the start bit self-clears (the SDK's is_transfer_busy
   * semantics).  The int-status DONE bit never latched with the enables
   * off -- polling it burned the whole budget every frame, silently
   * starving the display path.  Keep the enables on (NVIC stays masked)
   * and watch the start bit as primary.
   */

  putreg32(D2D_CTRL_M2M_PFC | (0x3fu << 8) | D2D_CTRL_START,
           D2D_REG_CTRL);

  while (budget-- > 0)
    {
      uint32_t sts = getreg32(D2D_REG_INTSTS);

      if ((sts & (D2D_STS_XFER_ERR | D2D_STS_CFG_ERR)) != 0)
        {
          putreg32(0x3f, D2D_REG_INTCLR);
          return -EIO;
        }

      if ((getreg32(D2D_REG_CTRL) & D2D_CTRL_START) == 0 ||
          (sts & D2D_STS_DONE) != 0)
        {
          putreg32(0x3f, D2D_REG_INTCLR);
          return OK;
        }
    }

  putreg32(0x3f, D2D_REG_INTCLR);
  return -ETIMEDOUT;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_dma2d_split
 *
 * Description:
 *   Convert one 320x160 packed-YUV422 frame into two 160x160 RGB565
 *   buffers (left half -> dstl, right half -> dstr).  Polled; both
 *   buffers must be 4-byte aligned.
 *
 ****************************************************************************/

int bk7258_dma2d_split(const void *src, void *dstl, void *dstr)
{
  int ret;

  if (!g_d2d_ready)
    {
      putreg32(0x1, D2D_REG_MODCTRL);         /* reset */
      up_udelay(10);
      putreg32(0x2, D2D_REG_MODCTRL);         /* release + clock gate on */

      if (getreg32(D2D_REG_ID) == 0)
        {
          return -ENODEV;
        }

      g_d2d_ready = true;
    }

  ret = d2d_run((uint32_t)(uintptr_t)src, (uint32_t)(uintptr_t)dstl);
  if (ret == OK)
    {
      ret = d2d_run((uint32_t)(uintptr_t)src + PANEL_W * 2,
                    (uint32_t)(uintptr_t)dstr);
    }

  return ret;
}
