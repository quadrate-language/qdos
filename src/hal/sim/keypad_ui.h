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

/*
 * The column pitch is the display's, not a choice: five soft labels across 25
 * columns is 80 pixels each, so a label sits squarely over the key it names.
 * The row pitch is a choice, and it is tall enough to give a key its full
 * height with the shift legend printed above it rather than carved out of it.
 */
#define QDOS_PAD_BUTTON_W (QDOS_SCREEN_W / QDOS_PAD_COLS)
#define QDOS_PAD_BUTTON_H 46
#define QDOS_PAD_W (QDOS_PAD_COLS * QDOS_PAD_BUTTON_W)
#define QDOS_PAD_H (QDOS_PAD_ROWS * QDOS_PAD_BUTTON_H)

/**
 * @brief The case around the panel and the keypad, in pixels
 *
 * The panel itself is drawn one window pixel per panel pixel, so a bezel cannot
 * come out of it -- the window grows by this much on every side instead. That
 * keeps the display honest and still leaves the thing looking like an object
 * rather than a screenshot of one.
 */
#define QDOS_FRAME 8

/**
 * @brief The machine's name, printed on the case above the glass
 *
 * On the shell, not on the display: a name drawn on the panel would be the
 * machine telling you what it is every time you looked at a number, and it
 * would cost a row of the ten there are.
 */
#define QDOS_NAMEPLATE "Quad r8 One"
#define QDOS_NAMEPLATE_H 22

/** @brief Where the panel's top-left pixel lands in the window */
#define QDOS_PANEL_X QDOS_FRAME
#define QDOS_PANEL_Y (QDOS_FRAME + QDOS_NAMEPLATE_H)

/** @brief Where the keypad's top-left pixel lands in the window */
#define QDOS_PAD_X QDOS_FRAME
#define QDOS_PAD_Y (QDOS_PANEL_Y + QDOS_SCREEN_H)

#define QDOS_WINDOW_W (QDOS_SCREEN_W + 2 * QDOS_FRAME)
#define QDOS_WINDOW_H (QDOS_PAD_Y + QDOS_PAD_H + QDOS_FRAME)

/*
 * Where a key sits inside its cell. The band above it is the case, printed
 * with what the shift layer does, in yellow, rather than making you press
 * shift to find out what it is. The row pitch is tall enough to carry that band
 * without taking the height out of the key. The cell stays the hit target, so
 * none of the gaps cost anything to aim at.
 */
#define QDOS_KEY_INSET_X 8
#define QDOS_KEY_INSET_TOP 14
#define QDOS_KEY_INSET_BOTTOM 5
#define QDOS_KEY_W (QDOS_PAD_BUTTON_W - 2 * QDOS_KEY_INSET_X)
#define QDOS_KEY_H (QDOS_PAD_BUTTON_H - QDOS_KEY_INSET_TOP - QDOS_KEY_INSET_BOTTOM)

/**
 * @brief Baseline of the shift legend, from the top of the cell
 *
 * Nearer its own key than the one above it, or it reads as belonging to
 * whichever it happens to sit closer to. Far enough off the key that a
 * descender still clears the top edge.
 */
#define QDOS_SHIFT_LABEL_BASELINE 10

/** @brief How wide a keycap may set before it touches the edge of the key */
#define QDOS_KEY_LABEL_W (QDOS_KEY_W - 6)

/**
 * @brief How far a key sinks while it is held, in pixels
 *
 * It has to be enough to see at 1:1 and no more than the gutter below the key,
 * or a pressed key in the bottom row would reach past the pad.
 */
#define QDOS_KEY_TRAVEL 2

/** @brief Which face of the keypad is showing */
typedef enum {
	QDOS_PAD_PLAIN = 0, ///< The calculator
	QDOS_PAD_ALPHA,		///< Letters, for names and strings
	QDOS_PAD_SYMBOL		///< Quadrate's syntax
} qdos_pad_layer;

typedef struct {
	/**
	 * What is printed on the cap.
	 *
	 * NULL means the button does nothing on this layer. Empty means it works
	 * but carries no inscription, which is what a soft key is: the display
	 * names it, and the name changes with the mode.
	 */
	const char* label;
	qdos_key key;	  ///< QDOS_KEY_NONE when the button types text instead
	const char* text; ///< Typed as QDOS_KEY_CHAR events; line mode only
} qdos_pad_action;

typedef struct {
	qdos_pad_action plain;
	qdos_pad_action alpha;	///< Label NULL where the plain action stands in
	qdos_pad_action symbol; ///< Label NULL where symbol does nothing

	/**
	 * The layer this button switches to, or QDOS_PAD_PLAIN if it is a key.
	 *
	 * Stated rather than inferred. This used to be read off the cap, which
	 * tied what a button *is* to what is printed on it -- and the moment the
	 * shift key was blanked it collided with the soft keys, which are blank for
	 * an entirely different reason.
	 */
	qdos_pad_layer selects;
} qdos_pad_button;

/** @brief The layer this button switches to, if it is a modifier at all */
bool qdos_pad_modifier(const qdos_pad_button* b, qdos_pad_layer* selects);

/**
 * @brief Draw the keypad at @p x0, @p y0 in an RGB buffer @p stride_px wide
 *
 * Nothing is drawn outside the QDOS_PAD_W by QDOS_PAD_H rectangle that starts
 * there, however far a shadow would otherwise spread.
 *
 * @param pressed The key being held down, or NULL if none is. Drawn sunk into
 *                the keywell, which is the only way the simulator can say a
 *                press was received -- there is nothing under your finger.
 */
void qdos_pad_draw(uint8_t* rgb, int stride_px, int x0, int y0, qdos_pad_layer layer,
		const qdos_pad_button* pressed);

/** @brief Draw the case around the panel and the keypad */
void qdos_frame_draw(uint8_t* rgb, int stride_px);

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
