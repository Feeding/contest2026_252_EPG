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
#include <unistd.h>
#include <syslog.h>
#include <stdio.h>

/* Closed-library entries (bk_idk prebuilt archives, ABI-matched:
 * armv8-m.main / fpv5-sp-d16 / hard float).
 */

extern int  bt_os_adapter_init(void *funcs);
extern int  bluetooth_controller_init(void);
extern int  bk_ble_reg_hci_recv_callback(int (*evt_cb)(uint8_t *, uint16_t),
                                         int (*acl_cb)(uint8_t *, uint16_t));
extern int  bk_ble_hci_cmd_to_controller(uint8_t *buf, uint16_t len);
extern int  bk_ble_create_advertising(void);
extern int  bt_feature_adapter_init(void *arg);
extern int  phy_adapter_init(void *funcs, void *vars);
extern void rf_adapter_init(const void *funcs, const void *vars);
extern void calibration_init(void);
extern void rf_module_vote_ctrl(uint32_t cmd, uint32_t bit);

/* The OSI table (bk7258_bt_osi.c).  Its address is taken, never called:
 * without a reference the whole object -- and every closed PHY symbol its
 * table entries point at -- is dropped by --gc-sections, and a build that
 * discards the code proves nothing about whether it links.
 */

extern int bk7258_bt_osi_init(void);

/* The PHY/RF adapter tables (bk7258_phy_osi.c), for the same reason:
 * unreferenced, --gc-sections drops the object along with every closed
 * PHY symbol its tables relocate against.
 */

extern int bk7258_phy_adapter_init(void);
extern int bk7258_rf_adapter_init(void);

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

/****************************************************************************
 * Name: bk7258_bt_cal_init
 *
 * Description:
 *   The radio calibration step, which this port had been skipping.
 *   The vendor runs it between the adapter tables and the controller
 *   (bk_init.c: vnd_cal_overlay then bk_cal_if_init on the BT-only
 *   path), and the controller's own log said so all along --
 *   cali_ready_status 0x0.  An untrimmed analogue front end explains a
 *   link layer that schedules events perfectly and neither transmits
 *   nor hears anything.
 *
 *   No external PA on this board: EPA_ENABLE_FLAG is undefined in the
 *   vendor tree, so the overlay passes 0 and the two GPIO numbers go
 *   unused.
 *
 *   The SARADC callbacks calibration needs -- TSSI power on channel 8,
 *   die temperature on 7, supply on 0 -- are now real, driven by
 *   bk7258_saradc.c, so the trim is solved against measurements rather
 *   than against stubs that reported failure.  That was a prerequisite,
 *   but calling it the cause of the UsageFault this path used to take
 *   does not survive checking: a divide by zero cannot fault here,
 *   because nothing in this tree sets CCR.DIV_0_TRP and the reset
 *   default leaves an integer divide by zero returning zero quietly.
 *   Whether the fault is gone is therefore an open question, and this
 *   stays off the default path until a board run answers it.  If it
 *   still faults, read CFSR rather than assuming: the calibration
 *   writes the TRX block at 0x4980c000 directly, which needs the RF
 *   domain and modem clock this function's first two table votes are
 *   supposed to have raised.
 *
 ****************************************************************************/

struct auto_pwr_cali_s
{
  uint32_t cali_mode;
  int32_t  gtx_tssi_thred_chan1_b;
  int32_t  gtx_tssi_thred_chan7_b;
  int32_t  gtx_tssi_thred_chan13_b;
  int32_t  gtx_tssi_thred_chan1_g;
  int32_t  gtx_tssi_thred_chan7_g;
  int32_t  gtx_tssi_thred_chan13_g;
};

extern void vnd_cal_set_auto_pwr_thred(struct auto_pwr_cali_s ctx);
extern void vnd_cal_set_epa_config(uint8_t epa_flag, uint16_t rx_gpio,
                                   uint16_t tx_gpio, uint32_t gainbase_b,
                                   uint32_t gainbase_g);
extern void vnd_cal_set_cca_level(uint8_t offset);
extern int  bk_cal_if_init(void);

extern const uint32_t pwr_gain_base_gain_b;
extern const uint32_t pwr_gain_base_gain_g;

int bk7258_bt_cal_init(void)
{
  static const struct auto_pwr_cali_s auto_pwr =
  {
    0x1,                    /* manual calibration mode */
    0x253, 0x253, 0x257,    /* TSSI thresholds, 802.11b channels */
    0x23f, 0x22b, 0x22b     /* 802.11g channels */
  };

  vnd_cal_set_auto_pwr_thred(auto_pwr);
  vnd_cal_set_epa_config(0, 28, 26, pwr_gain_base_gain_b,
                         pwr_gain_base_gain_g);
  vnd_cal_set_cca_level(0);

  return bk_cal_if_init();
}

