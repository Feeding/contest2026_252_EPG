/****************************************************************************
 * boards/arm/bk7258/contest_board/chip/bk7258_jpegdec.c
 *
 * Hardware JPEG decoder (0x48040000), polled, memory to memory.  The
 * hardware does entropy decode + IDCT but the CPU must parse the JFIF
 * header itself and hand over the tables: Huffman code words as
 * (code << 8) | value, the DQT pre-scaled by the Arai IPSF factors in
 * raster order (int32 each), and the zigzag map.  The SOS entropy-data
 * file offset goes into BASE_FFDA.  Output is packed YUYV 4:2:2.
 *
 * Constraints of this block (verified against the vendor clips): three
 * component baseline, Y sampling 0x21, chroma 0x11, quant ids Y=0/C=1,
 * huffman selectors 00/11, no progressive, headers within the first
 * 1 KB.  The input buffer needs 2 KB of readable slack past the end --
 * the AHB master over-reads.
 *
 * Recipe transcribed from bk_idk jpeg_dec_hal.c / jpeg_dec_driver.c /
 * jpeg_dec_ll_macro_def.h; the soft reset is the GDMA-style "0 means
 * held, 1 means running" level.
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/arch.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>

#include "arm_internal.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define JD_BASE               0x48040000ul

#define JD_REG_RESET          (JD_BASE + 0x008)  /* bit0 run, bit1 gate */
#define JD_REG_MCU_X          (JD_BASE + 0x014)
#define JD_REG_MCU_Y          (JD_BASE + 0x018)
#define JD_REG_CMD            (JD_BASE + 0x020)  /* 1=start 4=clear */
#define JD_REG_XPIXEL         (JD_BASE + 0x028)
#define JD_REG_STATUS_BUSY    (JD_BASE + 0x034)  /* bit0 busy, err bits */
#define JD_REG_MCU_BLK        (JD_BASE + 0x03c)
#define JD_REG_LEN_DCY        (JD_BASE + 0x040)  /* 16 words */
#define JD_REG_LEN_DCUV       (JD_BASE + 0x080)
#define JD_REG_LEN_ACY        (JD_BASE + 0x0c0)
#define JD_REG_LEN_ACUV       (JD_BASE + 0x100)
#define JD_REG_CTRL           (JD_BASE + 0x140)  /* en, out_fmt, dri_bps */
#define JD_REG_AUTO           (JD_BASE + 0x148)
#define JD_REG_UVVLD          (JD_BASE + 0x14c)
#define JD_REG_HUFCNT         (JD_BASE + 0x158)
#define JD_REG_RADDR          (JD_BASE + 0x160)
#define JD_REG_WADDR          (JD_BASE + 0x164)
#define JD_REG_RDLEN          (JD_BASE + 0x168)
#define JD_REG_WRLEN          (JD_BASE + 0x16c)
#define JD_REG_FFDA           (JD_BASE + 0x170)
#define JD_REG_RDCNT          (JD_BASE + 0x174)
#define JD_REG_INTEN          (JD_BASE + 0x178)
#define JD_REG_INTSTS         (JD_BASE + 0x17c)  /* bit8 done, bit9 huf */

#define JD_TBL_HUF_DCY        (JD_BASE + 0x200)
#define JD_TBL_HUF_DCUV       (JD_BASE + 0x300)
#define JD_TBL_HUF_ACY        (JD_BASE + 0x400)
#define JD_TBL_HUF_ACUV       (JD_BASE + 0x800)
#define JD_TBL_ZIGZAG         (JD_BASE + 0xc00)
#define JD_TBL_TMP0           (JD_BASE + 0xd00)
#define JD_TBL_DQT0           (JD_BASE + 0xe00)
#define JD_TBL_DQT1           (JD_BASE + 0xf00)

#define JD_CMD_START          1
#define JD_CMD_CLEAR          4

#define JD_CTRL_EN            (1u << 0)
#define JD_CTRL_DRI_BPS       (1u << 29)

