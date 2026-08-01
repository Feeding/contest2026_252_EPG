/****************************************************************************
 * boards/arm/bk7258/contest_board/chip/bk7258_camera.c
 *
 * Single-shot JPEG capture from the GC2145 DVP sensor.  The pipeline is
 * three blocks: YUV_BUF (0x48020000) slices the DVP stream into an SRAM
 * ping-pong line buffer, the JPEG encoder (0x48000000) eats the slices,
 * and the CPU drains the encoder's stream FIFO into the caller's buffer
 * (the GDMA block bus-faults on CPU0 -- unresolved protection gating --
 * but bitrate control caps frames at 35 KB, well within polling reach).
 * The encoder has no output-address register of its own, and software
 * must load the quantization tables (no hardware defaults).
 *
 * Strategy for a clean single frame: configure everything, start the
 * SENSOR first and let auto-exposure converge, and only then arm DMA +
 * encoder -- the first EOF observed is already a mature frame, so no
 * multi-frame DMA bookkeeping is needed.
 *
 * Register recipe transcribed from bk_idk (jpeg_ll/yuv_buf_ll/dvp
 * drivers) and bk_avdk_smp (GC2145 tables); every constant is cited in
 * PORTING_NOTES.
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/arch.h>
#include <nuttx/i2c/i2c_master.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>

#include "arm_internal.h"
#include "bk7258_memorymap.h"
#include "bk7258_gc2145_tables.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define JPEG_BASE             0x48000000ul
#define YUV_BASE              0x48020000ul

#define JPEG_REG_RESET        (JPEG_BASE + 0x08)
#define JPEG_REG_FIFO         (JPEG_BASE + 0x14)
#define JPEG_REG_STATUS       (JPEG_BASE + 0x18)   /* bit1 EOF */
#define JPEG_REG_BYTECNT      (JPEG_BASE + 0x1c)
#define JPEG_REG_INTEN        (JPEG_BASE + 0x30)
#define JPEG_REG_CFG          (JPEG_BASE + 0x34)
#define JPEG_REG_TGT_H        (JPEG_BASE + 0x38)
#define JPEG_REG_TGT_L        (JPEG_BASE + 0x3c)
#define JPEG_REG_QUANT        (JPEG_BASE + 0x80)

#define YUV_REG_RESET         (YUV_BASE + 0x08)
#define YUV_REG_CTRL          (YUV_BASE + 0x10)
#define YUV_REG_PIXEL         (YUV_BASE + 0x14)
#define YUV_REG_EMBASE        (YUV_BASE + 0x20)
#define YUV_REG_INTEN         (YUV_BASE + 0x24)
#define YUV_REG_STATUS        (YUV_BASE + 0x28)
#define YUV_REG_INTMASK       (YUV_BASE + 0x2c)
#define YUV_REG_EMRBASE       (YUV_BASE + 0x30)
#define YUV_REG_RESIZE        (YUV_BASE + 0x34)

/* JPEG stream FIFO status (REG_0x8): bit26 = stream fifo empty */

#define JPEG_REG_FIFOSTAT     (JPEG_BASE + 0x20)
#define JPEG_FIFO_EMPTY       (1u << 26)

/* Frame geometry: VGA */

#define CAM_W                 640
#define CAM_H                 480
#define CAM_XP                (CAM_W / 8)          /* 0x50 */
#define CAM_YP                (CAM_H / 8)          /* 0x3c */

/* The line ping-pong buffer must be SRAM (PSRAM cannot keep up with the
 * line rate): 32 KB carved off the top of heap region 2 by boardinit.
 */

#define CAM_EMBUF             0x28098000ul
#define CAM_EMBUF_SIZE        0x5000               /* width * 16 * 2 */

#define SCCB_ADDR             0x3c
#define SCCB_FREQ             100000

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct i2c_master_s *g_sccb;
static bool g_cam_ready;

static const uint32_t g_jpeg_quant[32] =
{
  0x07060608, 0x07080506, 0x09090707, 0x140c0a08,
  0x0b0b0c0d, 0x1312190c, 0x1a1d140f, 0x1a1d1e1f,
  0x24201c1c, 0x2220272e, 0x1c1c232c, 0x2c293728,
  0x34343130, 0x39271f34, 0x3c32383d, 0x3234332e,
  0x0c090909, 0x0d180c0b, 0x2132180d, 0x3232211c,
  0x32323232, 0x32323232, 0x32323232, 0x32323232,
  0x32323232, 0x32323232, 0x32323232, 0x32323232,
  0x32323232, 0x32323232, 0x32323232, 0x32323232
};


