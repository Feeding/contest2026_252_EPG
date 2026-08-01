# ##############################################################################
# apps/snap/CMakeLists.txt
#
# One-shot GC2145 JPEG capture to file.
#
# ##############################################################################

if(CONFIG_CONTEST_SNAP)
  nuttx_add_application(
    NAME
    snap
    SRCS
    snap_main.c
    STACKSIZE
    4096
    PRIORITY
    100)
endif()
