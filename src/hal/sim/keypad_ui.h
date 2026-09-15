/**
 * @file keypad_ui.h
 * @brief The simulator's on-screen keypad, drawn without SDL so it can be tested
 */

#ifndef QDOS_KEYPAD_UI_H
#define QDOS_KEYPAD_UI_H

#include <qdos/hal.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QDOS_PAD_COLS 5
#define QDOS_PAD_ROWS 8
#define QDOS_PAD_BUTTON_W (QDOS_SCREEN_W / QDOS_PAD_COLS)
#define QDOS_PAD_BUTTON_H 44
#define QDOS_PAD_H (QDOS_PAD_ROWS * QDOS_PAD_BUTTON_H)

typedef struct {
	const char* label;
	qdos_key key;	  ///< QDOS_KEY_NONE when the button types text instead
	const char* text; ///< Typed as QDOS_KEY_CHAR events; line mode only
} qdos_pad_button;

void qdos_pad_draw(uint8_t* rgb, int stride_px, int y0);

/** @brief Button at a window coordinate, panel included in y */
const qdos_pad_button* qdos_pad_at(int x, int y);

const qdos_pad_button* qdos_pad_button_at(int col, int row);

#ifdef __cplusplus
}
#endif

#endif // QDOS_KEYPAD_UI_H
