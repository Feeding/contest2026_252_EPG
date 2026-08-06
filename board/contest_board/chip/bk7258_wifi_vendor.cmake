# ############################################################################
# board/contest_board/chip/bk7258_wifi_vendor.cmake
#
# Compile Beken's WiFi driver sources into this port.
#
# The flags below are not invented.  They were taken verbatim from the
# vendor's own build of projects/wifi/sta_connect -- run their CMake, ask
# ninja for the command line it uses on a bk_wifi source, and keep the
# include paths, defines and machine options from it.  Guessing them does
# not work: the sources need -DBK_MAC=1 before rwnx_version.h will define
# NX_VERSION_PACK, -DBK_SUPPLICANT=1, and 113 include directories in an
# order that resolves several same-named headers to the bk7258 copy rather
# than bk7234's.  Reproduce with:
#
#   cd $BK_IDK && make bk7258 PROJECT=wifi/sta_connect
#   ninja -C build/sta_connect/bk7258 -t commands | grep bk_wifi.*rwnx_tx
#
# Note the machine options differ from this port's: the vendor builds with
# -mcpu=cortex-m33+nodsp and -mcmse.  They are applied per source rather
# than globally for that reason.
#
# ############################################################################

if(NOT DEFINED BK_IDK_ROOT)
  set(BK_IDK_ROOT /Users/apple/app/github.com/bk_idk)
endif()

set(BK_WIFI_SRC ${BK_IDK_ROOT}/components/bk_wifi/src)

set(BK_WIFI_SOURCES
    ${BK_WIFI_SRC}/bk_wifi_adapter.c
    ${BK_WIFI_SRC}/bk_workqueue.c
    ${BK_WIFI_SRC}/hostapd_intf.c
    ${BK_WIFI_SRC}/lsig_monitor.c
    ${BK_WIFI_SRC}/net_param.c
    ${BK_WIFI_SRC}/pbuf.c
    ${BK_WIFI_SRC}/phy.c
    ${BK_WIFI_SRC}/rw_ieee80211.c
    ${BK_WIFI_SRC}/rw_msdu.c
    ${BK_WIFI_SRC}/rw_msg_rx.c
    ${BK_WIFI_SRC}/rw_msg_tx.c
    ${BK_WIFI_SRC}/rw_task.c
    ${BK_WIFI_SRC}/rw_tx_buffering.c
    ${BK_WIFI_SRC}/rwm_proto.c
    ${BK_WIFI_SRC}/rwnx_misc.c
    ${BK_WIFI_SRC}/rwnx_params.c
    ${BK_WIFI_SRC}/rwnx_rx.c
    ${BK_WIFI_SRC}/rwnx_td.c
    ${BK_WIFI_SRC}/rwnx_tx.c
    ${BK_WIFI_SRC}/rwnx_txq.c
    ${BK_WIFI_SRC}/rwnx_utils.c
    ${BK_WIFI_SRC}/sa_ap.c
    ${BK_WIFI_SRC}/sa_station.c
    ${BK_WIFI_SRC}/skbuff.c
    ${BK_WIFI_SRC}/soft_encrypt.c
    ${BK_WIFI_SRC}/wifi_api.c
    ${BK_WIFI_SRC}/wifi_config.c
    ${BK_WIFI_SRC}/wifi_frame.c
    ${BK_WIFI_SRC}/wifi_init.c
    ${BK_WIFI_SRC}/wifi_netif.c
    ${BK_WIFI_SRC}/wifi_v2.c
    ${BK_WIFI_SRC}/wifi_wpa_cmd.c

    # Outside bk_wifi/src, but the same stack: the thirteen bk_feature_*
    # predicates the adapter consults are all here, and the file is 165
    # lines with two includes.  Compiling it beats reimplementing thirteen
    # policy answers we would have to keep in step with Beken's.
    ${BK_IDK_ROOT}/components/bk_common/bk_feature.c

    # Ours, not Beken's -- but compiled here on purpose.  The vendor sources
    # include lwIP 2.1.2's "pbuf.h" unguarded (rwnx_rx.c:16) and the include
    # list above puts it on the path, so struct pbuf is lwIP's in every
    # object here.  Building our allocator with the same flags is what makes
    # its idea of that struct identical rather than merely similar; the
    # struct grows trailing fields under PBUF_LIFETIME_DBG, so a mismatch
    # would be silent.  Beken's own src/pbuf.c is inert (guarded behind
    # CONFIG_FULLY_HOSTED/SEMI_HOSTED) and stale (writes p->total_len where
    # the rest of the tree reads p->tot_len), so it cannot be used.
    ${CMAKE_CURRENT_LIST_DIR}/bk7258_wifi_pbuf.c

    # Also ours: the calls that need the vendor's own headers to get their
    # argument types right.  See its header for what went wrong when they
    # were declared by hand instead.
    ${CMAKE_CURRENT_LIST_DIR}/bk7258_wifi_glue.c
)

