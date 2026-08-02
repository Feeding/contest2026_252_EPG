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
#define AUD_REG_ADC_CONFIG0   (AUD_BASE + 0x10)
#define AUD_REG_DAC_CONFIG0   (AUD_BASE + 0x1c)
#define AUD_REG_FIFO_CONFIG   (AUD_BASE + 0x28)
#define AUD_REG_FIFO_STATUS   (AUD_BASE + 0x38)   /* bit9 DACL_FIFO_FULL */
#define AUD_REG_DAC_FPORT     (AUD_BASE + 0x48)   /* write-only sample port */
#define AUD_REG_EXTEND_CFG    (AUD_BASE + 0x60)
#define AUD_REG_AUD_CONFIG    (AUD_BASE + 0xc0)   /* rate, dac_enable */

#define AUD_REG_ADC_FPORT     (AUD_BASE + 0x44)   /* read pops one L/R pair */

#define AUD_ID_VALUE          0x00617564u   /* "aud" -- lowercase on silicon */
#define DACL_FIFO_FULL        (1u << 9)
#define ADC_FIFO_EMPTY        (1u << 14)

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

static bool g_ana_ready;
static bool g_dac_ready;
static bool g_adc_ready;

/* ana_reg18 is shared bias territory (bit3 audio, bit4 adc, bit5 mic);
 * full-word writes only, so the composed value lives here.
 */

static uint32_t g_ana18 = 0x00bf8085;

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

static int aud_common_init(void)
{
  int ret;

  if (g_ana_ready)
    {
      return OK;
    }

  /* Domain power (0 = on), module clock gate */

  modifyreg32(SYS_POWER, 1u << 6, 0);
  modifyreg32(SYS_CLK_EN, 0, 1u << 30);

  if (getreg32(AUD_REG_DEVICEID) != AUD_ID_VALUE)
    {
      return -ENODEV;
    }

  putreg32(1, AUD_REG_CLK_CONTROL);     /* soft_reset stays set (SDK) */

  /* Analog baseline, verbatim vendor values */

  ret = aud_ana_write(18, g_ana18);
  ret |= aud_ana_write(19, 0x81800006);
  ret |= aud_ana_write(20, 0xfbc02423);
  ret |= aud_ana_write(21, 0x00500000);
  ret |= aud_ana_write(27, 0x91800006);

  /* APLL at 24.576 MHz: the ADC's modulator only runs from it (the
   * family default clk_src is APLL; the crystal path fed the DAC but
   * left the ADC FIFO forever empty).  Power the PLL, load the 48k-
   * family calibration, pulse spi_trigger, then move the audio mux.
   */

  ret |= aud_ana_write(5, getreg32(SYS_ANA_REG(5)) & ~(1u << 13));
  ret |= aud_ana_write(26, 0x8973ca6f);
  ret |= aud_ana_write(25, 0xc2a0ae86);
  ret |= aud_ana_write(25, 0xc2a0ae86 | (1u << 18));
  up_mdelay(2);
  ret |= aud_ana_write(25, 0xc2a0ae86);

  if (ret != OK)
    {
      return -ETIMEDOUT;
    }

  modifyreg32(AUD_REG_AUD_CONFIG, 0, 1u << 8);    /* apll_sel */
  modifyreg32(SYS_CLK_DIV1, 0, 1u << 25);         /* cksel_aud = APLL */
  modifyreg32(0x44010024ul, 0, 1u << 13);         /* dmic_clk_div (AVDK) */

  g_ana_ready = true;
  return OK;
}

