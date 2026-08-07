/****************************************************************************
 * board/contest_board/chip/bk7258_wifi_glue.c
 *
 * The calls into Beken's WiFi stack that need the vendor's own headers.
 *
 * Like bk7258_wifi_pbuf.c, this file is ours but is compiled inside the
 * vendor OBJECT library (bk7258_wifi_vendor.cmake) rather than with the
 * NuttX flags the rest of chip/ uses.  That is what lets it see
 * modules/wifi_types.h, and it is the whole reason the file exists.
 *
 * bk7258_wifi.c used to call the stack directly with a hand-written
 * declaration, "extern int bk_wifi_init(void)".  That is wrong, it linked
 * anyway, and the way it failed is worth recording because nothing about
 * the symptom pointed at the cause:
 *
 *   The real entry point is bk_err_t bk_wifi_init(const wifi_init_config_t
 *   *config), and its first act is to read config->os_funcs.  Called with
 *   no argument, r0 held whatever the caller happened to leave there.  The
 *   NULL check that guards this exact mistake passed, because garbage is
 *   not NULL, so the stack stored the garbage pointer in g_wifi_funcs and
 *   carried on.  rwnxl_init() then loaded a function pointer from offset
 *   0x238 of it and branched -- an instruction bus fault (CFSR 0x00000100,
 *   IBUSERR) roughly thirty function calls away from the actual error, in
 *   closed-source code, with no undefined symbol and no link warning
 *   anywhere.
 *
 * Hence this file: the config is built from the vendor's own macro, so the
 * table it points at is whatever the SDK says it should be.
 *
 * The macro also has a second, less obvious job.  g_wifi_os_funcs is an
 * ordinary global that nothing else in the tree references -- the closed
 * archives do not import it by name -- so with -ffunction-sections and
 * --gc-sections it was being collected out of the image entirely.  Naming
 * it here is what keeps it alive.
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <common/bk_include.h>
#include <modules/wifi.h>
#include <modules/wifi_types.h>

/* Declares g_wifi_os_funcs and g_wifi_os_variable, which the config macro
 * above names.  Reached through the "generated/" prefix because the vendor
 * include list stops at components/bk_wifi/include.
 */

#include "generated/lmac_wifi_adapter.h"

/* SCAN_PARAM_T, rw_msg_send_add_if, rw_msg_send_scanu_req, sr_get_scan_*. */

#include "bk_private/bk_rw.h"

#include "bk7258_wifi_scan.h"


/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_wifi_vendor_init
 *
 * Description:
 *   Bring the vendor MAC stack up.  Called from bk7258_wifi_ifup().
 *
 * Returned Value:
 *   BK_OK (0) on success, the vendor's negative error code otherwise.
 *
 ****************************************************************************/

int bk7258_wifi_vendor_init(void)
{
  wifi_init_config_t config = WIFI_DEFAULT_INIT_CONFIG();

  /* The PHY and RF adapters have to be registered before this runs -- see
   * bk7258_wifi_ifup(), which does it with this port's own tables from
   * bk7258_phy_osi.c rather than Beken's bk_phy_adapter.c.  Compiling
   * theirs instead is not an option: both define g_phy_os_funcs.
   */

  return bk_wifi_init(&config);
}

/****************************************************************************
 * Name: bk7258_wifi_scan_start
 *
 * Description:
 *   Start a scan without the supplicant.
 *
 *   bk_wifi_scan_start(), the documented entry point, cannot be used here:
 *   its second act is wifi_supplicant_start() -> wlan_sta_enable() ->
 *   wpa_ctrl_request(), which this port stubs out.  It logs "wifi enable
 *   fail" and returns BK_OK without ever asking the MAC to scan, so the
 *   call looks like it worked.
 *
 *   The layer underneath does not need the supplicant at all.  Scan results
 *   arrive as SCANU_RESULT_IND and are accumulated by rw_msg_rx.c into its
 *   own scan_rst_set_ptr; the supplicant only gets told afterwards, by a
 *   wpa_ctrl_event() call that is the last statement in the SCANU_START_CFM
 *   case and whose failure costs nothing.  So the results are complete and
 *   sorted whether or not anything is listening.
 *
 *   sa_station_init() is what brings the MAC up far enough to scan -- reset,
 *   me_config, chan_config, start -- and it guards itself on whether a VIF
 *   already exists, so calling it again is harmless.
 *
 ****************************************************************************/

int bk7258_wifi_scan_start(void)
{
  static uint8_t vif_idx = 0xff;
  SCAN_PARAM_T param;
  int ret;

  sa_station_init();

  if (vif_idx == 0xff)
    {
      struct mm_add_if_cfm cfm;
      uint8_t mac[6];

      bk_wifi_sta_get_mac(mac);

      ret = rw_msg_send_add_if(mac, NL80211_IFTYPE_STATION, 0, &cfm);
      if (ret != 0 || cfm.status != 0)
        {
          return -1;
        }

      vif_idx = cfm.inst_nbr;
    }

  /* All-zero means: every supported channel, any BSSID, no SSID filter,
   * no extra IEs.  rw_msg_send_scanu_req() reads freqs[0] == 0 as "use
   * rw_ieee80211_init_scan_chan()", which is the full channel list.
   */

  os_memset(&param, 0, sizeof(param));
  param.vif_idx = vif_idx;

  return rw_msg_send_scanu_req(&param) == 0 ? 0 : -1;
}

int bk7258_wifi_scan_count(void)
{
  return (int)sr_get_scan_number();
}

int bk7258_wifi_scan_get(int index, struct bk7258_scan_ap_s *ap)
{
  SCAN_RST_UPLOAD_T *set;
  SCAN_RST_ITEM_T *item;
  int ret = -1;

  if (ap == NULL || index < 0)
    {
      return -1;
    }

  /* sr_get_scan_results() takes a reference; it has to be released or the
   * next scan cannot free the set.
   */

  set = (SCAN_RST_UPLOAD_T *)sr_get_scan_results();
  if (set == NULL)
    {
      return -1;
    }

  if (index < set->scanu_num && set->res[index] != NULL)
    {
      item = set->res[index];

      os_memcpy(ap->bssid, item->bssid, sizeof(ap->bssid));
      os_memcpy(ap->ssid, item->ssid, sizeof(item->ssid));
      ap->ssid[sizeof(item->ssid)] = '\0';
      ap->channel = item->channel;
      ap->rssi    = item->level;
      ap->caps    = item->caps;
      ret = 0;
    }

  sr_release_scan_results(set);
  return ret;
}
