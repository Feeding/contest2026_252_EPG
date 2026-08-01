/****************************************************************************
 * boards/arm/bk7258/contest_board/chip/bk7258_psram.h
 ****************************************************************************/

#ifndef __BOARDS_ARM_BK7258_CONTEST_BOARD_CHIP_BK7258_PSRAM_H
#define __BOARDS_ARM_BK7258_CONTEST_BOARD_CHIP_BK7258_PSRAM_H

#include <stddef.h>

/****************************************************************************
 * Name: bk7258_psram_init
 *
 * Description:
 *   Power, clock and configure the 16 MB APS128XXO_OB9 octal PSRAM behind
 *   the controller at 0x46080000 and verify the die answers with its ID.
 *   On success the memory is directly addressable at 0x60000000.
 *
 * Returned Value:
 *   Usable size in bytes (16 MB), or 0 if the die never identified or the
 *   read-back test failed.
 *
 ****************************************************************************/

size_t bk7258_psram_init(void);

#endif /* __BOARDS_ARM_BK7258_CONTEST_BOARD_CHIP_BK7258_PSRAM_H */