set(BK_WIFI_INCLUDES
    -I${BK_IDK_ROOT}/build/sta_connect/bk7258/config
    -I${BK_IDK_ROOT}/components/bk_wifi/include
    -I${BK_IDK_ROOT}/components/bk_wifi/include/bk_private
    -I${BK_IDK_ROOT}/components/at_server
    -I${BK_IDK_ROOT}/components/at_server/_at_server
    -I${BK_IDK_ROOT}/components/at_server/_at_server_port
    -I${BK_IDK_ROOT}/components/bk_net/include
    -I${BK_IDK_ROOT}/components/bk_event
    -I${BK_IDK_ROOT}/include/modules
    -I${BK_IDK_ROOT}/include
    -I${BK_IDK_ROOT}/include/modules/securityip
    -I${BK_IDK_ROOT}/include/modules/freetype
    -I${BK_IDK_ROOT}/components/bk_common/include
    -I${BK_IDK_ROOT}/middleware/soc/common/soc/include
    -I${BK_IDK_ROOT}/middleware/soc/common/hal/include
    -I${BK_IDK_ROOT}/components/part_table
    -I${BK_IDK_ROOT}/middleware/soc/bk7258/soc
    -I${BK_IDK_ROOT}/middleware/soc/bk7258/hal
    -I${BK_IDK_ROOT}/middleware/soc/bk7258
    -I${BK_IDK_ROOT}/projects/wifi/sta_connect/config/bk7258
    -I${BK_IDK_ROOT}/middleware/boards/bk7258/partitions
    -I${BK_IDK_ROOT}/build/sta_connect/bk7258/armino/partitions/_build
    -I${BK_IDK_ROOT}/components/os_source/freertos_v10/include
    -I${BK_IDK_ROOT}/components/os_source/freertos_v10/portable/GCC/ARM_CM33_NTZ/non_secure
    -I${BK_IDK_ROOT}/components/release
    -I${BK_IDK_ROOT}/components/bk_startup/freertos
    -I${BK_IDK_ROOT}/components/easy_flash
    -I${BK_IDK_ROOT}/components/easy_flash/easy_flash_V4.X/inc
    -I${BK_IDK_ROOT}/components/easy_flash/easy_flash_V4.X/port
    -I${BK_IDK_ROOT}/components/base64
    -I${BK_IDK_ROOT}/components/bk_init/include
    -I${BK_IDK_ROOT}/components/app
    -I${BK_IDK_ROOT}/components/bk_netif/include
    -I${BK_IDK_ROOT}/components/bk_cli/include
    -I${BK_IDK_ROOT}/components/bk_cli/include/bk_private
    -I${BK_IDK_ROOT}/components/bk_cli
    -I${BK_IDK_ROOT}/components/utf8
    -I${BK_IDK_ROOT}/components/temp_detect
    -I${BK_IDK_ROOT}/middleware/driver
    -I${BK_IDK_ROOT}/middleware/driver/include
    -I${BK_IDK_ROOT}/middleware/driver/include/bk_private
    -I${BK_IDK_ROOT}/middleware/driver/include/bk_private/legacy
    -I${BK_IDK_ROOT}/middleware/driver/common
    -I${BK_IDK_ROOT}/middleware/driver/reset_reason
    -I${BK_IDK_ROOT}/middleware/driver/pwm
    -I${BK_IDK_ROOT}/middleware/driver/flash
    -I${BK_IDK_ROOT}/middleware/driver/uart
    -I${BK_IDK_ROOT}/middleware/driver/sys_ctrl
    -I${BK_IDK_ROOT}/middleware/driver/gpio
    -I${BK_IDK_ROOT}/middleware/driver/general_dma
    -I${BK_IDK_ROOT}/middleware/driver/icu
    -I${BK_IDK_ROOT}/middleware/driver/i2c
    -I${BK_IDK_ROOT}/middleware/driver/sdcard
    -I${BK_IDK_ROOT}/middleware/driver/saradc
    -I${BK_IDK_ROOT}/middleware/driver/pmu
    -I${BK_IDK_ROOT}/middleware/driver/mailbox
    -I${BK_IDK_ROOT}/middleware/driver/spinlock
    -I${BK_IDK_ROOT}/middleware/driver/touch
    -I${BK_IDK_ROOT}/middleware/driver/sbc
    -I${BK_IDK_ROOT}/middleware/driver/rtc
    -I${BK_IDK_ROOT}/middleware/driver/i2s
    -I${BK_IDK_ROOT}/middleware/driver/fft
    -I${BK_IDK_ROOT}/middleware/driver/chip_support
    -I${BK_IDK_ROOT}/middleware/compal/common_io/include
    -I${BK_IDK_ROOT}/middleware/driver/wdt
    -I${BK_IDK_ROOT}/middleware/driver/timer
    -I${BK_IDK_ROOT}/middleware/driver/scr
    -I${BK_IDK_ROOT}/middleware/driver/slcd
    -I${BK_IDK_ROOT}/middleware/driver/spi
    -I${BK_IDK_ROOT}/middleware/driver/jpeg_enc
    -I${BK_IDK_ROOT}/middleware/driver/bk7258
    -I${BK_IDK_ROOT}/middleware/driver/ckmn
    -I${BK_IDK_ROOT}/middleware/arch/cm33/include
    -I${BK_IDK_ROOT}/middleware/arch/cm33/os/freertos
    -I${BK_IDK_ROOT}/middleware/arch/cm33
    -I${BK_IDK_ROOT}/components/user_driver/include/bk_private
    -I${BK_IDK_ROOT}/components/bk_system/soc/bk7231n
    -I${BK_IDK_ROOT}/components/fatfs
    -I${BK_IDK_ROOT}/components/http
    -I${BK_IDK_ROOT}/projects/wifi/sta_connect/main
    -I${BK_IDK_ROOT}/components/adc_key
    -I${BK_IDK_ROOT}/components/key
    -I${BK_IDK_ROOT}/components/bk_websocket/include
    -I${BK_IDK_ROOT}/components/bk_rtos/include
    -I${BK_IDK_ROOT}/components/bk_rtos/freertos
    -I${BK_IDK_ROOT}/components/cmsis/CMSIS_5/CMSIS/Core/Include
    -I${BK_IDK_ROOT}/components/cmsis/CMSIS_5/Device/Beken/bk7236xx/Include
    -I${BK_IDK_ROOT}/components/cmsis/CMSIS_5/Device/Beken/bk7236xx/Include/Template
    -I${BK_IDK_ROOT}/components/cmsis/CMSIS_5/Device/Beken/armstar
    -I${BK_IDK_ROOT}/components/bk_ps/include
    -I${BK_IDK_ROOT}/components/wpa_supplicant-2.10/include/bk_private
    -I${BK_IDK_ROOT}/components/wpa_supplicant-2.10/hostapd
    -I${BK_IDK_ROOT}/components/wpa_supplicant-2.10/src/utils
    -I${BK_IDK_ROOT}/components/wpa_supplicant-2.10/src/ap
    -I${BK_IDK_ROOT}/components/wpa_supplicant-2.10/src/common
    -I${BK_IDK_ROOT}/components/wpa_supplicant-2.10/src/drivers
    -I${BK_IDK_ROOT}/components/wpa_supplicant-2.10/src
    -I${BK_IDK_ROOT}/components/wpa_supplicant-2.10/wpa_supplicant
    -I${BK_IDK_ROOT}/components/wpa_supplicant-2.10/bk_patch
    # bk_phy_adapter.c pulls in bluetooth_internal.h under CONFIG_BLUETOOTH,
    # which this build has on for the BLE stack.
    -I${BK_IDK_ROOT}/components/bk_bluetooth/include/private
    -I${BK_IDK_ROOT}/components/bk_phy/include
    -I${BK_IDK_ROOT}/components/lwip_intf_v2_1/lwip-2.1.2/src
    -I${BK_IDK_ROOT}/components/lwip_intf_v2_1/lwip-2.1.2/port
    -I${BK_IDK_ROOT}/components/lwip_intf_v2_1/lwip-2.1.2/src/include
    -I${BK_IDK_ROOT}/components/lwip_intf_v2_1/lwip-2.1.2/src/include/netif
    -I${BK_IDK_ROOT}/components/lwip_intf_v2_1/lwip-2.1.2/src/include/lwip
    -I${BK_IDK_ROOT}/components/lwip_intf_v2_1/lwip-2.1.2/src/include/lwip/priv
    -I${BK_IDK_ROOT}/components/mbedtls/mbedtls-port/inc
    -I${BK_IDK_ROOT}/components/mbedtls/mbedtls/include
    -I${BK_IDK_ROOT}/components/mbedtls/mbedtls/include/mbedtls
    -I${BK_IDK_ROOT}/components/mbedtls/mbedtls_ui
    -I${BK_IDK_ROOT}/components/wolfssl
    -I${BK_IDK_ROOT}/components/wolfssl/utils
    -I${BK_IDK_ROOT}/components/bk_cli/uart_debug
    -I${BK_IDK_ROOT}/components/bk_phy/include
)

