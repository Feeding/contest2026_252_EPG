/****************************************************************************
 * boards/arm/bk7258/contest_board/chip/bk7258_ble.c
 *
 * BK7258 BLE controller, OSI/PHY integration and raw-HCI diagnostics.
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <syslog.h>
#include <stdio.h>

/* Closed-library entries (Beken AVDK prebuilt archives, ABI-matched:
 * armv8-m.main / fpv5-sp-d16 / hard float).
 */

extern int  bt_os_adapter_init(void *funcs);
extern int  bluetooth_controller_init(void);
extern int  bk_ble_reg_hci_recv_callback(int (*evt_cb)(uint8_t *, uint16_t),
                                         int (*acl_cb)(uint8_t *, uint16_t));
extern int  bk_ble_hci_cmd_to_controller(uint8_t *buf, uint16_t len);
extern int  bt_feature_adapter_init(void *arg);
extern int  phy_adapter_init(void *funcs, void *vars);
extern void rf_adapter_init(const void *funcs, const void *vars);
extern void calibration_init(void);
extern void rf_module_vote_ctrl(uint32_t cmd, uint32_t bit);

/* rf_module_vote_ctrl() owns a bitmask, not a reference count.  Never use
 * RF_BY_BLE_BIT for a board-level hold: the closed controller legitimately
 * clears that bit during its own RF lifecycle.  RF_BY_BKREG_BIT is the SDK's
 * dedicated manual/diagnostic hold and therefore remains independent.
 */

#define BLE_RF_CLOSE                 0u
#define BLE_RF_OPEN                  1u
#define BLE_RF_BOARD_HOLD_BIT        (1u << 2)

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
  uint8_t rf_mode;
  uint8_t support_coex_rf_mode_switch;
  uint8_t support_sleep_phy_switch;
};

#define BT_RF_MODE_IQ_LOW_PLL 2

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
  g_bt_feature.rf_mode = BT_RF_MODE_IQ_LOW_PLL;
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
 *   Two separate things had to be fixed before this could run, and only
 *   one of them was the ADC.
 *
 *   The SARADC callbacks calibration needs -- TSSI power on channel 8,
 *   die temperature on 7, supply on 0 -- are now real, driven by
 *   bk7258_saradc.c, so the trim is solved against measurements rather
 *   than against stubs that reported failure.  Necessary, but it was
 *   never the cause of the UsageFault: nothing in this tree sets
 *   CCR.DIV_0_TRP, so a divide by zero here returns zero quietly rather
 *   than faulting.  CFSR read 0x00020000 -- UFSR.INVSTATE, a branch to an
 *   address with the Thumb bit clear -- with R3 zero.
 *
 *   That was nv_init, three statements into calibration_main, tail-calling
 *   a null _nv_phy_reg_set_hook; see the entry of that name in
 *   bk7258_phy_osi.c for why the table had a hole exactly there.  It is
 *   filled in now, and it was the only reachable hole: with --gc-sections
 *   applied, that entry was the one null slot any surviving code could
 *   still call.
 *
 *   Still unproven on hardware, so this stays off the default path.  If it
 *   faults again, read CFSR rather than assuming: calibration writes the
 *   TRX block at 0x4980c000 directly, which needs the RF domain and modem
 *   clock this function's first two table votes are supposed to raise, and
 *   that would show as a bus fault, not INVSTATE.
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

/****************************************************************************
 * Name: bk7258_ble_use_bt_pll
 *
 * Description:
 *   Select the AVDK IQ path driven by the Bluetooth low PLL and load the
 *   fallback BLE transmit-power table before controller initialisation.
 *   This matches the standalone receiver configuration already proven on
 *   this board and does not depend on the Polar demo's Wi-Fi calibration.
 *
 ****************************************************************************/

