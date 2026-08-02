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
#include <string.h>

/* Closed-library entries (bk_idk prebuilt archives, ABI-matched:
 * armv8-m.main / fpv5-sp-d16 / hard float).
 */

extern int  bt_os_adapter_init(void *funcs);
extern int  bluetooth_controller_init(void);
extern int  bk_ble_reg_hci_recv_callback(void *evt_cb, void *acl_cb);
extern int  bk_ble_hci_cmd_to_controller(uint8_t *buf, uint16_t len);
extern int  bk_ble_create_advertising(void);
extern int  bt_feature_adapter_init(void *arg);
extern int  phy_adapter_init(void *funcs, void *vars);
extern int  rf_adapter_init(void *funcs);
extern void calibration_init(void);
extern void rf_module_vote_ctrl(uint32_t cmd, uint32_t bit);

/* The OSI table (bk7258_bt_osi.c).  Its address is taken, never called:
 * without a reference the whole object -- and every closed PHY symbol its
 * table entries point at -- is dropped by --gc-sections, and a build that
 * discards the code proves nothing about whether it links.
 */

extern int bk7258_bt_osi_init(void);

/* Feature flags the controller reads once at init.  Layout must match
 * bt_feature_config.h byte for byte; all zero is the stock BLE profile,
 * and _support_lpo_rosc stays clear to agree with the OSI table, which
 * reports the divided-26M sleep clock rather than the ROSC.
 */

struct bt_feature_s
{
  uint8_t is_gatt_discovery_auto;
  uint8_t ignore_smp_key_distr_all_zero;
  uint8_t strict_smp_key_distr_check_except_all_zero;
  uint8_t ignore_smp_already_pair;
  uint8_t send_peripheral_feature_req_auto;
  uint8_t stop_smp_when_pair_err;
  uint8_t enable_smp_sec_req_evt;
  uint8_t support_lpo_rosc;
};

static struct bt_feature_s g_bt_feature;

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_bt_feature_init
 *
 * Description:
 *   Hand the controller its feature-flag block.  Stock BLE profile.
 *
 ****************************************************************************/

int bk7258_bt_feature_init(void)
{
  memset(&g_bt_feature, 0, sizeof(g_bt_feature));
  return bt_feature_adapter_init(&g_bt_feature);
}

/****************************************************************************
 * Name: bk7258_bt_controller_init
 *
 * Description:
 *   Start the closed link-layer controller.  It powers the BTSP domain,
 *   gates the BTDM and XVR clocks, hooks its interrupts and votes for
 *   the radio -- all through the OSI table -- so everything it needs
 *   from us is already in place, except that the RF vote lands in the
 *   closed PHY archive, whose own adapter table is not filled yet.
 *
 ****************************************************************************/

int bk7258_bt_controller_init(void)
{
  return bluetooth_controller_init();
}

uintptr_t bk7258_ble_link_probe(void)
{
  return (uintptr_t)bk7258_bt_osi_init +
         (uintptr_t)bk7258_bt_feature_init +
         (uintptr_t)bk7258_bt_controller_init +
         (uintptr_t)bt_os_adapter_init +
         (uintptr_t)bluetooth_controller_init +
         (uintptr_t)bk_ble_reg_hci_recv_callback +
         (uintptr_t)bk_ble_hci_cmd_to_controller +
         (uintptr_t)bk_ble_create_advertising +
         (uintptr_t)phy_adapter_init +
         (uintptr_t)rf_adapter_init +
         (uintptr_t)calibration_init +
         (uintptr_t)rf_module_vote_ctrl;
}