set(BK_WIFI_DEFINES
    -DWIFI_BLE_COEXIST
    -DCONFIG_CMAKE=1
    -DBK_MAC=1
    -DBK_SUPPLICANT=1
    -DMBEDTLS_CONFIG_FILE=\"tls_config.h\"
    -DMQTT_COMM_ENABLED
    -DMQTT_DIRECT
    -DIOTX_WITHOUT_TLS
    -DOTA_SIGNAL_CHANNEL=1
    -D_GNU_SOURCE
    -DCONFIG_RELEASE_VERSION=1
    -DCFG_LOG_LEVEL=2
    -DAPP_VERSION=\"unknow\"
    -DLIB_HASH=\"2d93879d9432a7617818a7f71ed9f98a\"
    -DARMINO_SOC=bk7258
    -DBEKEN_PLATFORM
)

set(BK_WIFI_MACHINE
    -mcpu=cortex-m33+nodsp -mfpu=fpv5-sp-d16 -mfloat-abi=hard -mcmse
    -std=gnu99 -fsigned-char -fno-strict-aliasing -Wno-error -w)

# An object library rather than set_source_files_properties: source
# properties only apply in the directory that sets them, and the arch target
# these end up in is defined elsewhere in the tree, so the flags would be
# silently dropped -- which is exactly what happened the first time.

add_library(bk_wifi_vendor OBJECT ${BK_WIFI_SOURCES})
target_compile_options(bk_wifi_vendor PRIVATE
                       ${BK_WIFI_INCLUDES} ${BK_WIFI_DEFINES}
                       ${BK_WIFI_MACHINE})
set(BK_WIFI_OBJECTS $<TARGET_OBJECTS:bk_wifi_vendor>)

# The closed MAC/PHY archives.  libwifi.a holds the 802.11 MAC that the
# sources above call into; libcom_phy.a and libbk_phy.a are the radio, and
# are already linked for BLE -- listing them again is harmless and keeps
# this file self-contained about what WiFi needs.

set(BK_WIFI_LIBS ${BK_IDK_ROOT}/components/bk_libs/bk7258/libs)

if(NOT EXISTS ${BK_WIFI_LIBS}/libwifi.a)
  message(FATAL_ERROR "libwifi.a not found under ${BK_WIFI_LIBS}")
endif()

nuttx_add_extra_library(${BK_WIFI_LIBS}/libwifi.a)
