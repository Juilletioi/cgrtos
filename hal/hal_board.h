/**
 * @file hal/hal_board.h
 * @brief 板级 MMIO 转发头（实际内容在 boards/{BOARD}/hal_board.h）
 * @author Cong Zhou / Juilletioi
 * @version 5.3.0
 * @date 2026-07-24
 * @copyright CG-RTOS
 *
 * @details
 * Makefile 将 `-Iboards/$(BOARD)` 置于 `-Ihal` 之前，使 `#include "hal_board.h"`
 * 优先命中板级文件。本文件仅作 IDE / 缺省回退（默认 Nuclei evalsoc）。
 */
#ifndef HAL_BOARD_FALLBACK_H
#define HAL_BOARD_FALLBACK_H
#include "../boards/nuclei_evalsoc/hal_board.h"
#endif
