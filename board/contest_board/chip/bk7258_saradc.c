/****************************************************************************
 * boards/arm/bk7258/contest_board/chip/bk7258_saradc.c
 *
 * BK7258 general SARADC (GADC), bare registers, polled.
 *
 * The only consumer in this image is RF calibration inside the closed PHY
 * archive: it samples the transmit power detector (channel 8), the on-die
 * temperature sensor (channel 7) and the supply (channel 0) to solve for
 * its trim values.  Calibration runs early and can reach the converter
 * with interrupts masked, so nothing here sleeps, allocates, takes a lock
 * or arms an interrupt -- the SARADC line is deliberately left masked in
 * the system block's interrupt matrix.  A conversion train is started, the
 * FIFO status bit is spun on, and words are popped out of the data
 * register.
 *
 * Every spin has a budget.  A converter that never produces a sample has
 * to look like a failed measurement so the caller can fall back, not like
 * a hung board -- this port has already paid for one unbounded wait.
 *
 * The register sequence is transcribed from the vendor driver
 * (bk_idk middleware/driver/saradc/adc_driver.c, soc/common/hal/adc_hal.c
 * and soc/bk7258/hal/adc_ll.h), with two deliberate departures:
 *
 *   - The interrupt-plus-FIFO-threshold acquisition is replaced by a plain
 *     poll.  The threshold field is still programmed the way the vendor
 *     programs it, because it only selects when an interrupt we never
 *     enable would fire.
 *
 *   - The vendor touches the analog shadow register twice, once in
 *     adc_ll_init() and once in sys_hal_set_saradc_config(), with the
 *     second write a superset of the first.  Each of those writes costs an
 *     internal SPI round trip, so they are merged into one.
 *
 * The converter sits in the same always-on peripheral group as TIMER1,
 * UART1/2, I2C and PWM, all of which this board already drives without
 * voting for domain power, so only the module clock gate is touched here.
 * Powering the group down is not this driver's business: the neighbours
 * would go with it.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <nuttx/arch.h>

#include "arm_internal.h"
#include "bk7258_memorymap.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Converter block.  Word indices are the vendor's (soc/adc_struct.h) and
 * are kept in the comments so the byte offsets can be checked against it.
 */

#define SARADC_BASE             0x45890000ul

#define SARADC_GLOBAL_CTRL      (SARADC_BASE + 0x08)  /* word 0x02 */
#define SARADC_CTRL             (SARADC_BASE + 0x10)  /* word 0x04 */
#define SARADC_STEADY_CTRL      (SARADC_BASE + 0x18)  /* word 0x06 */
#define SARADC_SAT_CTRL         (SARADC_BASE + 0x1c)  /* word 0x07 */
#define SARADC_DATA             (SARADC_BASE + 0x20)  /* word 0x08 */

#define SARADC_GLOBAL_SOFT_RST  (1u << 0)

#define SARADC_CTRL_MODE_SHIFT  0
#define SARADC_CTRL_MODE_MASK   (0x3u << 0)
#define SARADC_CTRL_EN          (1u << 2)
#define SARADC_CTRL_CHAN_SHIFT  3
#define SARADC_CTRL_CHAN_MASK   (0xfu << 3)
#define SARADC_CTRL_INT_CLR     (1u << 8)
#define SARADC_CTRL_DIV_SHIFT   9
#define SARADC_CTRL_DIV_MASK    (0x3fu << 9)
#define SARADC_CTRL_RATE_SHIFT  16
#define SARADC_CTRL_RATE_MASK   (0x3fu << 16)
#define SARADC_CTRL_FILT_SHIFT  22
#define SARADC_CTRL_FILT_MASK   (0x7fu << 22)
#define SARADC_CTRL_BUSY        (1u << 29)
#define SARADC_CTRL_FIFO_EMPTY  (1u << 30)

#define SARADC_STEADY_LVL_SHIFT 0
#define SARADC_STEADY_TM_SHIFT  5
#define SARADC_STEADY_BYPASS    (1u << 10)

#define SARADC_SAT_SEL_SHIFT    0
#define SARADC_SAT_ENABLE       (1u << 2)

/* System block: module clock gate, source select and the analog shadow
 * registers.  Word indices are the SDK's (bk7258 hal/sys_ll.h): the device
 * clock enables are word 0x0c, the mode-1 source selects word 0x08, the
 * analog shadow busy flags word 0x3a and the shadows themselves word 0x40
 * upwards, one bit of the busy word per shadow.
 */

