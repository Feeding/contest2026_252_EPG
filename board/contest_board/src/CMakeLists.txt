# ##############################################################################
# board/contest_board/src/CMakeLists.txt
#
# The "board" library target is created by nuttx/boards; we only add sources
# to it.
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

set(SRCS bk7258_boardinit.c)

if(CONFIG_BOARDCTL)
  list(APPEND SRCS bk7258_appinit.c)
endif()

if(CONFIG_USERLED)
  list(APPEND SRCS bk7258_userleds.c)
endif()

if(CONFIG_ARCH_BUTTONS)
  list(APPEND SRCS bk7258_buttons.c)
endif()

if(CONFIG_DEV_GPIO)
  list(APPEND SRCS bk7258_gpiodev.c)
endif()

if(CONFIG_I2C_BITBANG)
  list(APPEND SRCS bk7258_i2c_bitbang.c)
endif()

if(CONFIG_LCD)
  list(APPEND SRCS bk7258_gc9d01.c bk7258_swspi.c)
endif()

target_sources(board PRIVATE ${SRCS})

target_include_directories(board PRIVATE ${NUTTX_CHIP_ABS_DIR})
