/****************************************************************************
 * board/contest_board/chip/bk7258_wifi.c
 *
 * WiFi network device for the BK7258 -- the openvela-facing half.
 *
 * This is deliberately only one half of the driver, and it is worth being
 * explicit about why, because the file does not work yet and should not be
 * mistaken for something that does.
 *
 * The vendor stack cannot be compiled from any published SDK.  Two internal
 * headers, sm_task.h and ps.h, are absent from bekencorp/bk_idk (v2.0.1) and
 * bekencorp/bk_avdk_smp (v3.1.1) alike, and have never appeared in either
 * repository's history; the definition of struct vif_info_tag lives in one of
 * them and four files in components/bk_wifi/src index vif_info_tab, so they
 * cannot build without it.  Beken's own published tree therefore cannot build
 * its own WiFi component.  PORTING_NOTES chapter 21 has the measurements and
 * the list to ask them for.
 *
 * What that leaves is the half openvela specifies, which is small, well
 * documented (docs/zh-cn/device_dev_guide/connection/network/driver/
 * net_driver_guide.md) and has a complete worked example in
 * nuttx/drivers/net/wifi_sim.c.  It is written now so that when the headers
 * arrive the remaining work is confined to the vendor calls behind
 * bk7258_wifi_lower_*, rather than starting from nothing.
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

#include <net/if.h>
#include <nuttx/kmalloc.h>
#include <nuttx/net/net.h>
#include <nuttx/net/netdev_lowerhalf.h>
#include <nuttx/wireless/wireless.h>

#include "bk7258_wifi.h"

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

  nerr("ERROR: WiFi MAC is not built -- see PORTING_NOTES ch.21\n");
  return -ENOSYS;
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

static int bk7258_wifi_scan(FAR struct netdev_lowerhalf_s *dev,
                            FAR struct iwreq *iwr, bool set)
{
  UNUSED(dev);
  UNUSED(iwr);
  UNUSED(set);
  return -ENOSYS;
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