#define SARADC_SYS_CLK_DIV1     (BK7258_SYS_BASE + (0x08 << 2))
#define SARADC_SYS_CLK_EN       (BK7258_SYS_BASE + (0x0c << 2))
#define SARADC_SYS_ANA_BUSY     (BK7258_SYS_BASE + (0x3a << 2))
#define SARADC_SYS_ANA(n)       (BK7258_SYS_BASE + ((0x40 + (n)) << 2))

#define SARADC_CKEN_SADC        (1u << 5)   /* word 0x0c bit 5  */
#define SARADC_CKSEL_SADC       (1u << 17)  /* word 0x08 bit 17 */

/* Analog shadow 2 carries the converter's front end.  Only the six fields
 * the vendor programs are touched; the crystal trim in the low byte and
 * the scale/calibration enables above them belong to other owners and are
 * preserved.
 */

#define SARADC_ANA2             2
#define SARADC_ANA2_CMP_SHIFT   9    /* gadc_cmp_ictrl    [10: 9] */
#define SARADC_ANA2_INBUF_SHIFT 11   /* gadc_inbuf_ictrl  [12:11] */
#define SARADC_ANA2_REFBUF_SHFT 13   /* gadc_refbuf_ictrl [14:13] */
#define SARADC_ANA2_NOBUF       (1u << 15)
#define SARADC_ANA2_CAPCAL_SHFT 19   /* gadc_capcal       [24:19] */
#define SARADC_ANA2_SPNT_SHIFT  25   /* sp_nt_ctrl        [31:25] */

#define SARADC_ANA2_OURS        ((0x7fu << SARADC_ANA2_CMP_SHIFT)     | \
                                 (0x3fu << SARADC_ANA2_CAPCAL_SHFT)   | \
                                 (0x7fu << SARADC_ANA2_SPNT_SHIFT))

#define SARADC_ANA2_ON          ((0x2u << SARADC_ANA2_CMP_SHIFT)      | \
                                 (0x2u << SARADC_ANA2_INBUF_SHIFT)    | \
                                 (0x2u << SARADC_ANA2_REFBUF_SHFT)    | \
                                 (0x0u << SARADC_ANA2_CAPCAL_SHFT)    | \
                                 (0x3u << SARADC_ANA2_SPNT_SHIFT)     | \
                                 SARADC_ANA2_NOBUF)

/* Analog shadow 5 bit 4 gates the on-die temperature sensor.  The same
 * word holds the converter input divider, which the PHY drives through its
 * own accessor, so this is a read-modify-write of one bit.
 */

#define SARADC_ANA5             5
#define SARADC_ANA5_EN_TEMP     (1u << 4)

/* The source is the 26 MHz crystal; the divider yields
 * adc_clk = 26 MHz / (2 * (div + 1)) and is six bits wide.
 */

#define SARADC_SRC_CLK          26000000u
#define SARADC_DIV_MAX          0x3fu

#define SARADC_CHAN_MAX         15
#define SARADC_MODE_CONTINUOUS  3
#define SARADC_MODE_MAX         3
#define SARADC_SAT_MAX          4
#define SARADC_STEADY_MAX       7
#define SARADC_FIFO_LEVEL_MAX   0x1fu

/* Spin budgets.
 *
 * The analog shadow handshake and the busy flag settle in well under a
 * microsecond; the large loop counts only exist so a dead bus cannot
 * become a dead board.
 *
 * The acquisition budget is clamped hard.  At the slowest configuration
 * the vendor uses on this part (203 kHz converter clock, 16 clocks per
 * sample) a 32-sample batch takes about 2.5 ms, so 200 ms is two orders of
 * margin -- while the vendor's own 1000 ms is a semaphore timeout sized
 * for a sleeping task, which is not what a caller holding interrupts
 * masked can afford to spin for.  The budget counts one-microsecond waits
 * and the loop around them is not free, so the wall clock at expiry runs
 * somewhat past the nominal figure; what matters is that it is bounded.
 */

#define SARADC_ANA_SPINS        100000u
#define SARADC_BUSY_SPINS       10000u
#define SARADC_READ_MAX_US      200000u
#define SARADC_READ_DFLT_US     10000u
#define SARADC_FLUSH_SPINS      256u

