/****************************************************************************
 * boards/arm/bk7258/contest_board/chip/bk7258_pwm.c
 *
 * Vibration motor on PWM3: unit 0, hardware channel 3 (TIM2 / CCR4),
 * GPIO 9 second-function 1.  The vendor's proven operating point is a
 * 1 kHz carrier at 30% duty, active high; the motor rail is an external
 * 3.3 V LDO gated by GPIO 52 -- shared with the SD card, so this driver
 * only ever *verifies* that rail, it never switches it.
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/arch.h>
#include <stdint.h>
#include <stdio.h>

#include "arm_internal.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define PWM0_BASE             0x458a0000ul

#define PWM_REG_CG_RESET      (PWM0_BASE + 0x08)  /* bit0 soft_reset */
#define PWM_REG_CR1           (PWM0_BASE + 0x10)  /* bit1 = CEN2 (TIM2!) */
#define PWM_REG_CCMR          (PWM0_BASE + 0x28)
#define PWM_REG_PSC           (PWM0_BASE + 0x38)  /* bits[15:8] = PSC2 */
#define PWM_REG_TIM2_ARR      (PWM0_BASE + 0x40)
#define PWM_REG_CCR4          (PWM0_BASE + 0x60)
#define PWM_REG_CCR5          (PWM0_BASE + 0x64)
#define PWM_REG_CCR6          (PWM0_BASE + 0x68)

/* CCMR fields for hardware channel 3 (= "channel 4" in register speak) */

#define CCMR_CH4P_MASK        (3u << 6)           /* polarity: 00 idle low */
#define CCMR_CH4E             (1u << 15)          /* output enable */
#define CCMR_OC2M_MASK        (7u << 24)
#define CCMR_OC2M_TOGGLE      (1u << 24)          /* toggle on CCR match */

#define SYS_CLK_EN            0x44010030ul        /* bit3 pwm0_cken */
#define SYS_CLK_DIV1          0x44010020ul        /* bit18 cksel_pwm0=26M */
#define SYS_PINMUX_G1         0x440100c4ul        /* GPIO8-15, 4 bits each */
#define GPIO9_CFG             0x44000424ul
#define GPIO52_CFG            0x440004d0ul

#define MOTOR_PERIOD          26000               /* 26 MHz / 1 kHz */

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_motor_init
 *
 * Description:
 *   Bring up PWM unit 0 and park TIM2 running with the output disabled.
 *   Returns OK, or -EIO when the motor's LDO rail (GPIO 52, shared with
 *   the SD card) is not already up -- we refuse to switch that rail.
 *
 ****************************************************************************/

int bk7258_motor_init(void)
{
  static bool ready;
  uint32_t regval;
  int i;

  if (ready)
    {
      return OK;
    }

  /* The 3.3 V rail: bit3 output-enable... GPIO cfg semantics on this part
   * use io_mode bits 3:2 = 0b00 for output, so "driving high" shows as
   * bit1 (output value) set with bits 3:2 clear.  If the bootloader left
   * the rail configured some other way we leave it alone and report.
   */

  regval = getreg32(GPIO52_CFG);
  if ((regval & (1u << 1)) == 0)
    {
      printf("motor: LDO rail GPIO52 cfg=%08lx not driven high, "
             "leaving untouched\n", (unsigned long)regval);
    }

  /* Clock: unit 0 gate on, source = 26 MHz crystal */

  modifyreg32(SYS_CLK_EN, 0, 1u << 3);
  modifyreg32(SYS_CLK_DIV1, 0, 1u << 18);

  /* Module soft reset (bit0: 0 = hold, 1 = run) */

  modifyreg32(PWM_REG_CG_RESET, 1u, 0);
  for (i = 0; i < 100; i++)
    {
      getreg32(PWM_REG_CG_RESET);
    }

  modifyreg32(PWM_REG_CG_RESET, 0, 1u);

  /* GPIO 9 -> second function 1 (PWM3): func nibble, then the pad
   * (second-function enable + pull-up, input/output drivers off).
   */

  regval  = getreg32(SYS_PINMUX_G1);
  regval &= ~(0xfu << 4);
  regval |= (1u << 4);
  putreg32(regval, SYS_PINMUX_G1);
  putreg32(0x70, GPIO9_CFG);

  /* TIM2: 1 kHz period, output parked off, counter running.  CCR5/CCR6
   * follow the vendor's fixed pattern (period / zero).
   */

  regval  = getreg32(PWM_REG_CCMR);
  regval &= ~(CCMR_CH4P_MASK | CCMR_OC2M_MASK | CCMR_CH4E);
  regval |= CCMR_OC2M_TOGGLE;
  putreg32(regval, PWM_REG_CCMR);

  putreg32(MOTOR_PERIOD - 1, PWM_REG_TIM2_ARR);
  putreg32(MOTOR_PERIOD, PWM_REG_CCR4);          /* 0%: never toggles */
  putreg32(MOTOR_PERIOD, PWM_REG_CCR5);
  putreg32(0, PWM_REG_CCR6);
  modifyreg32(PWM_REG_PSC, 0xffu << 8, 0);
  modifyreg32(PWM_REG_CR1, 0, 1u << 1);          /* CEN2 */

  ready = true;
  return OK;
}

/****************************************************************************
 * Name: bk7258_motor_on / bk7258_motor_off
 *
 * Description:
 *   Gate the vibration.  duty_pct is clamped to the vendor-proven 30%
 *   ceiling by default callers; the hard clamp here is 60% to keep an
 *   ERM motor off DC-equivalent drive.
 *
 ****************************************************************************/

int bk7258_motor_on(int duty_pct)
{
  if (duty_pct < 1)
    {
      duty_pct = 1;
    }
  else if (duty_pct > 60)
    {
      duty_pct = 60;
    }

  /* High pulse sits at the tail of the period: toggle point = end - duty */

  putreg32(MOTOR_PERIOD - (MOTOR_PERIOD / 100) * duty_pct, PWM_REG_CCR4);
  modifyreg32(PWM_REG_CCMR, 0, CCMR_CH4E);
  return OK;
}

int bk7258_motor_off(void)
{
  modifyreg32(PWM_REG_CCMR, CCMR_CH4E, 0);
  return OK;
}