/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int sccb_write(uint8_t reg, uint8_t val)
{
  struct i2c_msg_s msg;
  uint8_t buf[2] = { reg, val };

  msg.frequency = SCCB_FREQ;
  msg.addr      = SCCB_ADDR;
  msg.flags     = 0;
  msg.buffer    = buf;
  msg.length    = 2;

  return I2C_TRANSFER(g_sccb, &msg, 1);
}

static int sccb_read(uint8_t reg, uint8_t *val)
{
  struct i2c_msg_s msg[2];

  msg[0].frequency = SCCB_FREQ;
  msg[0].addr      = SCCB_ADDR;
  msg[0].flags     = 0;
  msg[0].buffer    = &reg;
  msg[0].length    = 1;
  msg[1].frequency = SCCB_FREQ;
  msg[1].addr      = SCCB_ADDR;
  msg[1].flags     = I2C_M_READ;
  msg[1].buffer    = val;
  msg[1].length    = 1;

  return I2C_TRANSFER(g_sccb, msg, 2);
}

static int sccb_write_table(const uint8_t (*tbl)[2], size_t n)
{
  size_t i;
  int ret;

  for (i = 0; i < n; i++)
    {
      ret = sccb_write(tbl[i][0], tbl[i][1]);
      if (ret < 0)
        {
          return ret;
        }

      /* Soft-reset entries deserve a settle; everything else just gets
       * a guard gap on top of the 100 kHz bus timing.
       */

      if (tbl[i][0] == 0xfe && tbl[i][1] == 0xf0)
        {
          up_mdelay(2);
        }
      else
        {
          up_udelay(30);
        }
    }

  return OK;
}

static void cam_pins_setup(void)
{
  int pin;

  /* PCLK/HSYNC/VSYNC (GPIO29-31) and D0-D7 (GPIO32-39): function
   * index 0, pad handed to the peripheral (func_en + output stage off,
   * no pulls) -- the value proven right in the MCLK saga.
   */

  modifyreg32(BK7258_SYS_BASE + (0x33 << 2),
              (0xfu << 20) | (0xfu << 24) | (0xfu << 28), 0);
  putreg32(0, BK7258_SYS_BASE + (0x34 << 2));

  for (pin = 29; pin <= 39; pin++)
    {
      putreg32((1u << 6) | (1u << 3), 0x44000400 + pin * 4);
    }
}

