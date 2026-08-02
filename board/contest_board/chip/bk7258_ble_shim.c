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
#include <stdint.h>

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

void delay_us(uint32_t us)
{
  up_udelay(us);
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