extern void ble_enter_polar_mode(void);
extern void ble_enter_iq_mode(void);
extern int manual_cal_load_default_txpwr_polar_tab(uint32_t status);
extern int manual_cal_load_default_txpwr_tab(uint32_t status);
extern uint8_t manual_cal_get_ble_pwr_idx(uint8_t channel);
extern uint8_t gtxpwr_tab_polar_ble[40];
extern const uint8_t gtxpwr_tab_def_polar_ble[40];
extern uint8_t gtxpwr_tab_ble[40];
extern const uint8_t gtxpwr_tab_def_ble[40];
extern volatile uint16_t rwnx_rfconfig;

static int bk7258_ble_load_default_polar_power(void)
{
  int ret = manual_cal_load_default_txpwr_polar_tab(0);

  /* With no RF partition the closed PHY stays in automatic-calibration
   * mode.  Its loader deliberately refuses to install defaults in that
   * mode, but automatic TSSI calibration is unavailable in this port.
   * Use the board's AVDK table as the documented no-partition fallback.
   */

  if ((gtxpwr_tab_polar_ble[19] & 0x7f) == 0)
    {
      memcpy(gtxpwr_tab_polar_ble, gtxpwr_tab_def_polar_ble,
             sizeof(gtxpwr_tab_polar_ble));
      syslog(LOG_INFO, "ble: installed AVDK fallback polar power table\n");
    }

  return ret;
}

static int bk7258_ble_load_default_iq_power(void)
{
  int ret = manual_cal_load_default_txpwr_tab(0);

  if ((gtxpwr_tab_ble[19] & 0x7f) == 0)
    {
      memcpy(gtxpwr_tab_ble, gtxpwr_tab_def_ble, sizeof(gtxpwr_tab_ble));
      syslog(LOG_INFO, "ble: installed AVDK fallback IQ power table\n");
    }

  return ret;
}

int bk7258_ble_use_bt_pll(void)
{
  int ret;

  syslog(LOG_INFO, "ble: rfconfig before %04x\n", rwnx_rfconfig);
  ble_enter_iq_mode();
  ret = bk7258_ble_load_default_iq_power();
  syslog(LOG_INFO,
         "ble: enter IQ/BTPLL mode -> %d, rfconfig now %04x, ch19 %u\n",
         ret, rwnx_rfconfig, manual_cal_get_ble_pwr_idx(19));
  return ret;
}

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
  int ret;

  /* The standalone IQ/BTPLL path is selected before controller start.
   * Do not call the legacy full Wi-Fi/BT calibration here: its ATE
   * preparation path assumes SDK boot services this NuttX port
   * intentionally does not provide.
   */

  ret = bluetooth_controller_init();
  if (ret == 0)
    {
      /* Restore the BLE table after the controller's final RF-mode switch
       * and before the first advertising event asks for an index.
       */

      int load_ret = bk7258_ble_load_default_iq_power();

      syslog(LOG_INFO,
             "ble: IQ power table reload -> %d, raw %u, ch19 %u\n",
             load_ret, gtxpwr_tab_ble[19] & 0x7f,
             manual_cal_get_ble_pwr_idx(19));
    }

  return ret;
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
static volatile uint8_t g_hci_status;
static volatile uint16_t g_hci_opcode;

static volatile int g_adv_reports;