#define JD_INT_FRAME          (1u << 8)
#define JD_INT_HUF_ERR        (1u << 9)

#define JD_HDR_WINDOW         1024

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Zigzag order and Arai prescale factors (x8192), the same constants
 * TJpgDec carries -- the SDK's parser is TJpgDec-derived.
 */

static const uint8_t g_zig[64] =
{
   0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
  12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
  35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
  58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};

static const uint16_t g_ipsf[64] =
{
  (uint16_t)(1.00000 * 8192), (uint16_t)(1.38704 * 8192),
  (uint16_t)(1.30656 * 8192), (uint16_t)(1.17588 * 8192),
  (uint16_t)(1.00000 * 8192), (uint16_t)(0.78570 * 8192),
  (uint16_t)(0.54120 * 8192), (uint16_t)(0.27590 * 8192),
  (uint16_t)(1.38704 * 8192), (uint16_t)(1.92388 * 8192),
  (uint16_t)(1.81226 * 8192), (uint16_t)(1.63099 * 8192),
  (uint16_t)(1.38704 * 8192), (uint16_t)(1.08979 * 8192),
  (uint16_t)(0.75066 * 8192), (uint16_t)(0.38268 * 8192),
  (uint16_t)(1.30656 * 8192), (uint16_t)(1.81226 * 8192),
  (uint16_t)(1.70711 * 8192), (uint16_t)(1.53636 * 8192),
  (uint16_t)(1.30656 * 8192), (uint16_t)(1.02656 * 8192),
  (uint16_t)(0.70711 * 8192), (uint16_t)(0.36048 * 8192),
  (uint16_t)(1.17588 * 8192), (uint16_t)(1.63099 * 8192),
  (uint16_t)(1.53636 * 8192), (uint16_t)(1.38268 * 8192),
  (uint16_t)(1.17588 * 8192), (uint16_t)(0.92388 * 8192),
  (uint16_t)(0.63638 * 8192), (uint16_t)(0.32442 * 8192),
  (uint16_t)(1.00000 * 8192), (uint16_t)(1.38704 * 8192),
  (uint16_t)(1.30656 * 8192), (uint16_t)(1.17588 * 8192),
  (uint16_t)(1.00000 * 8192), (uint16_t)(0.78570 * 8192),
  (uint16_t)(0.54120 * 8192), (uint16_t)(0.27590 * 8192),
  (uint16_t)(0.78570 * 8192), (uint16_t)(1.08979 * 8192),
  (uint16_t)(1.02656 * 8192), (uint16_t)(0.92388 * 8192),
  (uint16_t)(0.78570 * 8192), (uint16_t)(0.61732 * 8192),
  (uint16_t)(0.42522 * 8192), (uint16_t)(0.21677 * 8192),
  (uint16_t)(0.54120 * 8192), (uint16_t)(0.75066 * 8192),
  (uint16_t)(0.70711 * 8192), (uint16_t)(0.63638 * 8192),
  (uint16_t)(0.54120 * 8192), (uint16_t)(0.42522 * 8192),
  (uint16_t)(0.29290 * 8192), (uint16_t)(0.14932 * 8192),
  (uint16_t)(0.27590 * 8192), (uint16_t)(0.38268 * 8192),
  (uint16_t)(0.36048 * 8192), (uint16_t)(0.32442 * 8192),
  (uint16_t)(0.27590 * 8192), (uint16_t)(0.21678 * 8192),
  (uint16_t)(0.14932 * 8192), (uint16_t)(0.07612 * 8192)
};

static bool g_jd_ready;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* Load one DHT: write the 16 per-length counts to lenreg, the canonical
 * (code << 8) | value words to tblreg.  Returns the symbol count.
 */

