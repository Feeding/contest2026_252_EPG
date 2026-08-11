/****************************************************************************
 * board/contest_board/chip/bk7258_wifi.c
 *
 * WiFi network device for the BK7258 -- the openvela-facing half.
 *
 * This file is the netdev lower half: it registers wlan0 and answers the
 * wireless ioctls.  The vendor MAC stack lives beside it, compiled from
 * bk_idk with the vendor's own flags (bk7258_wifi_vendor.cmake) and driven
 * from ifup() below.
 *
 * An earlier version of this comment claimed the vendor stack could not be
 * compiled at all -- that two internal headers, sm_task.h and ps.h, were
 * missing from every published SDK.  That was wrong, and the way it was
 * wrong is worth keeping: both #includes sit inside
 * "#if NX_VERSION > NX_VERSION_PACK(6,22,0,0)", and this tree is 6.8.2.0,
 * so the preprocessor never reaches them.  The conclusion came from
 * grepping for the include lines without checking what guarded them.  All
 * 33 sources in components/bk_wifi/src compile as published.
 *
 * The openvela side follows
 * docs/zh-cn/device_dev_guide/connection/network/driver/net_driver_guide.md,
 * with a complete worked example in nuttx/drivers/net/wifi_sim.c.
 *
 * The shape openvela expects is worth stating, because it is not the one a
 * Linux background suggests: wireless_ops_s carries essid, passwd and auth
 * alongside connect, so association and the security handshake belong to the
 * driver.  There is no separate supplicant process to feed -- the vendor's
 * own supplicant is linked into the image and driven from underneath.
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

#include <nuttx/config.h>

#include <debug.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>

#include <net/if.h>
#include <net/if_arp.h>
#include <nuttx/kmalloc.h>
#include <nuttx/net/net.h>
#include <nuttx/net/netdev_lowerhalf.h>
#include <nuttx/wireless/wireless.h>

#include "bk7258_wifi.h"
#include "bk7258_wifi_scan.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* How many packets the driver may hold.  Kept small until the vendor side
 * exists and the real depth of its queues is known -- a quota that lies is
 * worse than one that is conservative, because the upper half throttles on
 * it.
 */

