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
