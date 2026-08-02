/****************************************************************************
 * boards/arm/bk7258/contest_board/chip/bk7258_ble.c
 *
 * BLE bring-up staging.  Present stage: a link probe that references the
 * closed-library entry points so the linker extracts the full dependency
 * tree and enumerates every symbol the NuttX side still owes -- the
 * concrete work inventory for the OSI/PHY/PM adaptation.
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>

/* Closed-library entries (bk_idk prebuilt archives, ABI-matched:
 * armv8-m.main / fpv5-sp-d16 / hard float).
 */

extern int  bt_os_adapter_init(void *funcs);
extern void bluetooth_controller_init(void);
extern int  bk_ble_reg_hci_recv_callback(void *evt_cb, void *acl_cb);
extern int  bk_ble_hci_cmd_to_controller(uint8_t *buf, uint16_t len);
extern int  bk_ble_create_advertising(void);
extern int  phy_adapter_init(void *funcs, void *vars);
extern int  rf_adapter_init(void *funcs);
extern void calibration_init(void);
extern void rf_module_vote_ctrl(uint32_t cmd, uint32_t bit);

/****************************************************************************
 * Public Functions
 ****************************************************************************/

uintptr_t bk7258_ble_link_probe(void)
{
  return (uintptr_t)bt_os_adapter_init +
         (uintptr_t)bluetooth_controller_init +
         (uintptr_t)bk_ble_reg_hci_recv_callback +
         (uintptr_t)bk_ble_hci_cmd_to_controller +
         (uintptr_t)bk_ble_create_advertising +
         (uintptr_t)phy_adapter_init +
         (uintptr_t)rf_adapter_init +
         (uintptr_t)calibration_init +
         (uintptr_t)rf_module_vote_ctrl;
}
