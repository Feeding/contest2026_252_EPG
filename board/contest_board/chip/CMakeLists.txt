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

# Closed-source BLE controller/host + RF PHY archives (Beken bk_idk on the
# build host; ABI: armv8-m.main / fpv5-sp-d16 / hard float, verified).  The
# BLE staging compiles only when the SDK checkout is present.

set(BK_IDK_LIBS /Users/apple/app/github.com/bk_idk/components/bk_libs/bk7258/libs)

if(EXISTS ${BK_IDK_LIBS}/libbluetooth_controller_ble.a)
  list(APPEND SRCS bk7258_ble.c bk7258_ble_shim.c bk7258_vnd_cal.c
                   bk7258_bt_osi.c)
  nuttx_add_extra_library(${BK_IDK_LIBS}/libbluetooth_controller_ble.a)
  nuttx_add_extra_library(${BK_IDK_LIBS}/libbluetooth_host_ble.a)
  nuttx_add_extra_library(${BK_IDK_LIBS}/libbk_phy.a)
  nuttx_add_extra_library(${BK_IDK_LIBS}/libcom_phy.a)
endif()

target_sources(arch PRIVATE ${SRCS})