/* One settle after the front end is switched on.  The converter has its
 * own steady counter for this -- (steady + 1) * 8 converter clocks before
 * the first sample is declared valid -- but that counter starts with the
 * conversion, not with the bias ramp, and the cost of covering the ramp
 * here is a hundred microseconds once per calibration.
 */

#define SARADC_SETTLE_US        100

/****************************************************************************
 * Private Data
 ****************************************************************************/

static bool    g_saradc_on;
static bool    g_saradc_running;
static uint8_t g_saradc_flag;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: saradc_ana_field
 *
 * Description:
 *   Analog shadow register access.  A write to a shadow is pushed to the
 *   analog island over an internal SPI link and bit n of the busy word
 *   stays set until that transfer drains; issuing the next write before it
 *   clears loses it.  Reads come straight back from the shadow.  Same
 *   protocol as aud_ana_write() in bk7258_audio.c and phy_ana_write() in
 *   bk7258_phy_osi.c.
 *
 ****************************************************************************/

static void saradc_ana_field(int n, uint32_t clrbits, uint32_t setbits)
{
  uint32_t budget = SARADC_ANA_SPINS;
  uint32_t reg;

  reg = getreg32(SARADC_SYS_ANA(n));
  reg &= ~clrbits;
  reg |= setbits;
  putreg32(reg, SARADC_SYS_ANA(n));

  while ((getreg32(SARADC_SYS_ANA_BUSY) & (1u << n)) != 0)
    {
      if (--budget == 0)
        {
          return;
        }
    }
}

/****************************************************************************
 * Name: saradc_flush
 *
 * Description:
 *   Drop whatever the FIFO accumulated and clear the pending interrupt
 *   flag, so the next batch a caller reads is entirely post-flush.  The
 *   drain is counted rather than run to empty: in continuous mode the
 *   converter keeps producing, and at a high sample rate a "drain until
 *   empty" loop can be outrun by the hardware.
 *
 ****************************************************************************/