int bk7258_audio_dac_init(bool rate16k)
{
  int ret;

  if (g_dac_ready)
    {
      return OK;
    }

  ret = aud_common_init();
  if (ret != OK)
    {
      return ret;
    }

  /* Audio bias plus the composed DAC path words (bias + driver + dcoc
   * + idac + left channel, differential, analog gain 0xA, unmuted).
   */

  g_ana18 |= 1u << 3;                             /* enaudbias */
  ret = aud_ana_write(18, g_ana18);
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
  modifyreg32(AUD_REG_AUD_CONFIG, 3u << 6, rate16k ? (1u << 6) : 0);
  modifyreg32(AUD_REG_FIFO_CONFIG, 0x3ffu, 8u << 5);

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

/****************************************************************************
 * Name: bk7258_audio_adc_init / _start / _stop / _read
 *
 * Description:
 *   Microphone capture: MIC1 (left) through the on-chip ADC, 16-bit at
 *   8/16 kHz, CPU-polled.  MIC2 carries the board's hardware echo
 *   reference and is simply discarded here.
 *
 ****************************************************************************/

int bk7258_audio_adc_init(bool rate16k)
{
  int ret;

  if (g_adc_ready)
    {
      return OK;
    }

  ret = aud_common_init();
  if (ret != OK)
    {
      return ret;
    }

  /* MIC1 on, all three biases (audio + adc + mic), then the vendor's
   * mic reset pulse.
   */

  ret = aud_ana_write(19, 0x91840006);            /* MICEN, PGA 8 (genie audio_para) */
  g_ana18 |= (1u << 3) | (1u << 4) | (1u << 5);
  ret |= aud_ana_write(18, g_ana18);
  ret |= aud_ana_write(19, 0x91840006 | (1u << 29));
  up_mdelay(1);
  ret |= aud_ana_write(19, 0x91840006);

  if (ret != OK)
    {
      return -ETIMEDOUT;
    }

  /* Digital: gain 0x2d (0 dB), both HPFs bypassed, analog mic source;
   * automatic fractional divider; sample-rate field shares AUD_CONFIG
   * with the DAC, so RMW.
   */

  putreg32((0x33u << 18) | (3u << 16), AUD_REG_ADC_CONFIG0);
  modifyreg32(AUD_REG_EXTEND_CFG, 1u << 1, 0);
  modifyreg32(AUD_REG_AUD_CONFIG, 0x3u, rate16k ? 1u : 0);
  modifyreg32(AUD_REG_FIFO_CONFIG, 0x1fu << 15, 8u << 15);

  g_adc_ready = true;
  return OK;
}

int bk7258_audio_adc_start(void)
{
  modifyreg32(AUD_REG_AUD_CONFIG, 0, (1u << 3) | (1u << 5));
  return OK;
}

int bk7258_audio_adc_stop(void)
{
  modifyreg32(AUD_REG_AUD_CONFIG, (1u << 3) | (1u << 5), 0);
  return OK;
}

/* Hardware mic-to-speaker passthrough (the silicon's own loop bit) */

int bk7258_audio_loopback(bool on)
{
  modifyreg32(AUD_REG_FIFO_CONFIG, 1u << 25, on ? (1u << 25) : 0);
  return OK;
}

/* Diagnostic: the analog regs as the hardware actually holds them */

void bk7258_audio_ana_dump(void)
{
  printf("ana18 %08lx 19 %08lx 20 %08lx 21 %08lx\n",
         (unsigned long)getreg32(SYS_ANA_REG(18)),
         (unsigned long)getreg32(SYS_ANA_REG(19)),
         (unsigned long)getreg32(SYS_ANA_REG(20)),
         (unsigned long)getreg32(SYS_ANA_REG(21)));
}

/* Raw L/R pair capture: low half MIC1, high half MIC2 */

int bk7258_audio_adc_read2(uint32_t *pairs, int nsamples)
{
  int i;

  for (i = 0; i < nsamples; i++)
    {
      uint32_t budget = 5000000;

      while ((getreg32(AUD_REG_FIFO_STATUS) & ADC_FIFO_EMPTY) != 0)
        {
          if (--budget == 0)
            {
              return i;
            }
        }

      pairs[i] = getreg32(AUD_REG_ADC_FPORT);
    }

  return nsamples;
}

int bk7258_audio_adc_read(int16_t *pcm, int nsamples)
{
  int i;

  for (i = 0; i < nsamples; i++)
    {
      uint32_t budget = 5000000;      /* never hang the board again */

      while ((getreg32(AUD_REG_FIFO_STATUS) & ADC_FIFO_EMPTY) != 0)
        {
          if (--budget == 0)
            {
              printf("adc: starved at %d/%d, status %08lx cfg %08lx\n",
                     i, nsamples,
                     (unsigned long)getreg32(AUD_REG_FIFO_STATUS),
                     (unsigned long)getreg32(AUD_REG_AUD_CONFIG));
              return i;
            }
        }

      pcm[i] = (int16_t)(getreg32(AUD_REG_ADC_FPORT) & 0xffff);
    }

  return nsamples;
}