int bk7258_bt_controller_init(void)
{
  return bluetooth_controller_init();
}

/****************************************************************************
 * Raw HCI advertising
 *
 * The controller exposes a standard HCI boundary, so a handful of Core
 * spec commands are enough to get on the air -- no host stack in the
 * picture at all, which makes this the shortest honest proof that the
 * radio works end to end.
 ****************************************************************************/

static volatile int g_hci_evt;
static uint8_t g_hci_status;

static volatile int g_adv_reports;

static int ble_hci_evt_cb(uint8_t *buf, uint16_t len)
{
  /* LE Advertising Report: proof that the receiver hears the world.
   * Meta event 0x3e, subevent 0x02, then one report per entry:
   * event type, address type, six address bytes, data length, data,
   * and RSSI as the last byte.
   */

  if (len >= 12 && buf[0] == 0x3e && buf[2] == 0x02)
    {
      uint8_t dlen = buf[11];
      int8_t rssi = (len >= 13 + dlen) ? (int8_t)buf[12 + dlen] : 0;

      g_adv_reports++;
      syslog(LOG_INFO,
             "ble: heard %02x:%02x:%02x:%02x:%02x:%02x rssi %d\n",
             buf[10], buf[9], buf[8], buf[7], buf[6], buf[5], rssi);
      return 0;
    }

  /* Whether the transport hands up a bare event or keeps the H4 type
   * byte in front is not documented either way, so accept both and
   * show the raw bytes: if this never prints, the events are not
   * coming through the VHCI path at all, which is a different problem
   * from parsing them wrong.
   */

  uint8_t *e = buf;
  char hex[3 * 8 + 1];
  int n = len > 8 ? 8 : len;
  int i;

  for (i = 0; i < n; i++)
    {
      snprintf(hex + i * 3, 4, "%02x ", buf[i]);
    }

  hex[n * 3] = '\0';
  syslog(LOG_INFO, "hci: evt len %u [%s]\n", (unsigned)len, hex);

  if (len >= 1 && buf[0] == 0x04)
    {
      e = buf + 1;
      len--;
    }

  if (len >= 4 && (e[0] == 0x0e || e[0] == 0x0f))
    {
      g_hci_status = (e[0] == 0x0e) ? e[5] : e[2];
      g_hci_evt = 1;
    }

  return 0;
}

static int ble_hci_acl_cb(uint8_t *buf, uint16_t len)
{
  return 0;
}

static int hci_cmd(uint16_t opcode, const uint8_t *params, uint8_t plen)
{
  uint8_t buf[64];
  int waited;

  buf[0] = opcode & 0xff;
  buf[1] = opcode >> 8;
  buf[2] = plen;
  if (plen > 0)
    {
      memcpy(buf + 3, params, plen);
    }

  g_hci_evt = 0;
  g_hci_status = 0xff;

  if (bk_ble_hci_cmd_to_controller(buf, plen + 3) != 0)
    {
      syslog(LOG_INFO, "hci: cmd %04x rejected on submit\n", opcode);
      return -1;
    }

  syslog(LOG_INFO, "hci: cmd %04x sent (%u bytes)\n", opcode,
         (unsigned)(plen + 3));

  for (waited = 0; waited < 500 && g_hci_evt == 0; waited++)
    {
      usleep(10 * 1000);
    }

  syslog(LOG_INFO, "hci: cmd %04x waited %d, evt %d, status %02x\n",
         opcode, waited, g_hci_evt, g_hci_status);

  if (g_hci_evt == 0)
    {
      return -2;
    }

  return g_hci_status;
}

/****************************************************************************
 * Name: bk7258_ble_scan
 *
 * Description:
 *   Listen for other people's advertisements for a few seconds.  This
 *   is the one proof of a working radio that needs nothing but the
 *   board: any phone, headset or fitness band nearby is transmitting,
 *   and hearing them exercises the same transceiver, antenna and
 *   calibration the transmitter uses.
 *
 ****************************************************************************/

int bk7258_ble_scan(int seconds)
{
  static const uint8_t scan_params[7] =
  {
    0x00,                   /* passive: listen, never ask for more */
    0x10, 0x00,             /* interval 16 * 0.625 ms = 10 ms */
    0x10, 0x00,             /* window: listen the whole interval */
    0x00,                   /* own address: public */
    0x00                    /* accept every advertiser */
  };

  uint8_t enable[2] = { 0x01, 0x00 };
  int ret;
  int i;

  ret = bk_ble_reg_hci_recv_callback(ble_hci_evt_cb, ble_hci_acl_cb);
  if (ret != 0)
    {
      return -1;
    }

  g_adv_reports = 0;

  ret = hci_cmd(0x200b, scan_params, sizeof(scan_params));
  syslog(LOG_INFO, "ble: scan params -> %d\n", ret);
  if (ret != 0)
    {
      return ret;
    }

  ret = hci_cmd(0x200c, enable, 2);
  syslog(LOG_INFO, "ble: scan enable -> %d\n", ret);
  if (ret != 0)
    {
      return ret;
    }

  for (i = 0; i < seconds; i++)
    {
      sleep(1);
      syslog(LOG_INFO, "ble: %d s, %d reports\n", i + 1, g_adv_reports);
    }

  enable[0] = 0x00;
  hci_cmd(0x200c, enable, 2);
  return g_adv_reports;
}