static int ble_hci_evt_cb(uint8_t *buf, uint16_t len)
{
  /* LE Advertising Report: proof that the receiver hears the world.
   * Meta event 0x3e, subevent 0x02, then one report per entry:
   * event type, address type, six address bytes, data length, data,
   * and RSSI as the last byte.
   */

  if (len >= 13 && buf[0] == 0x3e && buf[2] == 0x02)
    {
      /* Report layout after the meta header and report count: event
       * type, address type, six address bytes, data length, the data
       * itself, then RSSI.  An earlier version started the address one
       * byte early, which is why every RSSI it printed was nonsense.
       */

      uint8_t atype = buf[5];
      uint8_t *addr = buf + 6;
      uint8_t dlen = buf[12];
      int8_t rssi = (len > 13 + dlen) ? (int8_t)buf[13 + dlen] : 0;
      char nm[32];
      int nlen = 0;
      int i = 13;

      nm[0] = '\0';
      while (i + 1 < 13 + dlen)
        {
          uint8_t alen = buf[i];
          uint8_t atyp = buf[i + 1];

          if (alen == 0)
            {
              break;
            }

          if ((atyp == 0x09 || atyp == 0x08) && alen > 1)
            {
              nlen = alen - 1;
              if (nlen > 30)
                {
                  nlen = 30;
                }

              memcpy(nm, buf + i + 2, nlen);
              nm[nlen] = '\0';
              break;
            }

          i += alen + 1;
        }

      g_adv_reports++;
      syslog(LOG_INFO,
             "ble: %02x:%02x:%02x:%02x:%02x:%02x t%u %ddBm %s\n",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0],
             atype, rssi, nlen ? nm : "-");
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

  if (len >= 6 && e[0] == 0x0e &&
      (uint16_t)(e[3] | ((uint16_t)e[4] << 8)) == g_hci_opcode)
    {
      g_hci_status = e[5];
      g_hci_evt = 1;
    }
  else if (len >= 6 && e[0] == 0x0f &&
           (uint16_t)(e[4] | ((uint16_t)e[5] << 8)) == g_hci_opcode)
    {
      g_hci_status = e[2];
      g_hci_evt = 1;
    }

  return 0;
}

static int ble_hci_acl_cb(uint8_t *buf, uint16_t len)
{
  return 0;
}

static int hci_cmd(uint16_t opcode, const uint8_t *params, uint8_t plen);

static int hci_register_once(void)
{
  static int s_registered;

  if (s_registered)
    {
      return 0;
    }

  if (bk_ble_reg_hci_recv_callback(ble_hci_evt_cb, ble_hci_acl_cb) != 0)
    {
      return -1;
    }

  s_registered = 1;

  return 0;
}

/* LE_Set_Event_Mask is deliberately never sent.  This controller
 * delivers advertising reports on its power-on default and stops
 * delivering them after any 0x2001, including the Core spec's own
 * default value of 0x1f -- verified both ways on hardware.
 */


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
  g_hci_opcode = opcode;

  if (bk_ble_hci_cmd_to_controller(buf, plen + 3) != 0)
    {
      syslog(LOG_INFO, "hci: cmd %04x rejected on submit\n", opcode);
      return -1;
    }

  syslog(LOG_INFO, "hci: cmd %04x sent (%u bytes)\n", opcode,
         (unsigned)(plen + 3));

  for (waited = 0; waited < 1500 && g_hci_evt == 0; waited++)
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
 * Name: bk7258_ble_txpwr
 *
 * Description:
 *   Report and optionally override the transmit power index.  The
 *   receiver demonstrably works while nothing hears the transmitter,
 *   and an index left at the bottom of the table is the cheapest
 *   explanation: calibration found no factory record, so whatever the
 *   fallback put here decides how far the advertisement carries.
 *
 ****************************************************************************/

extern void ble_cal_set_txpwr(uint8_t idx);