#define BK7258_WIFI_TX_QUOTA   8
#define BK7258_WIFI_RX_QUOTA   8

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct bk7258_wifi_dev_s
{
  struct netdev_lowerhalf_s dev;    /* Must be first */

  /* Association parameters, held here until connect() hands them down.  The
   * wireless ops set them one at a time in any order, so they have to live
   * somewhere until the caller says go.
   */

  uint8_t  ssid[32 + 1];
  uint8_t  ssid_len;
  uint8_t  bssid[IFHWADDRLEN];
  bool     bssid_set;
  char     passwd[64 + 1];
  uint32_t auth;                    /* IW_AUTH_WPA_VERSION_* */
  uint32_t mode;                    /* IW_MODE_INFRA or IW_MODE_MASTER */
  bool     connected;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int bk7258_wifi_ifup(FAR struct netdev_lowerhalf_s *dev);
static int bk7258_wifi_ifdown(FAR struct netdev_lowerhalf_s *dev);
static int bk7258_wifi_transmit(FAR struct netdev_lowerhalf_s *dev,
                                FAR netpkt_t *pkt);
static FAR netpkt_t *bk7258_wifi_receive(FAR struct netdev_lowerhalf_s *dev);

static int bk7258_wifi_connect(FAR struct netdev_lowerhalf_s *dev);
static int bk7258_wifi_disconnect(FAR struct netdev_lowerhalf_s *dev);
static int bk7258_wifi_essid(FAR struct netdev_lowerhalf_s *dev,
                             FAR struct iwreq *iwr, bool set);
static int bk7258_wifi_bssid(FAR struct netdev_lowerhalf_s *dev,
                             FAR struct iwreq *iwr, bool set);
static int bk7258_wifi_passwd(FAR struct netdev_lowerhalf_s *dev,
                              FAR struct iwreq *iwr, bool set);
static int bk7258_wifi_mode(FAR struct netdev_lowerhalf_s *dev,
                            FAR struct iwreq *iwr, bool set);
static int bk7258_wifi_auth(FAR struct netdev_lowerhalf_s *dev,
                            FAR struct iwreq *iwr, bool set);
static int bk7258_wifi_freq(FAR struct netdev_lowerhalf_s *dev,
                            FAR struct iwreq *iwr, bool set);
static int bk7258_wifi_bitrate(FAR struct netdev_lowerhalf_s *dev,
                               FAR struct iwreq *iwr, bool set);
static int bk7258_wifi_txpower(FAR struct netdev_lowerhalf_s *dev,
                               FAR struct iwreq *iwr, bool set);
static int bk7258_wifi_country(FAR struct netdev_lowerhalf_s *dev,
                               FAR struct iwreq *iwr, bool set);
static int bk7258_wifi_sensitivity(FAR struct netdev_lowerhalf_s *dev,
                                   FAR struct iwreq *iwr, bool set);
static int bk7258_wifi_scan(FAR struct netdev_lowerhalf_s *dev,
                            FAR struct iwreq *iwr, bool set);
static int bk7258_wifi_range(FAR struct netdev_lowerhalf_s *dev,
                             FAR struct iwreq *iwr);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct netdev_ops_s g_bk7258_wifi_ops =
{
  .ifup     = bk7258_wifi_ifup,
  .ifdown   = bk7258_wifi_ifdown,
  .transmit = bk7258_wifi_transmit,
  .receive  = bk7258_wifi_receive,

  /* addmac / rmmac are unimplemented on purpose: multicast filtering is the
   * MAC block's job and the vendor stack owns that register file.  reclaim
   * likewise waits for the vendor side -- there is nothing to reclaim until
   * something can queue.
   */
};

static const struct wireless_ops_s g_bk7258_wifi_iw_ops =
{
  .connect     = bk7258_wifi_connect,
  .disconnect  = bk7258_wifi_disconnect,
  .essid       = bk7258_wifi_essid,
  .bssid       = bk7258_wifi_bssid,
  .passwd      = bk7258_wifi_passwd,
  .mode        = bk7258_wifi_mode,
  .auth        = bk7258_wifi_auth,
  .freq        = bk7258_wifi_freq,
  .bitrate     = bk7258_wifi_bitrate,
  .txpower     = bk7258_wifi_txpower,
  .country     = bk7258_wifi_country,
  .sensitivity = bk7258_wifi_sensitivity,
  .scan        = bk7258_wifi_scan,
  .range       = bk7258_wifi_range,
};

static struct bk7258_wifi_dev_s g_bk7258_wifi;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* Everything below that ends in -ENOSYS is waiting on the vendor headers,
 * not on a decision.  They are separated from the ones that already do their
 * job so that "what is left" stays readable as the port fills in.
 */

/****************************************************************************
 * Name: bk7258_wifi_ifup / bk7258_wifi_ifdown
 ****************************************************************************/

static int bk7258_wifi_ifup(FAR struct netdev_lowerhalf_s *dev)
{
  UNUSED(dev);

#ifdef CONFIG_BK7258_WIFI_VENDOR
  /* First real call into the vendor stack.  Nothing above this point in the
   * port has ever executed vendor MAC code, so a link failure here is the
   * honest signal that the integration is not finished -- which is why the
   * call is made from ifup() rather than hidden behind another stub.
   *
   * It goes through bk7258_wifi_glue.c rather than straight to
   * bk_wifi_init() because the stack's entry point takes a config struct
   * whose type only exists behind the vendor include path.  Declaring it
   * here by hand once seemed harmless and cost an afternoon: see that
   * file's header.
   */

    {
      extern int bk7258_phy_adapter_init(void);
      extern int bk7258_rf_adapter_init(void);
      extern int bk7258_wifi_vendor_init(void);
      extern void bk7258_wifi_zeroing(bool on);
      static bool phy_ready = false;
      int ret;

      /* The radio's OS abstraction, before anything reaches the radio.
       * bk7258_phy_osi.c has carried these tables since the BLE port and
       * says so in its own header -- but only bk7258_ble.c ever called the
       * initialisers, and the xts configuration builds without BLE.
       *
       * The cost of missing this is not a link error.  wifi_init() calls
       * rf_module_vote_ctrl() inside libbk_phy.a, which loads g_rf_funcs_t
       * out of .bss and branches through offset 16 of the NULL it finds:
       * an instruction bus fault in closed code, six frames below anything
       * we wrote, with the WiFi stack looking like the culprit.
       *
       * Guarded because a BLE-enabled image runs them at BLE bring-up and
       * this would be the second time.
       */

      if (!phy_ready)
        {
          extern void bk7258_wifi_fault_probe_install(void);
          bk7258_wifi_fault_probe_install();

          bk7258_phy_adapter_init();
          bk7258_rf_adapter_init();
          phy_ready = true;
        }

      /* Left on for the life of the stack, not just across init.  The
       * first attempt scoped it to bk7258_wifi_vendor_init() and the board
       * got all the way through RF calibration before faulting on a
       * semaphore handle that was garbage rather than NULL -- the same
       * assumption, in an allocation made from the work queue after the
       * window had closed.  The assumption belongs to the vendor stack as
       * a whole.
       */

      bk7258_wifi_zeroing(true);
      ret = bk7258_wifi_vendor_init();

      if (ret != 0)
        {
          nerr("ERROR: bk_wifi_init: %d\n", ret);
          return -EIO;
        }

      ninfo("vendor WiFi stack initialised\n");
      return OK;
    }
#else
  nerr("ERROR: vendor WiFi sources not built (CONFIG_BK7258_WIFI_VENDOR)\n");
  return -ENOSYS;
#endif
}

static int bk7258_wifi_ifdown(FAR struct netdev_lowerhalf_s *dev)
{
  UNUSED(dev);
  return OK;
}

/****************************************************************************
 * Name: bk7258_wifi_transmit
 *
 * Description:
 *   Hand one frame to the MAC.  Ownership of pkt passes to us on success;
 *   on failure the upper half keeps it and will retry, so it must not be
 *   freed here.
 *
 ****************************************************************************/

static int bk7258_wifi_transmit(FAR struct netdev_lowerhalf_s *dev,
                                FAR netpkt_t *pkt)
{
  UNUSED(dev);
  UNUSED(pkt);

  return -ENOSYS;
}

/****************************************************************************
 * Name: bk7258_wifi_receive
 *
 * Description:
 *   Called after netdev_lower_rxready().  Returns one packet or NULL when
 *   the driver has none left; the upper half keeps calling until NULL.
 *
 ****************************************************************************/

static FAR netpkt_t *bk7258_wifi_receive(FAR struct netdev_lowerhalf_s *dev)
{
  UNUSED(dev);

  return NULL;
}

/****************************************************************************
 * Name: bk7258_wifi_connect / bk7258_wifi_disconnect
 ****************************************************************************/

static int bk7258_wifi_connect(FAR struct netdev_lowerhalf_s *dev)
{
  FAR struct bk7258_wifi_dev_s *priv = (FAR struct bk7258_wifi_dev_s *)dev;

  ninfo("connect: ssid '%.*s' auth %" PRIu32 " mode %" PRIu32 "\n",
        priv->ssid_len, priv->ssid, priv->auth, priv->mode);

  return -ENOSYS;
}

static int bk7258_wifi_disconnect(FAR struct netdev_lowerhalf_s *dev)
{
  FAR struct bk7258_wifi_dev_s *priv = (FAR struct bk7258_wifi_dev_s *)dev;

  priv->connected = false;
  netdev_lower_carrier_off(dev);
  return OK;
}

/****************************************************************************
 * Name: bk7258_wifi_essid
 *
 * Description:
 *   Store or report the SSID.  This one is real: the parameter setters can
 *   be finished without the vendor stack, and having them work means the
 *   wapi command line can be exercised against the driver now.
 *
 ****************************************************************************/

static int bk7258_wifi_essid(FAR struct netdev_lowerhalf_s *dev,
                             FAR struct iwreq *iwr, bool set)
{
  FAR struct bk7258_wifi_dev_s *priv = (FAR struct bk7258_wifi_dev_s *)dev;
  FAR struct iw_point *essid = &iwr->u.essid;

  if (set)
    {
      if (essid->length > sizeof(priv->ssid) - 1)
        {
          return -EINVAL;
        }

      memcpy(priv->ssid, essid->pointer, essid->length);
      priv->ssid[essid->length] = '\0';
      priv->ssid_len = essid->length;
    }
  else
    {
      essid->length = priv->ssid_len;
      memcpy(essid->pointer, priv->ssid, priv->ssid_len);
      essid->flags = priv->connected ? IW_ESSID_ON : IW_ESSID_OFF;
    }

  return OK;
}

/****************************************************************************
 * Name: bk7258_wifi_bssid
 ****************************************************************************/

static int bk7258_wifi_bssid(FAR struct netdev_lowerhalf_s *dev,
                             FAR struct iwreq *iwr, bool set)
{
  FAR struct bk7258_wifi_dev_s *priv = (FAR struct bk7258_wifi_dev_s *)dev;

  if (set)
    {
      memcpy(priv->bssid, iwr->u.ap_addr.sa_data, IFHWADDRLEN);
      priv->bssid_set = true;
    }
  else
    {
      memcpy(iwr->u.ap_addr.sa_data, priv->bssid, IFHWADDRLEN);
    }

  return OK;
}

/****************************************************************************
 * Name: bk7258_wifi_passwd
 ****************************************************************************/

static int bk7258_wifi_passwd(FAR struct netdev_lowerhalf_s *dev,
                              FAR struct iwreq *iwr, bool set)
{
  FAR struct bk7258_wifi_dev_s *priv = (FAR struct bk7258_wifi_dev_s *)dev;
  FAR struct iw_point *data = &iwr->u.data;

  /* Only ever accepted, never reported: handing a key back out to any
   * caller that asks is not something this driver should do.
   */

  if (!set)
    {
      return -ENOTSUP;
    }

  if (data->length > sizeof(priv->passwd) - 1)
    {
      return -EINVAL;
    }

  memcpy(priv->passwd, data->pointer, data->length);
  priv->passwd[data->length] = '\0';
  return OK;
}

/****************************************************************************
 * Name: bk7258_wifi_mode / bk7258_wifi_auth
 ****************************************************************************/

static int bk7258_wifi_mode(FAR struct netdev_lowerhalf_s *dev,
                            FAR struct iwreq *iwr, bool set)
{
  FAR struct bk7258_wifi_dev_s *priv = (FAR struct bk7258_wifi_dev_s *)dev;

  if (set)
    {
      if (iwr->u.mode != IW_MODE_INFRA && iwr->u.mode != IW_MODE_MASTER)
        {
          return -ENOSYS;
        }

      priv->mode = iwr->u.mode;
    }
  else
    {
      iwr->u.mode = priv->mode;
    }

  return OK;
}

static int bk7258_wifi_auth(FAR struct netdev_lowerhalf_s *dev,
                            FAR struct iwreq *iwr, bool set)
{
  FAR struct bk7258_wifi_dev_s *priv = (FAR struct bk7258_wifi_dev_s *)dev;

  if (set)
    {
      priv->auth = iwr->u.param.value;
    }
  else
    {
      iwr->u.param.value = priv->auth;
    }

  return OK;
}

/****************************************************************************
 * Name: the radio parameters
 *
 * Description:
 *   Frequency, bit rate, transmit power, country and sensitivity all read
 *   or write MAC/PHY state that only the vendor stack can reach.
 *
 ****************************************************************************/

static int bk7258_wifi_freq(FAR struct netdev_lowerhalf_s *dev,
                            FAR struct iwreq *iwr, bool set)
{
  UNUSED(dev);
  UNUSED(iwr);
  UNUSED(set);
  return -ENOSYS;
}

static int bk7258_wifi_bitrate(FAR struct netdev_lowerhalf_s *dev,
                               FAR struct iwreq *iwr, bool set)
{
  UNUSED(dev);
  UNUSED(iwr);
  UNUSED(set);
  return -ENOSYS;
}

static int bk7258_wifi_txpower(FAR struct netdev_lowerhalf_s *dev,
                               FAR struct iwreq *iwr, bool set)
{
  UNUSED(dev);
  UNUSED(iwr);
  UNUSED(set);
  return -ENOSYS;
}

static int bk7258_wifi_country(FAR struct netdev_lowerhalf_s *dev,
                               FAR struct iwreq *iwr, bool set)
{
  UNUSED(dev);
  UNUSED(iwr);
  UNUSED(set);
  return -ENOSYS;
}

static int bk7258_wifi_sensitivity(FAR struct netdev_lowerhalf_s *dev,
                                   FAR struct iwreq *iwr, bool set)
{
  UNUSED(dev);
  UNUSED(iwr);
  UNUSED(set);
  return -ENOSYS;
}

/****************************************************************************
 * Name: bk7258_wifi_scan
 *
 * Description:
 *   SIOCSIWSCAN (set == true) starts a scan; SIOCGIWSCAN (set == false)
 *   returns what the last one found, as the stream of variable-length
 *   struct iw_event records the wireless extensions define.  One AP is four
 *   records: BSSID, ESSID, frequency (as a channel), and quality.
 *
 *   -EAGAIN is the documented answer to a GET issued before the results are
 *   in; wapi and iwlist both retry on it.  There is no completion callback
 *   to hang that on -- the vendor's is a wpa_ctrl_event() this port stubs
 *   out -- so "in" means the MAC has reported at least one AP, and the
 *   caller decides how long to keep asking.
 *
 ****************************************************************************/

static int bk7258_wifi_scan(FAR struct netdev_lowerhalf_s *dev,
                            FAR struct iwreq *iwr, bool set)
{
  FAR struct iw_event *iwe;
  struct bk7258_scan_ap_s ap;
  FAR char *buf;
  size_t used = 0;
  size_t len;
  int count;
  int i;

  UNUSED(dev);

  if (set)
    {
      int ret = bk7258_wifi_scan_start();

      syslog(LOG_INFO, "wifi: scan start -> %d\n", ret);
      return ret == 0 ? OK : -EIO;
    }

  if (iwr == NULL || iwr->u.data.pointer == NULL)
    {
      return -EINVAL;
    }

  count = bk7258_wifi_scan_count();

  /* Report the interrupt counters here, not at scan start.  At start they
   * are trivially zero; what matters is whether the MAC took a single
   * RX-trigger interrupt while the radio was walking the channels.  wapi
   * polls this path, so once is enough -- rate-limit it.
   */

    {
      extern void bk7258_wifi_irq_report(void);
      static int reported = 0;

      if (reported++ == 0)
        {
          bk7258_wifi_irq_report();
        }
    }

  syslog(LOG_INFO, "wifi: scan results -> %d\n", count);
  if (count <= 0)
    {
      return -EAGAIN;
    }

  /* One reference for the whole enumeration.  Taking and dropping it per
   * item frees the vendor's result set on the first drop -- see
   * bk7258_wifi_scan_acquire() in bk7258_wifi_glue.c.
   */

  count = bk7258_wifi_scan_acquire();
  if (count <= 0)
    {
      bk7258_wifi_scan_release();
      return -EAGAIN;
    }

  /* Size the answer first.  wapi asks twice: wapi_scan_stat() probes with a
   * one-byte buffer purely to learn whether results exist, and treats -E2BIG
   * as "ready" and -EAGAIN as "not yet"; wapi_scan_coll() then doubles its
   * buffer and retries for as long as it keeps getting -E2BIG
   * (apps/wireless/wapi/src/wireless.c:1289 and :1371).  Returning OK with a
   * truncated stream instead, as this used to, tells the probe that a
   * one-byte buffer was enough and loses every result.
   */

  len = 0;
  for (i = 0; i < count; i++)
    {
      if (bk7258_wifi_scan_get(i, &ap) != 0)
        {
          continue;
        }

      len += IW_EV_LEN(ap_addr) + IW_EV_LEN(freq) + IW_EV_LEN(qual) +
             IW_EV_LEN(essid) + ((strnlen(ap.ssid, 32) + 3) & ~3);
    }

  if (iwr->u.data.length < len)
    {
      iwr->u.data.length = len;
      bk7258_wifi_scan_release();
      return -E2BIG;
    }

  buf = (FAR char *)iwr->u.data.pointer;

  for (i = 0; i < count; i++)
    {
      if (bk7258_wifi_scan_get(i, &ap) != 0)
        {
          continue;
        }

      iwe = (FAR struct iw_event *)&buf[used];
      iwe->len = IW_EV_LEN(ap_addr);
      iwe->cmd = SIOCGIWAP;
      iwe->u.ap_addr.sa_family = ARPHRD_ETHER;
      memcpy(iwe->u.ap_addr.sa_data, ap.bssid, IFHWADDRLEN);
      used += iwe->len;

      /* The SSID travels inline, immediately after the iw_point, and
       * u.essid.pointer carries the offset to it rather than an address:
       * wapi_event_stream_extract() computes the real pointer as
       * "current + offsetof(struct iw_event, u) + (unsigned long)pointer"
       * (wireless.c:296).  This used to store &ap.ssid, the address of a
       * stack local that is reused every iteration and gone by the time the
       * caller looks -- so wapi added a stack address to its own buffer base
       * and read from somewhere arbitrary.  Same encoding as the in-tree
       * bcm43xxx driver (bcmf_driver.c:1065).
       */

      iwe = (FAR struct iw_event *)&buf[used];
      iwe->cmd = SIOCGIWESSID;
      iwe->u.essid.flags   = 1;
      iwe->u.essid.length  = strnlen(ap.ssid, 32);
      iwe->u.essid.pointer = (FAR void *)sizeof(iwe->u.essid);
      memcpy(&iwe->u.essid + 1, ap.ssid, iwe->u.essid.length);
      iwe->len = IW_EV_LEN(essid) + ((iwe->u.essid.length + 3) & ~3);
      used += iwe->len;

      iwe = (FAR struct iw_event *)&buf[used];
      iwe->len = IW_EV_LEN(freq);
      iwe->cmd = SIOCGIWFREQ;
      iwe->u.freq.m = ap.channel;
      iwe->u.freq.e = 0;
      iwe->u.freq.i = 0;
      used += iwe->len;

      iwe = (FAR struct iw_event *)&buf[used];
      iwe->len = IW_EV_LEN(qual);
      iwe->cmd = IWEVQUAL;
      iwe->u.qual.qual    = 0;
      iwe->u.qual.level   = ap.rssi;
      iwe->u.qual.noise   = 0;
      iwe->u.qual.updated = IW_QUAL_DBM;
      used += iwe->len;
    }

  bk7258_wifi_scan_release();

  iwr->u.data.length = used;
  return OK;
}

static int bk7258_wifi_range(FAR struct netdev_lowerhalf_s *dev,
                             FAR struct iwreq *iwr)
{
  UNUSED(dev);
  UNUSED(iwr);
  return -ENOSYS;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_wifi_initialize
 *
 * Description:
 *   See bk7258_wifi.h.
 *
 ****************************************************************************/

int bk7258_wifi_initialize(void)
{
  FAR struct bk7258_wifi_dev_s *priv = &g_bk7258_wifi;
  int ret;

  memset(priv, 0, sizeof(*priv));

  priv->dev.ops    = &g_bk7258_wifi_ops;
  priv->dev.iw_ops = &g_bk7258_wifi_iw_ops;
  priv->mode       = IW_MODE_INFRA;

  /* The MAC address stays zero until the vendor stack can read it out of the
   * chip.  Leaving it zero is deliberate: an invented address would be a
   * plausible-looking lie, and this interface cannot pass traffic yet
   * anyway.
   */

  priv->dev.quota[NETPKT_TX] = BK7258_WIFI_TX_QUOTA;
  priv->dev.quota[NETPKT_RX] = BK7258_WIFI_RX_QUOTA;

  ret = netdev_lower_register(&priv->dev, NET_LL_IEEE80211);
  if (ret < 0)
    {
      nerr("ERROR: netdev_lower_register: %d\n", ret);
      return ret;
    }

  ninfo("wlan registered; MAC not built, see PORTING_NOTES ch.21\n");
  return OK;
}
