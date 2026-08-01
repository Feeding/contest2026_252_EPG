/****************************************************************************
 * board/contest_board/include/board.h
 *
 * Agora ConvoAI Kit R1 (Beken BK7258).
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

#ifndef __BOARD_CONTEST_BOARD_INCLUDE_BOARD_H
#define __BOARD_CONTEST_BOARD_INCLUDE_BOARD_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Clocking *****************************************************************/

/* The BK7258 runs from a 26 MHz crystal (XTALH).  The bootloader has already
 * locked the DPLL and switched the core over by the time NuttX starts; the
 * core runs at 120 MHz in the bootloader's default configuration.
 */

#define BOARD_XTAL_FREQUENCY    26000000
#define BOARD_CPU_FREQUENCY     120000000

/* up_udelay() calibration.  This is a starting estimate for a 120 MHz core;
 * refine it with apps/examples/calib_udelay if precise delays are needed.
 */

#define BOARD_LOOPSPERMSEC      12000

/* UART pin assignment ******************************************************/

/* UART0 (GPIO10 = RX, GPIO11 = TX) is the console.  These are the pads the
 * boot ROM uses for its flash-download protocol, and on this board they are
 * wired to the CH340 USB-serial bridge behind the USB-C connector.
 */

#define BOARD_CONSOLE_UART      0

/* LEDs **********************************************************************/

/* The bi-colour LED, straight from the vendor firmware for this exact board
 * (led_blink.h): red on GPIO40, green on GPIO41, push-pull, driven high to
 * light.  The power-on default mux table calls these pins LCD_G4/G3; that is
 * template residue -- this board runs twin SPI panels and the vendor's own
 * LED driver reclaims the pads at runtime, as does this port's GPIO layer.
 */

#define BOARD_LED_RED       0
#define BOARD_LED_GREEN     1
#define BOARD_NLEDS         2

/* Buttons *******************************************************************/

/* Three side keys, active low with the internal pull-up, mapped by the
 * vendor's key table (key_app_config.h) and the board manual: S1 volume-up
 * on GPIO13, S2 power on GPIO12 (also the deep-sleep wake source), S3
 * volume-down on GPIO8.
 */

#define BUTTON_S1           0
#define BUTTON_S2           1
#define BUTTON_S3           2
#define NUM_BUTTONS         3

#define BUTTON_S1_BIT       (1 << BUTTON_S1)
#define BUTTON_S2_BIT       (1 << BUTTON_S2)
#define BUTTON_S3_BIT       (1 << BUTTON_S3)

#endif /* __BOARD_CONTEST_BOARD_INCLUDE_BOARD_H */