int bk7258_ble_txpwr(int idx)
{
  uint8_t ch;

  for (ch = 0; ch < 40; ch += 13)
    {
      syslog(LOG_INFO, "ble: pwr idx ch%u = %u\n", ch,
             manual_cal_get_ble_pwr_idx(ch));
    }

  if (idx >= 0)
    {
      ble_cal_set_txpwr((uint8_t)idx);
      syslog(LOG_INFO, "ble: forced pwr idx %d\n", idx);
    }

  return 0;
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

/****************************************************************************
 * Name: bk7258_ble_adv_stop
 *
 * Description:
 *   Turn the transmitter off.  Exists to settle one question the earlier
 *   "advertising kills the scan" measurement could not: a controller with
 *   a single activity slot would show the same thing while working
 *   perfectly.  If reception comes back after this, advertising was
 *   simply holding the radio; if it stays dead, the radio is wedged.
 *
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_ble_tx_test / bk7258_ble_hci_reset
 *
 * Description:
 *   Direct Test Mode transmit is the shortest path to keying the
 *   transmitter -- one command, no advertising state machine, no PDU
 *   construction -- so if it wedges the radio the same way advertising
 *   does, the fault is in the transmit chain itself rather than
 *   anywhere above it.  The reset is here to find out whether the wedge
 *   is soft state the controller can be talked out of.
 *
 ****************************************************************************/

int bk7258_ble_tx_test(int channel, int seconds)
{
  uint8_t p[3];
  int ret;

  p[0] = (uint8_t)channel;   /* 0..39, (F - 2402) / 2 */
  p[1] = 37;                 /* payload length */
  p[2] = 0;                  /* PRBS9 */

  ret = hci_cmd(0x201e, p, 3);
  syslog(LOG_INFO, "ble: tx_test start ch%d -> %d\n", channel, ret);
  if (ret != 0)
    {
      return ret;
    }

  sleep(seconds);

  ret = hci_cmd(0x201f, NULL, 0);
  syslog(LOG_INFO, "ble: tx_test stop -> %d\n", ret);
  return ret;
}

int bk7258_ble_hci_reset(void)
{
  return hci_cmd(0x0c03, NULL, 0);
}

int bk7258_ble_adv_stop(void)
{
  uint8_t off = 0;
  int ret;

  ret = hci_cmd(0x200a, &off, 1);
  if (ret == 0)
    {
      rf_module_vote_ctrl(BLE_RF_CLOSE, BLE_RF_BOARD_HOLD_BIT);
      syslog(LOG_INFO, "ble: board RF hold released\n");
    }

  return ret;
}

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

  /* Register once per boot.  Handing the controller a fresh callback
   * pair on every scan left the second scan hearing nothing at all --
   * an artefact that masqueraded as the radio being wedged by whatever
   * ran in between.
   */

  ret = hci_register_once();
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
 *   Configure a 100 ms scannable advertisement carrying name,
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
    0x00,                   /* ADV_IND, connectable/scannable undirected */
    0x01,                   /* own address: static random */
    0x00,                   /* peer address type */
    0, 0, 0, 0, 0, 0,       /* peer address, unused for undirected */
    0x07,                   /* all three advertising channels */
    0x00                    /* no scan/connect filtering */
  };

  static const uint8_t rnd[6] =
  {
    0x01, 0x03, 0x08, 0x26, 0x52, 0xd2
  };

  uint8_t adv_data[32];
  uint8_t scan_rsp[32];
  uint8_t enable = 1;
  size_t nlen = strlen(name);
  int ret;

  /* Reserve ten bytes for a diagnostic manufacturer-data AD structure.
   * Together with flags and FFF0 this leaves twelve bytes for the complete
   * local name and fills the legacy 31-byte payload exactly.
   */

  if (nlen > 12)
    {
      nlen = 12;
    }

  memset(adv_data, 0, sizeof(adv_data));
  adv_data[0] = (uint8_t)(19 + nlen); /* significant part length */
  adv_data[1] = 0x02;                 /* flags AD: length */
  adv_data[2] = 0x01;                 /* flags AD: type */
  adv_data[3] = 0x06;                 /* general discoverable, LE only */
  adv_data[4] = 0x03;                 /* service AD: length */
  adv_data[5] = 0x03;                 /* complete 16-bit UUID list */
  adv_data[6] = 0xf0;                 /* contest EPG service 0xfff0 */
  adv_data[7] = 0xff;
  adv_data[8] = (uint8_t)(nlen + 1);  /* name AD: length */
  adv_data[9] = 0x09;                 /* name AD: complete local name */
  memcpy(adv_data + 10, name, nlen);

  /* Test company ID 0xffff plus ASCII "EPG252".  nRF Connect exposes the
   * raw manufacturer field even when an OS suppresses the local name, so
   * this is an unambiguous board marker during bring-up.
   */

  adv_data[10 + nlen] = 0x09;
  adv_data[11 + nlen] = 0xff;
  adv_data[12 + nlen] = 0xff;
  adv_data[13 + nlen] = 0xff;
  adv_data[14 + nlen] = 'E';
  adv_data[15 + nlen] = 'P';
  adv_data[16 + nlen] = 'G';
  adv_data[17 + nlen] = '2';
  adv_data[18 + nlen] = '5';
  adv_data[19 + nlen] = '2';

  /* Repeat the complete name in the scan response for active scanners that
   * do not surface it from the primary PDU.  HCI parameter byte zero is the
   * data length; the following bytes contain the usual AD structure.
   */

  memset(scan_rsp, 0, sizeof(scan_rsp));
  scan_rsp[0] = (uint8_t)(nlen + 2);
  scan_rsp[1] = (uint8_t)(nlen + 1);
  scan_rsp[2] = 0x09;                 /* complete local name */
  memcpy(scan_rsp + 3, name, nlen);

  ret = hci_register_once();
  syslog(LOG_INFO, "hci: reg callback -> %d\n", ret);
  if (ret != 0)
    {
      return -1;
    }

  /* Advertise from a static random address, which is what the product
   * firmware for this board does: its source sets own_addr_type to
   * random with the public option commented out beside it.  Nothing
   * here confirms the controller ever adopted a usable public address,
   * and enabling advertising with a declared address type that has no
   * address behind it is rejected outright -- error 0x12.  The top two
   * bits of the last byte mark the address static random, as the vendor
   * also does.
   */

  ret = hci_cmd(0x2005, rnd, sizeof(rnd));
  if (ret != 0)
    {
      return ret;
    }

  ret = hci_cmd(0x2006, adv_params, sizeof(adv_params));
  if (ret != 0)
    {
      return ret;
    }

  ret = hci_cmd(0x2008, adv_data, sizeof(adv_data));
  if (ret != 0)
    {
      return ret;
    }

  ret = hci_cmd(0x2009, scan_rsp, sizeof(scan_rsp));
  if (ret != 0)
    {
      return ret;
    }

  /* Hold the radio with the SDK's independent BKREG/manual vote before
   * keying the transmitter.  Reusing RF_BY_BLE_BIT here is not a second
   * reference: the arbiter stores a bitmask, so the controller's later
   * RF_CLOSE clears that shared bit and leaves HCI advertising enabled with
   * no carrier.  This board hold intentionally trades low-power operation
   * for stable discoverability; bk7258_ble_adv_stop() releases it.
   */

  rf_module_vote_ctrl(BLE_RF_OPEN, BLE_RF_BOARD_HOLD_BIT);
  syslog(LOG_INFO, "ble: board RF hold acquired\n");

  ret = hci_cmd(0x200a, &enable, 1);
  if (ret != 0)
    {
      rf_module_vote_ctrl(BLE_RF_CLOSE, BLE_RF_BOARD_HOLD_BIT);
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
         (uintptr_t)bk7258_ble_adv_stop +
         (uintptr_t)bk7258_ble_tx_test +
         (uintptr_t)bk7258_ble_hci_reset +
         (uintptr_t)bk7258_bt_cal_init +
         (uintptr_t)bk7258_ble_use_bt_pll +
         (uintptr_t)bk7258_ble_txpwr +
         (uintptr_t)bt_os_adapter_init +
         (uintptr_t)bluetooth_controller_init +
         (uintptr_t)bk_ble_reg_hci_recv_callback +
         (uintptr_t)bk_ble_hci_cmd_to_controller +
         (uintptr_t)phy_adapter_init +
         (uintptr_t)rf_adapter_init +
         (uintptr_t)calibration_init +
         (uintptr_t)rf_module_vote_ctrl;
}