static int jd_load_huff(const uint8_t *bits, const uint8_t *vals,
                        uintptr_t lenreg, uintptr_t tblreg)
{
  uint32_t code = 0;
  int total = 0;
  int i;
  int n;

  for (i = 0; i < 16; i++)
    {
      putreg32(bits[i], lenreg + i * 4);
      total += bits[i];
    }

  for (i = 0; i < 16; i++)
    {
      for (n = 0; n < bits[i]; n++)
        {
          putreg32((code << 8) | *vals++, tblreg);
          tblreg += 4;
          code++;
        }

      code <<= 1;
    }

  return total;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_jpegdec_decode
 *
 * Description:
 *   Decode one baseline JFIF frame (headers included) into a packed
 *   YUYV 4:2:2 buffer of width*height*2 bytes.  The input buffer must
 *   have 2 KB of readable slack past jpg + len.  Polled; returns 0 or
 *   a negated errno.
 *
 ****************************************************************************/

int bk7258_jpegdec_decode(const uint8_t *jpg, size_t len,
                          uint8_t *out, int width, int height)
{
  const uint8_t *dqt[2] = { NULL, NULL };
  const uint8_t *bits[2][2] = { { NULL, NULL }, { NULL, NULL } };
  const uint8_t *vals[2][2] = { { NULL, NULL }, { NULL, NULL } };
  uint32_t sos_off = 0;
  uint32_t counts;
  uint32_t budget;
  size_t i;
  int cls;
  int num;
  int t;

  if (jpg == NULL || out == NULL || len < 16 ||
      jpg[0] != 0xff || jpg[1] != 0xd8)
    {
      return -EINVAL;
    }

  if (!g_jd_ready)
    {
      putreg32(0, JD_REG_RESET);
      up_udelay(10);
      putreg32(3, JD_REG_RESET);          /* run + gate bypass */
      g_jd_ready = true;
    }

  /* Header walk: everything must sit inside the first 1 KB. */

  i = 2;
  while (i + 4 <= len && i < JD_HDR_WINDOW)
    {
      uint32_t seglen;
      uint8_t marker;

      if (jpg[i] != 0xff)
        {
          return -EBADMSG;
        }

      marker = jpg[i + 1];
      seglen = ((uint32_t)jpg[i + 2] << 8) | jpg[i + 3];

      if (marker == 0xdb)                 /* DQT: one or more tables */
        {
          uint32_t o = i + 4;

          while (o + 65 <= i + 2 + seglen)
            {
              if ((jpg[o] & 0xf0) != 0)
                {
                  return -EBADMSG;        /* not 8-bit */
                }

              dqt[jpg[o] & 1] = &jpg[o + 1];
              o += 65;
            }
        }
      else if (marker == 0xc4)            /* DHT: one or more tables */
        {
          uint32_t o = i + 4;

          while (o + 17 <= i + 2 + seglen)
            {
              int total = 0;
              int k;

              cls = (jpg[o] >> 4) & 1;
              num = jpg[o] & 1;
              bits[num][cls] = &jpg[o + 1];

              for (k = 0; k < 16; k++)
                {
                  total += jpg[o + 1 + k];
                }

              vals[num][cls] = &jpg[o + 17];
              o += 17 + total;
            }
        }
      else if (marker == 0xc0)            /* SOF0: verify the profile */
        {
          int w = ((int)jpg[i + 7] << 8) | jpg[i + 8];
          int h = ((int)jpg[i + 5] << 8) | jpg[i + 6];

          if (w != width || h != height || jpg[i + 9] != 3 ||
              jpg[i + 11] != 0x21)
            {
              return -ENOTSUP;
            }
        }
      else if (marker == 0xda)            /* SOS: entropy data follows */
        {
          sos_off = i + 2 + seglen;
          break;
        }
      else if (marker == 0xd9 || (marker >= 0xd0 && marker <= 0xd7))
        {
          return -EBADMSG;
        }

      i += 2 + seglen;
    }

  if (sos_off == 0 || dqt[0] == NULL || dqt[1] == NULL ||
      bits[0][0] == NULL || bits[0][1] == NULL ||
      bits[1][0] == NULL || bits[1][1] == NULL)
    {
      return -EBADMSG;
    }

  /* Scratch RAM clean, then the four Huffman tables. */

  for (i = 0; i < 64; i++)
    {
      putreg32(0, JD_TBL_TMP0 + i * 4);
    }

  counts  = (uint32_t)jd_load_huff(bits[0][0], vals[0][0],
                                   JD_REG_LEN_DCY, JD_TBL_HUF_DCY);
  counts |= (uint32_t)jd_load_huff(bits[1][0], vals[1][0],
                                   JD_REG_LEN_DCUV, JD_TBL_HUF_DCUV) << 8;
  counts |= (uint32_t)jd_load_huff(bits[0][1], vals[0][1],
                                   JD_REG_LEN_ACY, JD_TBL_HUF_ACY) << 16;
  counts |= (uint32_t)jd_load_huff(bits[1][1], vals[1][1],
                                   JD_REG_LEN_ACUV, JD_TBL_HUF_ACUV) << 24;
  putreg32(counts, JD_REG_HUFCNT);

  /* DQT pre-scaled to raster order, plus the zigzag map itself. */

  for (t = 0; t < 2; t++)
    {
      uintptr_t reg = t ? JD_TBL_DQT1 : JD_TBL_DQT0;

      for (i = 0; i < 64; i++)
        {
          uint8_t z = g_zig[i];

          putreg32((uint32_t)dqt[t][i] * g_ipsf[z], reg + (uintptr_t)z * 4);
        }
    }

  for (i = 0; i < 64; i++)
    {
      putreg32(g_zig[i], JD_TBL_ZIGZAG + i * 4);
    }

  /* Buffers, geometry, mode. */

  putreg32((uint32_t)(uintptr_t)jpg, JD_REG_RADDR);
  putreg32((uint32_t)(uintptr_t)out, JD_REG_WADDR);
  putreg32(len + 2048, JD_REG_RDLEN);
  putreg32((uint32_t)width * height * 2, JD_REG_WRLEN);
  putreg32(sos_off, JD_REG_FFDA);
  putreg32((uint32_t)width * height * 2 / 64 - 1, JD_REG_MCU_BLK);

  putreg32(JD_INT_FRAME | JD_INT_HUF_ERR, JD_REG_INTEN);
  putreg32(1, JD_REG_AUTO);
  putreg32(JD_CTRL_DRI_BPS | JD_CTRL_EN, JD_REG_CTRL);

  putreg32(0, JD_REG_MCU_X);
  putreg32(0, JD_REG_MCU_Y);
  putreg32((uint32_t)width, JD_REG_XPIXEL);
  putreg32(JD_CMD_CLEAR, JD_REG_CMD);
  putreg32(JD_CMD_START, JD_REG_CMD);

  /* A 320x160 frame decodes in a few ms; budget generously. */

  budget = 2000000;

  for (; ; )
    {
      uint32_t sts = getreg32(JD_REG_INTSTS);

      if ((sts & JD_INT_HUF_ERR) != 0)
        {
          break;
        }

      if ((sts & JD_INT_FRAME) != 0)
        {
          /* Success path: quiesce and report. */

          putreg32(0xffff, JD_REG_INTSTS);
          putreg32(JD_CMD_CLEAR, JD_REG_CMD);
          putreg32(0, JD_REG_UVVLD);
          modifyreg32(JD_REG_CTRL, JD_CTRL_EN, 0);
          return OK;
        }

      if (--budget == 0)
        {
          break;
        }
    }

  /* Error path: clear, magic huffman-length restore, disable. */

  putreg32(0xffff, JD_REG_INTSTS);
  putreg32(JD_CMD_CLEAR, JD_REG_CMD);
  putreg32(0xffffffff, JD_REG_HUFCNT);
  putreg32(0, JD_REG_UVVLD);
  modifyreg32(JD_REG_CTRL, JD_CTRL_EN, 0);
  return -EIO;
}
