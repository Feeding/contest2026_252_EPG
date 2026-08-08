/****************************************************************************
 * board/contest_board/src/rxsens.c
 *
 * Beken's own receiver-sensitivity test, exposed to NSH.
 *
 * The vendor registers this as the "rxsens" console command from bk_cli,
 * which this port does not build -- so the test sat in the image
 * (do_rx_sensitivity is linked) with no way to reach it.  This is the
 * missing three lines.
 *
 * It exists because the receiver currently reports zero frames on all
 * thirteen scan channels and takes no RX-trigger interrupt at all, and the
 * question "does the radio hear anything" deserves a direct measurement
 * rather than more disassembly of closed code.
 *
 * Usage, from the library's own help string:
 *
 *   rxsens [-b bandwidth]  0:20M (default)  1:40M
 *          [-c channel]    wifi 1..14, ble 2400..2527
 *          [-d duration]   0: stop timer, d: start timer with d interval
 *
 * Requires the vendor MAC to be up -- run "ifup wlan0" first, or let
 * netinit do it.  Its output comes from the closed PHY library and is
 * gated by _bk_feature_phy_log_enable in bk7258_phy_osi.c.
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

#include <stdio.h>

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

/* components/bk_wifi/src/phy.c, compiled into this image with the vendor's
 * flags.  Declared rather than included: its header pulls in the vendor
 * include path, which this file is not compiled against.
 */

extern void rx_sens_cmd_test(char *buf, int len, int argc, char **argv);

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  if (argc < 2)
    {
      printf("rxsens [-b bandwidth]  0:20M (default)  1:40M\n"
             "       [-c channel]    wifi 1..14, ble 2400..2527\n"
             "       [-d duration]   0: stop timer, d: start with interval\n"
             "\n"
             "Bring the interface up first: ifup wlan0\n");
      return 1;
    }

  /* The vendor parses argv itself, U-Boot style, and expects argv[0] to be
   * the command name -- which is exactly what NSH hands us.
   */

  rx_sens_cmd_test(NULL, 0, argc, argv);
  return 0;
}
