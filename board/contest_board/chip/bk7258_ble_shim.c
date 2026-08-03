/****************************************************************************
 * boards/arm/bk7258/contest_board/chip/bk7258_ble_shim.c
 *
 * The closed Beken BLE/PHY libraries link against a handful of direct
 * symbols (everything else is injected through function-pointer tables
 * at runtime).  These are their NuttX implementations: the beken rtos_*
 * critical-section and mutex primitives, a microsecond delay, and two
 * harmless vendor hooks.
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include <nuttx/mutex.h>
#include <nuttx/kmalloc.h>
#include <stdbool.h>
#include <stdint.h>

#include "arm_internal.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/* Beken semantics: disable returns the saved state, enable restores it. */

uint32_t rtos_disable_int(void)
{
  return (uint32_t)up_irq_save();
}

void rtos_enable_int(uint32_t flags)
{
  up_irq_restore((irqstate_t)flags);
}

/* beken_mutex_t is an opaque void *; init allocates, lock/unlock operate.
 * Return convention: 0 = ok (bk_err_t).
 */

int rtos_init_mutex(void **mutex)
{
  mutex_t *m = kmm_malloc(sizeof(mutex_t));

  if (m == NULL)
    {
      return -1;
    }

  nxmutex_init(m);
  *mutex = m;
  return 0;
}

int rtos_lock_mutex(void **mutex)
{
  return nxmutex_lock((mutex_t *)*mutex);
}

int rtos_unlock_mutex(void **mutex)
{
  return nxmutex_unlock((mutex_t *)*mutex);
}

int rtos_deinit_mutex(void **mutex)
{
  mutex_t *m;

  if (mutex == NULL || *mutex == NULL)
    {
      return 0;
    }

  m = (mutex_t *)*mutex;
  nxmutex_destroy(m);
  kmm_free(m);
  *mutex = NULL;
  return 0;
}

void delay_us(uint32_t us)
{
  up_udelay(us);
}

void bk_delay_us(uint32_t us)
{
  up_udelay(us);
}

bool ate_is_enabled(void)
{
  return false;
}

void shell_log_flush(void)
{
}

void bk_system_dump(const char *func, int line)
{
  (void)func;
  (void)line;
}

/* Vendor hooks the controller calls directly; nothing to do on NuttX.
 * bk_uart_recover_rx_isr re-arms the DUT UART after HCI passthrough;
 * bk_set_printf_sync toggles synchronous logging.
 */

void bk_uart_recover_rx_isr(void)
{
}

void bk_set_printf_sync(uint8_t enable)
{
  (void)enable;
}

/****************************************************************************
 * Name: sys_drv_module_power_state_get
 *
 * Description:
 *   Report whether a power domain is switched off.  The closed PHY's
 *   rfconfig switch calls this directly; the module number is the bit
 *   index in the sleep/wakeup word, and the bit reads 1 when the domain
 *   is powered down, matching the vendor's HAL.
 *
 ****************************************************************************/

int sys_drv_module_power_state_get(uint32_t module)
{
  return (int)((getreg32(0x44010040ul) >> module) & 1u);
}