/****************************************************************************
 * Name: bk7258_ble_adv_start
 *
 * Description:
 *   Reset, configure a 100 ms connectable advertisement carrying name,
 *   and switch the transmitter on.  Returns 0 when the controller
 *   accepted every step.
 *
 ****************************************************************************/

int bk7258_ble_adv_start(const char *name)
{
  static const uint8_t adv_params[15] =
  {
    0xa0, 0x00,             /* min interval, 160 * 0.625 ms = 100 ms */
    0xa0, 0x00,             /* max interval */
    0x00,                   /* ADV_IND, connectable undirected */
    0x00,                   /* own address: public */
    0x00,                   /* peer address type */
    0, 0, 0, 0, 0, 0,       /* peer address, unused for undirected */
    0x07,                   /* all three advertising channels */
    0x00                    /* no scan/connect filtering */
  };

  uint8_t adv_data[32];
  uint8_t enable = 0x01;
  size_t nlen = strlen(name);
  int ret;

  if (nlen > 26)
    {
      nlen = 26;
    }

  memset(adv_data, 0, sizeof(adv_data));
  adv_data[1] = 0x02;                 /* flags AD: length */
  adv_data[2] = 0x01;                 /* flags AD: type */
  adv_data[3] = 0x06;                 /* general discoverable, LE only */
  adv_data[4] = (uint8_t)(nlen + 1);  /* name AD: length */
  adv_data[5] = 0x09;                 /* name AD: complete local name */
  memcpy(adv_data + 6, name, nlen);
  adv_data[0] = (uint8_t)(5 + nlen);  /* significant part length */

  ret = bk_ble_reg_hci_recv_callback(ble_hci_evt_cb, ble_hci_acl_cb);
  syslog(LOG_INFO, "hci: reg callback -> %d\n", ret);
  if (ret != 0)
    {
      return -1;
    }

  /* Probe first, and do not stop at the first silence: HCI_Reset is
   * answered but LE_Set_Advertising_Parameters was not, so the useful
   * question is which command groups this path answers at all.  A
   * vendor-info read, an LE buffer read and an LE feature read bracket
   * the three cases (base band, LE informational, LE control).
   */

    {
      extern void bk7258_bt_osi_diag(void);

      bk7258_bt_osi_diag();
      usleep(500 * 1000);
      bk7258_bt_osi_diag();
    }

  syslog(LOG_INFO, "hci: probe read_local_version -> %d\n",
         hci_cmd(0x1001, NULL, 0));
  syslog(LOG_INFO, "hci: probe le_read_buffer_size -> %d\n",
         hci_cmd(0x2002, NULL, 0));
  syslog(LOG_INFO, "hci: probe le_read_local_features -> %d\n",
         hci_cmd(0x2003, NULL, 0));

  syslog(LOG_INFO, "hci: adv_params -> %d\n",
         hci_cmd(0x2006, adv_params, sizeof(adv_params)));
  syslog(LOG_INFO, "hci: adv_data -> %d\n",
         hci_cmd(0x2008, adv_data, 32));

  ret = hci_cmd(0x200a, &enable, 1);
  syslog(LOG_INFO, "hci: adv_enable -> %d\n", ret);

    {
      /* Sample the interrupt counter only now: before the transmitter
       * is enabled the link layer has nothing to schedule, so a zero
       * reading earlier said nothing.  A count that climbs here is the
       * radio actually running advertising events.
       */

      extern void bk7258_bt_osi_diag(void);

      bk7258_bt_osi_diag();
      sleep(2);
      bk7258_bt_osi_diag();
      sleep(2);
      bk7258_bt_osi_diag();
    }

  return ret;
}

uintptr_t bk7258_ble_link_probe(void)
{
  return (uintptr_t)bk7258_bt_osi_init +
         (uintptr_t)bk7258_phy_adapter_init +
         (uintptr_t)bk7258_rf_adapter_init +
         (uintptr_t)bk7258_bt_feature_init +
         (uintptr_t)bk7258_bt_controller_init +
         (uintptr_t)bk7258_ble_adv_start +
         (uintptr_t)bk7258_ble_scan +
         (uintptr_t)bk7258_bt_cal_init +
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
