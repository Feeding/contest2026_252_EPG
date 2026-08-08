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
 * RUN IT IN THE BACKGROUND:  rxsens -c 6 -d 2000 &
 *
 * It does not return.  Run in the foreground it occupies NSH, and since it
 * prints nothing after "[RS]reset_mm" the console looks dead -- which is
 * how this was first, wrongly, written up as "hangs the board".  It does
 * not: with "&" the shell stays responsive and ps shows the task alive.
 *
 * What ps actually shows is the useful part.  The task sits in state Ready,
 * not Waiting, with its stack usage identical across samples minutes apart:
 * a tight spin at a fixed call depth, not a blocked wait.  Interrupts are
 * never masked either -- a traced run recorded zero rtos_disable_int calls.
 * So the test is polling for something that never arrives.
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

#include <stdbool.h>
#include <stdio.h>

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

/* components/bk_wifi/src/phy.c, compiled into this image with the vendor's
 * flags.  Declared rather than included: its header pulls in the vendor
 * include path, which this file is not compiled against.
 */

extern void rx_sens_cmd_test(char *buf, int len, int argc, char **argv);

/* libwifi.a.  Clears bit 13 of 0x49000004, the MAC core clock gate.
 *
 * Necessary but NOT sufficient -- read the second half of this note before
 * concluding anything from it.
 *
 * The command reaches the MAC outside the core thread:
 *
 *   do_rx_sensitivity -> rs_test -> [g_phy_funcs_t+24] -> rs_init
 *                     -> hal_machw_stop
 *
 * and hal_machw_stop sets the MAC soft-reset bit and polls it until the
 * hardware clears it.  rwnx_intf_init() calls rwnxl_sleep() at the tail of
 * wifi_init(), which gates that clock off; core_thread_main re-opens it for
 * every message it pops, but the NSH task never goes through core_thread_main,
 * so on this path the clock stayed gated.  Measured with bkreg while the task
 * spun: core_clk=GATED and soft_reset=1 at the same moment.
 *
 * Calling this first is the vendor's own idiom rather than an invention --
 * dbg_assert_err and dbg_wifi_assert_handler are its only other callers, for
 * exactly this reason: they need the MAC reachable from whatever context the
 * assert fired in.
 *
 * What it does NOT do is fix the hang, and the earlier claim in this tree
 * that the gated clock was the root cause is withdrawn.  With this call in
 * place bkreg now reports core_clk=running -- and soft_reset is still 1, with
 * the task still spinning in the same poll.  So the gate was genuinely shut
 * and is now genuinely open, and the reset still does not complete.
 *
 * The standing suspicion is the modem: at the moment of the measurement
 * phy_cken was 0 and wifp_phy was OFF, and the MAC's reset may need the
 * MAC-PHY interface clock to retire.  Do not test that by racing a scan
 * against rxsens from the console -- that was tried and it reset the board.
 * Note also that the vendor's own rxsens is an ATE-mode tool: rwnx_intf_init
 * only calls rwnxl_sleep() on the !ate_is_enabled() branch, and this port's
 * ate_is_enabled() returns false, so the stack puts itself to sleep in a way
 * the vendor's test build never does.
 */

extern void hal_machw_enable_maccore_clk(void);

/* Temporary: arms the critical-section trace in bk7258_ble_shim.c while the
 * rxsens hang is being located.
 */

extern void bk7258_int_trace(bool on);

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

  hal_machw_enable_maccore_clk();

  bk7258_int_trace(true);
  rx_sens_cmd_test(NULL, 0, argc, argv);
  bk7258_int_trace(false);
  return 0;
}
