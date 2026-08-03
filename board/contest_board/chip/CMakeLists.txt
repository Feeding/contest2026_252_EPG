# ##############################################################################
# board/contest_board/chip/CMakeLists.txt
#
# BK7258 chip layer for openvela / NuttX.
#
# Licensed to the Apache Software Foundation (ASF) under one or more contributor
# license agreements.  See the NOTICE file distributed with this work for
# additional information regarding copyright ownership.  The ASF licenses this
# file to you under the Apache License, Version 2.0 (the "License"); you may not
# use this file except in compliance with the License.  You may obtain a copy of
# the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
# License for the specific language governing permissions and limitations under
# the License.
#
# ##############################################################################

set(SRCS bk7258_start.c bk7258_irq.c bk7258_lowputc.c bk7258_allocateheap.c
         bk7258_gpio.c bk7258_clockconfig.c bk7258_timerisr.c
    bk7258_idle.c bk7258_wdt.c bk7258_psram.c bk7258_pwm.c bk7258_audio.c)

if(CONFIG_BK7258_RTC)
  list(APPEND SRCS bk7258_rtc.c)
endif()

if(CONFIG_BK7258_WDT)
  list(APPEND SRCS bk7258_wdt_lowerhalf.c)
endif()

if(CONFIG_MMCSD)
  list(APPEND SRCS bk7258_sdio.c)
endif()

if(CONFIG_I2C_BITBANG)
  list(APPEND SRCS bk7258_camera.c)
endif()

if(CONFIG_CONTEST_FACE)
  list(APPEND SRCS bk7258_jpegdec.c bk7258_dma2d.c)
endif()

if(CONFIG_BK7258_I2C1)
  list(APPEND SRCS bk7258_i2c.c)
endif()

if(CONFIG_BK7258_SPI1)
  list(APPEND SRCS bk7258_spi.c bk7258_qspi.c)
endif()

if(CONFIG_BK7258_UART0 OR CONFIG_BK7258_UART1 OR CONFIG_BK7258_UART2)
  list(APPEND SRCS bk7258_serial.c)
endif()

# Closed-source BLE controller + RF PHY archives from the BK7258 CP side of
# Beken AVDK (ABI: armv8-m.main / fpv5-sp-d16 / hard float, verified).  This
# is the same stack generation used by projects/bluetooth/polar; unlike the
# older IDK pair it exposes the BK7258 polar RF path and its matching OSI ABI.

set(BK_AVDK_ROOT "$ENV{BK_AVDK_ROOT}")
if(NOT BK_AVDK_ROOT)
  get_filename_component(BK_AVDK_ROOT "${NUTTX_DIR}/../../bk_avdk_smp" ABSOLUTE)
endif()
set(BK_AVDK_BT_LIBS
    "${BK_AVDK_ROOT}/cp/components/bk_libs/bk7258/libs")

if(EXISTS ${BK_AVDK_BT_LIBS}/libbluetooth_controller_controller_only_ble.a)
  list(APPEND SRCS bk7258_ble.c bk7258_ble_shim.c bk7258_vnd_cal.c
                   bk7258_bt_osi.c bk7258_phy_osi.c bk7258_saradc.c)
  # This port drives the controller directly through its HCI boundary and
  # deliberately does not initialize Beken's GAP/GATT host.  Use the archive
  # built for that exact CONFIG_BTDM_CONTROLLER_ONLY arrangement; pairing the
  # regular BLE controller with an uninitialized host leaves its HCI event
  # path in a mixed configuration.
  nuttx_add_extra_library(
    ${BK_AVDK_BT_LIBS}/libbluetooth_controller_controller_only_ble.a)
  # The no-Wi-Fi PHY paired with the controller-only BLE archive by AVDK.
  nuttx_add_extra_library(${BK_AVDK_BT_LIBS}/libcom_phy.a)
else()
  message(FATAL_ERROR
    "BK7258 BLE libraries not found under ${BK_AVDK_BT_LIBS}; "
    "set BK_AVDK_ROOT to the bk_avdk_smp checkout")
endif()

target_sources(arch PRIVATE ${SRCS})