static void saradc_flush(void)
{
  uint32_t budget = SARADC_FLUSH_SPINS;

  modifyreg32(SARADC_CTRL, 0, SARADC_CTRL_INT_CLR);

  while ((getreg32(SARADC_CTRL) & SARADC_CTRL_FIFO_EMPTY) == 0)
    {
      getreg32(SARADC_DATA);

      if (--budget == 0)
        {
          return;
        }
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_saradc_div
 *
 * Description:
 *   Convert a requested converter clock in Hz to the six-bit divider,
 *   using the vendor's own arithmetic (adc_hal_set_clk): the source is
 *   halved before the divider, and the field counts from zero.  A request
 *   the divider cannot reach is clamped rather than refused, because the
 *   sample rate is not what a caller is checking for.
 *
 * Input Parameters:
 *   adc_clk - Requested converter clock, Hz.  Zero selects the slowest.
 *
 * Returned Value:
 *   The divider field value, 0..63.
 *
 ****************************************************************************/

uint32_t bk7258_saradc_div(uint32_t adc_clk)
{
  uint32_t div;

  if (adc_clk == 0)
    {
      return SARADC_DIV_MAX;
    }

  div = SARADC_SRC_CLK / 2 / adc_clk;
  if (div > 0)
    {
      div--;
    }

  if (div > SARADC_DIV_MAX)
    {
      div = SARADC_DIV_MAX;
    }

  return div;
}

/****************************************************************************
 * Name: bk7258_saradc_pwrup
 *
 * Description:
 *   Open the module clock gate and bring up the analog front end with the
 *   vendor's bias settings (sys_hal_sadc_pwr_up plus
 *   sys_hal_set_saradc_config, merged into one shadow write).  Idempotent,
 *   because the PHY calls the matching hook once per calibration pass and
 *   bk7258_saradc_start() calls it again for itself.
 *
 * Returned Value:
 *   OK.
 *
 ****************************************************************************/

int bk7258_saradc_pwrup(void)
{
  if (g_saradc_on)
    {
      return OK;
    }

  modifyreg32(SARADC_SYS_CLK_EN, 0, SARADC_CKEN_SADC);
  saradc_ana_field(SARADC_ANA2, SARADC_ANA2_OURS, SARADC_ANA2_ON);

  g_saradc_on = true;
  return OK;
}

/****************************************************************************
 * Name: bk7258_saradc_start
 *
 * Description:
 *   Configure the converter and start it.  Follows the vendor's order:
 *   clock gate and front end, then source select, then a block reset, then
 *   the three configuration words, and the enable bit last.
 *
 *   Calibration result correction is bypassed, which is what all three of
 *   the vendor's own callers do (bk_cal_saradc_start, and the temperature
 *   and supply detectors): the correction wants a converter calibration
 *   pass this driver does not run.
 *
 * Input Parameters:
 *   channel  - Input channel, 0..15.  Only the analog channels (0 supply,
 *              7 temperature, 8 transmit power detector, 9, 11) work
 *              without pad muxing, and those are the only ones the RF
 *              library asks for; a digital channel would additionally need
 *              its pin switched to the converter, which is not done here.
 *   mode     - 1 single step, 2 software controlled, 3 continuous.  Mode 0
 *              powers the converter down and is rejected.
 *   div      - Clock divider field, see bk7258_saradc_div().
 *   saturate - Saturation mode in the vendor's numbering: 0 off, 1..4 for
 *              its modes 0..3.
 *   steady   - Settle time code, 0..7; the converter waits
 *              (steady + 1) * 8 converter clocks before the first sample.
 *   rate     - Sample period trim, continuous mode only; the period is
 *              (16 + rate) converter clocks.
 *   filter   - Accumulation count, continuous mode only.
 *
 * Returned Value:
 *   OK, -EINVAL on an out-of-range argument, -EBUSY if a conversion is
 *   already in flight or the converter never went idle.
 *
 ****************************************************************************/

int bk7258_saradc_start(uint8_t channel, uint8_t mode, uint32_t div,
                        uint8_t saturate, uint8_t steady, uint8_t rate,
                        uint8_t filter)
{
  uint32_t budget = SARADC_BUSY_SPINS;
  uint32_t regval;

  if (channel > SARADC_CHAN_MAX || mode == 0 || mode > SARADC_MODE_MAX ||
      div > SARADC_DIV_MAX || saturate > SARADC_SAT_MAX ||
      steady > SARADC_STEADY_MAX)
    {
      return -EINVAL;
    }

  if (g_saradc_running)
    {
      return -EBUSY;
    }

  bk7258_saradc_pwrup();

  /* Source select: the 26 MHz crystal rather than the audio PLL. */

  modifyreg32(SARADC_SYS_CLK_DIV1, SARADC_CKSEL_SADC, 0);

  /* Block reset.  The bit is not self-clearing and the vendor leaves it
   * set, same as the audio block on this part.
   */

  modifyreg32(SARADC_GLOBAL_CTRL, 0, SARADC_GLOBAL_SOFT_RST);
  putreg32(0, SARADC_CTRL);

  if (saturate == 0)
    {
      regval = 0;
    }
  else
    {
      regval = (((uint32_t)saturate - 1) << SARADC_SAT_SEL_SHIFT) |
               SARADC_SAT_ENABLE;
    }

  putreg32(regval, SARADC_SAT_CTRL);

  /* The FIFO threshold only picks when an interrupt would fire and no
   * interrupt is armed here, but it is left at the vendor's value so a
   * register dump matches theirs.
   */

  regval = (SARADC_FIFO_LEVEL_MAX << SARADC_STEADY_LVL_SHIFT) |
           ((uint32_t)steady << SARADC_STEADY_TM_SHIFT) |
           SARADC_STEADY_BYPASS;
  putreg32(regval, SARADC_STEADY_CTRL);

  /* Bit 7 stays clear for the vendor's four-cycle sample wait and bit 15
   * for the crystal rather than the internal 32 MHz oscillator.
   */

  regval = (((uint32_t)mode << SARADC_CTRL_MODE_SHIFT) &
            SARADC_CTRL_MODE_MASK) |
           (((uint32_t)channel << SARADC_CTRL_CHAN_SHIFT) &
            SARADC_CTRL_CHAN_MASK) |
           ((div << SARADC_CTRL_DIV_SHIFT) & SARADC_CTRL_DIV_MASK);

  if (mode == SARADC_MODE_CONTINUOUS)
    {
      regval |= ((uint32_t)rate << SARADC_CTRL_RATE_SHIFT) &
                SARADC_CTRL_RATE_MASK;
      regval |= ((uint32_t)filter << SARADC_CTRL_FILT_SHIFT) &
                SARADC_CTRL_FILT_MASK;
    }

  putreg32(regval, SARADC_CTRL);

  while ((getreg32(SARADC_CTRL) & SARADC_CTRL_BUSY) != 0)
    {
      if (--budget == 0)
        {
          return -EBUSY;
        }
    }

  modifyreg32(SARADC_CTRL, 0, SARADC_CTRL_EN);
  up_udelay(SARADC_SETTLE_US);

  g_saradc_running = true;
  return OK;
}

/****************************************************************************
 * Name: bk7258_saradc_stop
 *
 * Description:
 *   Stop conversions, empty the FIFO and drop the front end and the module
 *   clock.  Safe to call on a converter that was never started, which is
 *   what the RF library does on its own error paths.
 *
 * Returned Value:
 *   OK.
 *
 ****************************************************************************/

int bk7258_saradc_stop(void)
{
  if (g_saradc_running)
    {
      modifyreg32(SARADC_CTRL, SARADC_CTRL_EN, 0);
      saradc_flush();
      g_saradc_running = false;
    }

  if (g_saradc_on)
    {
      saradc_ana_field(SARADC_ANA2, SARADC_ANA2_NOBUF, 0);
      modifyreg32(SARADC_SYS_CLK_EN, SARADC_CKEN_SADC, 0);
      g_saradc_on = false;
    }

  return OK;
}

/****************************************************************************
 * Name: bk7258_saradc_read
 *
 * Description:
 *   Collect size samples into buf by polling the FIFO.  The FIFO is
 *   flushed first, so every sample returned was converted after the call
 *   started -- the vendor's blocking read does the same before it arms its
 *   interrupt.
 *
 * Input Parameters:
 *   buf        - Destination, size 16-bit samples.
 *   size       - Sample count.
 *   timeout_ms - Caller's patience.  Zero selects a default; anything
 *                beyond the clamp described above is reduced to it.
 *
 * Returned Value:
 *   OK, -EINVAL on a bad argument, -EPERM if the converter is not
 *   running, -ETIMEDOUT if the batch did not complete in time.  On
 *   timeout the samples already collected are left in buf.
 *
 ****************************************************************************/

int bk7258_saradc_read(uint16_t *buf, uint32_t size, uint32_t timeout_ms)
{
  uint32_t budget;
  uint32_t got = 0;

  if (buf == NULL || size == 0)
    {
      return -EINVAL;
    }

  if (!g_saradc_running)
    {
      return -EPERM;
    }

  if (timeout_ms == 0)
    {
      budget = SARADC_READ_DFLT_US;
    }
  else if (timeout_ms >= SARADC_READ_MAX_US / 1000u)
    {
      budget = SARADC_READ_MAX_US;
    }
  else
    {
      budget = timeout_ms * 1000u;
    }

  saradc_flush();

  while (got < size)
    {
      if ((getreg32(SARADC_CTRL) & SARADC_CTRL_FIFO_EMPTY) != 0)
        {
          if (budget == 0)
            {
              return -ETIMEDOUT;
            }

          budget--;
          up_udelay(1);
          continue;
        }

      buf[got++] = (uint16_t)(getreg32(SARADC_DATA) & 0xffff);
    }

  return OK;
}

/****************************************************************************
 * Name: bk7258_saradc_tempsensor
 *
 * Description:
 *   Gate the on-die temperature sensor that feeds channel 7.  The vendor
 *   brackets every temperature acquisition with this rather than leaving
 *   the sensor biased, and its own comment says it is not sure why, so
 *   this port follows rather than improves.
 *
 * Input Parameters:
 *   enable - true to bias the sensor, false to drop it.
 *
 ****************************************************************************/

void bk7258_saradc_tempsensor(bool enable)
{
  if (enable)
    {
      saradc_ana_field(SARADC_ANA5, 0, SARADC_ANA5_EN_TEMP);
    }
  else
    {
      saradc_ana_field(SARADC_ANA5, SARADC_ANA5_EN_TEMP, 0);
    }
}

/****************************************************************************
 * Name: bk7258_saradc_set_flag / bk7258_saradc_get_flag
 *
 * Description:
 *   The vendor's g_saradc_flag.  It picks between two code-to-volt
 *   formulas, but only on the BK7256 (RISC-V) part -- on this one the
 *   conversion is unconditional and the flag is inert.  It is stored
 *   rather than dropped so the value the RF library set stays readable.
 *
 ****************************************************************************/

void bk7258_saradc_set_flag(uint8_t flag)
{
  g_saradc_flag = flag;
}

uint8_t bk7258_saradc_get_flag(void)
{
  return g_saradc_flag;
}
