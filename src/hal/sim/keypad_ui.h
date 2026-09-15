/**
 * @file keypad_ui.h
 * @brief The simulator's on-screen keypad, drawn without SDL so it can be tested
 */

#ifndef QDOS_KEYPAD_UI_H
#define QDOS_KEYPAD_UI_H

#include <qdos/hal.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QDOS_PAD_COLS 5
#define QDOS_PAD_ROWS 10
#define QDOS_PAD_BUTTON_W (QDOS_SCREEN_W / QDOS_PAD_COLS)
#define QDOS_PAD_BUTTON_H 36
#define QDOS_PAD_H (QDOS_PAD_ROWS * QDOS_PAD_BUTTON_H)

/** @brief Which face of the keypad is showing */
typedef enum {
	QDOS_PAD_PLAIN = 0, ///< The calculator
	QDOS_PAD_ALPHA,		///< Letters, for names and strings
	QDOS_PAD_SYMBOL		///< Quadrate's syntax
} qdos_pad_layer;

typedef struct {
	const char* label;
	qdos_key key;	  ///< QDOS_KEY_NONE when the button types text instead
	const char* text; ///< Typed as QDOS_KEY_CHAR events; line mode only
} qdos_pad_action;

typedef struct {
	qdos_pad_action plain;
	qdos_pad_action alpha;	///< Label NULL where the plain action stands in
	qdos_pad_action symbol; ///< Label NULL where symbol does nothing
} qdos_pad_button;

/** @brief The layer this button switches to, if it is a modifier at all */
bool qdos_pad_modifier(const qdos_pad_button* b, qdos_pad_layer* selects);

void qdos_pad_draw(uint8_t* rgb, int stride_px, int y0, qdos_pad_layer layer);

/** @brief Button at a window coordinate, panel included in y */
const qdos_pad_button* qdos_pad_at(int x, int y);

const qdos_pad_button* qdos_pad_button_at(int col, int row);

/**
 * @brief The action a press should send on this layer
 *
 * Alpha falls back to the plain action, so a locked layer still has its arrows,
 * Enter and backspace. Symbol does not: a button with nothing on it stays quiet.
 */
const qdos_pad_action* qdos_pad_action_for(const qdos_pad_button* b, qdos_pad_layer layer);

#ifdef __cplusplus
}
#endif

#endif // QDOS_KEYPAD_UI_H
