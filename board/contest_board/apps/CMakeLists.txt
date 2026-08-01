# ##############################################################################
# apps/CMakeLists.txt
#
# Contest board applications.  This directory is reached from the apps build
# through vendor/openvela/apps (a symlink back into the contest tree), so
# application code lives beside the board port it belongs to.
#
# ##############################################################################

nuttx_add_subdirectory()
nuttx_generate_kconfig(MENUDESC "Contest board apps")
