/****************************************************************************
 * board/contest_board/chip/bk7258_wifi_scan.h
 *
 * The flat scan-result shape shared between the two halves of the WiFi
 * driver.  bk7258_wifi_glue.c fills it with the vendor's headers on the
 * include path; bk7258_wifi.c reads it with NuttX's.  Neither side can
 * include the other's headers, so this file is the whole contract.
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

#ifndef __BOARD_CONTEST_BOARD_CHIP_BK7258_WIFI_SCAN_H
#define __BOARD_CONTEST_BOARD_CHIP_BK7258_WIFI_SCAN_H

#include <stdint.h>

struct bk7258_scan_ap_s
{
  uint8_t bssid[6];
  char    ssid[33];      /* NUL-terminated; the vendor's is 32 unterminated */
  uint8_t channel;
  int32_t rssi;
  uint16_t caps;         /* 802.11 capability field; bit 4 is Privacy */
};

/* Kick off a scan.  Returns 0 once the request has been accepted by the
 * MAC, which is not the same as finished -- results arrive asynchronously.
 */

int bk7258_wifi_scan_start(void);

/* How many APs the last completed scan found. */

int bk7258_wifi_scan_count(void);

/* Copy entry 'index' out.  Returns 0 on success, -1 if it does not exist. */

int bk7258_wifi_scan_get(int index, struct bk7258_scan_ap_s *ap);

#endif /* __BOARD_CONTEST_BOARD_CHIP_BK7258_WIFI_SCAN_H */
