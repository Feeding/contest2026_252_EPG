# ##############################################################################
# apps/eyes/CMakeLists.txt
#
# Robot-face eye animation for the contest board (dual GC9D01 over QSPI).
#
# ##############################################################################

if(CONFIG_CONTEST_EYES)
  nuttx_add_application(
    NAME
    eyes
    SRCS
    eyes_main.c
    STACKSIZE
    ${CONFIG_CONTEST_EYES_STACKSIZE}
    PRIORITY
    100)
endif()
