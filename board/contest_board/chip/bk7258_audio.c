/****************************************************************************
 * boards/arm/bk7258/contest_board/chip/bk7258_audio.c
 *
 * On-chip AUD block, DAC playback path: 16 kHz / 16-bit mono PCM out the
 * differential speaker driver, CPU-polled FIFO.  The analog side is a
 * verbatim transplant of the vendor's bring-up values; every ana_reg
 * write goes over an internal SPI sync and must poll its busy bit.
 * The power amplifier is gated by GPIO 50, active high, and is opened
 * only after the DAC is running (and closed before it stops) to keep
 * the pop out of the speaker.
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/arch.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>

#include "arm_internal.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define AUD_BASE              0x47800000ul
#define AUD_REG_DEVICEID      (AUD_BASE + 0x00)   /* "aud" on real silicon  */
#define AUD_REG_CLK_CONTROL   (AUD_BASE + 0x08)   /* bit0 soft_reset (stays 1) */
#define AUD_REG_DAC_CONFIG0   (AUD_BASE + 0x1c)
#define AUD_REG_FIFO_CONFIG   (AUD_BASE + 0x28)
#define AUD_REG_FIFO_STATUS   (AUD_BASE + 0x38)   /* bit9 DACL_FIFO_FULL */
#define AUD_REG_DAC_FPORT     (AUD_BASE + 0x48)   /* write-only sample port */
#define AUD_REG_EXTEND_CFG    (AUD_BASE + 0x60)
#define AUD_REG_AUD_CONFIG    (AUD_BASE + 0xc0)   /* rate, dac_enable */

#define AUD_ID_VALUE          0x00617564u   /* "aud" -- lowercase on silicon */
#define DACL_FIFO_FULL        (1u << 9)

#define SYS_POWER             0x44010040ul        /* bit6 pwd_audp, 0=on */
#define SYS_CLK_DIV1          0x44010020ul        /* bit25 cksel_aud, 0=26M */
#define SYS_CLK_EN            0x44010030ul        /* bit30 aud_cken */
#define SYS_ANA_BUSY          0x440100e8ul        /* bit N: ana_regN busy */
#define SYS_ANA_REG(n)        (0x44010100ul + (n) * 4)

#define GPIO50_CFG            0x440004c8ul        /* PA enable, active high */
#define PA_ON                 0x00000122u
#define PA_OFF                0x00000120u

/****************************************************************************
 * Private Data
 ****************************************************************************/

static bool g_dac_ready;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* Analog register write: full-word replace, then wait for the internal
 * SPI shadow sync to drain before the next one may be issued.
 */

static int aud_ana_write(int n, uint32_t val)
{
  uint32_t budget = 100000;

  putreg32(val, SYS_ANA_REG(n));

  while ((getreg32(SYS_ANA_BUSY) & (1u << n)) != 0)
    {
      if (--budget == 0)
        {
          return -ETIMEDOUT;
        }
    }

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_audio_dac_init
 *
 * Description:
 *   Power the AUDP domain, run the vendor analog bring-up, and configure
 *   the digital DAC for 16 kHz (rate16k) or 8 kHz mono.  The DAC and PA
 *   stay off until bk7258_audio_dac_start().
 *
 ****************************************************************************/

int bk7258_audio_dac_init(bool rate16k)
{
  int ret;

  if (g_dac_ready)
    {
      return OK;
    }

  /* Domain power (0 = on), audio clock = 26 MHz crystal, module gate */

  modifyreg32(SYS_POWER, 1u << 6, 0);
  modifyreg32(SYS_CLK_DIV1, 1u << 25, 0);
  modifyreg32(SYS_CLK_EN, 0, 1u << 30);

  if (getreg32(AUD_REG_DEVICEID) != AUD_ID_VALUE)
    {
      return -ENODEV;
    }

  putreg32(1, AUD_REG_CLK_CONTROL);     /* soft_reset stays set (SDK) */

  /* Analog bring-up, verbatim vendor values; then audio bias on and
   * the composed DAC path words (bias + driver + dcoc + idac + left
   * channel, differential, analog gain 0xA, unmuted).
   */

  ret = aud_ana_write(18, 0x00bf8085);
  ret |= aud_ana_write(19, 0x81800006);
  ret |= aud_ana_write(20, 0xfbc02423);
  ret |= aud_ana_write(21, 0x00500000);
  ret |= aud_ana_write(27, 0x91800006);
  ret |= aud_ana_write(18, 0x00bf808d);           /* enaudbias */
  ret |= aud_ana_write(21, 0x00d40000);           /* enbs + enidacl */
  ret |= aud_ana_write(20, 0xfaa92423);           /* drv+dcoc+dacl, g=0xA */

  if (ret != OK)
    {
      return -ETIMEDOUT;
    }

  /* Digital: HPF bypassed, digital gain 0x20; automatic fractional
   * divider; sample rate field (0=8k, 1=16k); DACL read threshold 8.
   */

  putreg32(0x00830000, AUD_REG_DAC_CONFIG0);
  modifyreg32(AUD_REG_EXTEND_CFG, 1u, 0);
  putreg32(rate16k ? (1u << 6) : 0, AUD_REG_AUD_CONFIG);
  putreg32(8u << 5, AUD_REG_FIFO_CONFIG);

  g_dac_ready = true;
  return OK;
}

/****************************************************************************
 * Name: bk7258_audio_dac_start / _stop
 ****************************************************************************/

int bk7258_audio_dac_start(void)
{
  modifyreg32(AUD_REG_AUD_CONFIG, 0, 1u << 2);    /* dac_enable */
  usleep(30 * 1000);                              /* settle before PA */
  putreg32(PA_ON, GPIO50_CFG);
  return OK;
}

int bk7258_audio_dac_stop(void)
{
  aud_ana_write(20, 0xfaa92423 | (1u << 26));     /* dacmute */
  putreg32(PA_OFF, GPIO50_CFG);
  usleep(30 * 1000);
  modifyreg32(AUD_REG_AUD_CONFIG, 1u << 2, 0);
  aud_ana_write(20, 0xfaa92423);                  /* unmute for next run */
  return OK;
}

/****************************************************************************
 * Name: bk7258_audio_dac_write
 *
 * Description:
 *   Blocking-feed PCM into the DAC FIFO.  Stereo WAV data is already
 *   the port's native layout -- interleaved L,R little-endian is one
 *   (right<<16)|left word per frame -- so write2 streams it verbatim;
 *   mono samples get duplicated into both halves.  Only the left
 *   channel reaches an analog driver on this part.
 *
 ****************************************************************************/

int bk7258_audio_dac_write2(const uint32_t *frames, int nframes)
{
  int i;

  for (i = 0; i < nframes; i++)
    {
      while ((getreg32(AUD_REG_FIFO_STATUS) & DACL_FIFO_FULL) != 0)
        {
        }

      putreg32(frames[i], AUD_REG_DAC_FPORT);
    }

  return nframes;
}

int bk7258_audio_dac_write(const int16_t *pcm, int nsamples)
{
  int i;

  for (i = 0; i < nsamples; i++)
    {
      uint32_t s = (uint16_t)pcm[i];

      while ((getreg32(AUD_REG_FIFO_STATUS) & DACL_FIFO_FULL) != 0)
        {
        }

      putreg32((s << 16) | s, AUD_REG_DAC_FPORT);
    }

  return nsamples;
}
