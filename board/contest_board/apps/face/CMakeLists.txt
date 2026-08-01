# ##############################################################################
# apps/face/CMakeLists.txt
#
# Robot face demo: procedural eyes + vendor MJPEG expression clips.
#
# ##############################################################################

if(CONFIG_CONTEST_FACE)
  nuttx_add_application(
    NAME
    face
    SRCS
    face_main.c
    tjpgd.c
    STACKSIZE
    8192
    PRIORITY
    100)
endif()