static void cam_engine_setup(void)
{
  int i;

  /* YUV_BUF: reset, slice geometry, sync polarity (GC2145 = both
   * active high, no reversal), YUYV order, HSYNC-rising start.
   */

  putreg32(0, YUV_REG_RESET);
  putreg32(3, YUV_REG_RESET);                 /* release + gate bypass */

  putreg32(0x3c, YUV_REG_INTMASK);
  putreg32((1u << 9) | (3u << 10) | (1u << 19), YUV_REG_CTRL);
  putreg32(((uint32_t)(CAM_W * CAM_H / 128) << 16) |
           ((uint32_t)CAM_YP << 8) | CAM_XP, YUV_REG_PIXEL);
  putreg32(((uint32_t)CAM_YP << 9) | ((uint32_t)CAM_XP << 1),
           YUV_REG_RESIZE);
  putreg32(CAM_EMBUF, YUV_REG_EMBASE);
  putreg32(CAM_EMBUF, YUV_REG_EMRBASE);
  putreg32(0x1ff, YUV_REG_INTEN);             /* latch status, no NVIC */

  /* JPEG encoder: reset, quantization tables (mandatory), byte-rate
   * window for VGA, geometry.  Enable bit stays off until snap time.
   */

  putreg32(0, JPEG_REG_RESET);
  putreg32(1, JPEG_REG_RESET);
  putreg32(getreg32(JPEG_REG_STATUS), JPEG_REG_STATUS);

  for (i = 0; i < 32; i++)
    {
      putreg32(g_jpeg_quant[i], JPEG_REG_QUANT + i * 4);
    }

  putreg32(0x8c00, JPEG_REG_TGT_H);
  putreg32(0x5000, JPEG_REG_TGT_L);
  putreg32(0x08, JPEG_REG_INTEN);             /* EOF latch */
  putreg32((1u << 1) | ((uint32_t)CAM_XP << 8) | (1u << 16) |
           (1u << 17) | (3u << 18) | ((uint32_t)CAM_YP << 24),
           JPEG_REG_CFG);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_camera_snap
 *
 * Description:
 *   Capture one JPEG frame into buf (32-byte aligned, >= 64 KB advised).
 *   Returns the JFIF length (SOI..EOI) or a negated errno.
 *
 ****************************************************************************/

int bk7258_camera_snap(uint8_t *buf, size_t maxlen)
{
  uint32_t budget;
  uint32_t count;
  uint8_t id;
  size_t scan;
  int ret;

  if (buf == NULL || maxlen < 0x8000)
    {
      return -EINVAL;
    }

  if (!g_cam_ready)
    {
      extern struct i2c_master_s *bk7258_i2c_bitbang_initialize(void);

      g_sccb = bk7258_i2c_bitbang_initialize();
      if (g_sccb == NULL)
        {
          return -ENODEV;
        }

      cam_pins_setup();
      cam_engine_setup();

      /* The sensor must identify before we trust anything else. */

      sccb_write(0xfe, 0x00);
      ret = sccb_read(0xf0, &id);
      if (ret < 0 || id != 0x21)
        {
          return (ret < 0) ? ret : -ENODEV;
        }

      ret = sccb_write_table(g_gc2145_init,
                             sizeof(g_gc2145_init) / 2);
      if (ret == OK)
        {
          ret = sccb_write_table(g_gc2145_vga,
                                 sizeof(g_gc2145_vga) / 2);
        }

      if (ret == OK)
        {
          ret = sccb_write_table(g_gc2145_vga_20fps,
                                 sizeof(g_gc2145_vga_20fps) / 2);
        }

      if (ret < 0)
        {
          return ret;
        }

      /* Let auto-exposure/white-balance converge before first use. */

      up_mdelay(400);
      g_cam_ready = true;
    }

  /* Clean slate.  The GDMA block bus-faults on CPU0 (unresolved
   * protection gating), so the CPU itself drains the stream FIFO --
   * bitrate control caps a frame at 35 KB while a tight polling loop
   * can move tens of MB/s, a comfortable 50x margin.
   */

  putreg32(getreg32(JPEG_REG_STATUS), JPEG_REG_STATUS);
  putreg32(getreg32(YUV_REG_STATUS), YUV_REG_STATUS);

  modifyreg32(JPEG_REG_RESET, 0, 1u << 1);    /* clk gate bypass */
  modifyreg32(JPEG_REG_CFG, 0, 1u << 4);      /* jpeg_enc_en */

  /* Drain until EOF is latched AND the FIFO has run dry.  One frame at
   * 20 fps is 50 ms; the outer budget covers several frame times.
   */

    {
      size_t pos = 0;
      uint32_t idle = 0;
      bool eof = false;

      budget = 30000000;
      ret    = -ETIMEDOUT;

      while (budget-- > 0)
        {
          if ((getreg32(JPEG_REG_FIFOSTAT) & JPEG_FIFO_EMPTY) == 0)
            {
              uint32_t word = getreg32(JPEG_REG_FIFO);

              idle = 0;
              if (pos + 4 <= maxlen)
                {
                  memcpy(buf + pos, &word, 4);
                }

              pos += 4;
              continue;
            }

          if (eof)
            {
              /* EOF seen and FIFO dry: allow a short grace for
               * stragglers, then call the frame complete.
               */

              if (++idle > 2000)
                {
                  ret = OK;
                  break;
                }

              continue;
            }

          if ((getreg32(JPEG_REG_STATUS) & (1u << 1)) != 0)
            {
              eof = true;
              continue;
            }

          if ((getreg32(YUV_REG_STATUS) &
               ((1u << 4) | (1u << 6) | (1u << 8))) != 0)
            {
              ret = -EIO;
              break;
            }
        }

      if (pos > maxlen)
        {
          ret = -E2BIG;
        }
    }

  /* Stop the engine either way. */

  modifyreg32(JPEG_REG_CFG, 1u << 4, 0);

  count = getreg32(JPEG_REG_BYTECNT);
  putreg32(getreg32(JPEG_REG_STATUS), JPEG_REG_STATUS);
  putreg32(getreg32(YUV_REG_STATUS), YUV_REG_STATUS);

  if (ret < 0)
    {
      return ret;
    }

  /* The stream is complete JFIF plus a 5-byte CRC tail, and the byte
   * counter is documented unreliable -- the EOI marker is the truth.
   */

  if (buf[0] != 0xff || buf[1] != 0xd8)
    {
      return -EBADMSG;
    }

  scan = count + 64;
  if (scan > maxlen)
    {
      scan = maxlen;
    }

  while (scan >= 2)
    {
      if (buf[scan - 2] == 0xff && buf[scan - 1] == 0xd9)
        {
          return (int)scan;
        }

      scan--;
    }

  return -EBADMSG;
}
