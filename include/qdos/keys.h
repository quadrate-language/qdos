/**
 * @file keys.h
 * @brief The keys the machine has, and what a press of one is
 *
 * Apart from the HAL: a native module names keys without seeing the backend.
 */

#ifndef QDOS_KEYS_H
#define QDOS_KEYS_H

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Logical keys on the calculator keypad */
typedef enum {
	QDOS_KEY_NONE = 0,

	/* Digits and decimal point */
	QDOS_KEY_0,
	QDOS_KEY_1,
	QDOS_KEY_2,
	QDOS_KEY_3,
	QDOS_KEY_4,
	QDOS_KEY_5,
	QDOS_KEY_6,
	QDOS_KEY_7,
	QDOS_KEY_8,
	QDOS_KEY_9,
	QDOS_KEY_DOT,

	/* Arithmetic */
	QDOS_KEY_ADD,
	QDOS_KEY_SUB,
	QDOS_KEY_MUL,
	QDOS_KEY_DIV,

	/* Stack — the operations an RPN keypad needs as dedicated keys */
	QDOS_KEY_DUP,
	QDOS_KEY_DROP,
	QDOS_KEY_SWAP,
	QDOS_KEY_NEG, ///< +/-, which a bare minus cannot do while entering a number

	/* Words with a key of their own. Contiguous, and FUNCTION_WORD runs in step. */
	QDOS_KEY_SIN,
	QDOS_KEY_COS,
	QDOS_KEY_TAN,
	QDOS_KEY_LN,
	QDOS_KEY_LOG,
	QDOS_KEY_SQRT,
	QDOS_KEY_SQ,
	QDOS_KEY_POW,
	QDOS_KEY_INV,
	QDOS_KEY_ABS,
	QDOS_KEY_FLOOR,
	QDOS_KEY_CEIL,
	QDOS_KEY_ROUND,
	QDOS_KEY_MOD,
	QDOS_KEY_ROT,
	QDOS_KEY_OVER,
#define QDOS_KEY_FN_FIRST QDOS_KEY_SIN
#define QDOS_KEY_FN_LAST QDOS_KEY_OVER

	/* Editing and control */
	QDOS_KEY_ENTER,
	QDOS_KEY_BACKSPACE,
	QDOS_KEY_TAB,
	QDOS_KEY_CLEAR,
	QDOS_KEY_POWER,

	/* Navigation */
	QDOS_KEY_UP,
	QDOS_KEY_DOWN,
	QDOS_KEY_LEFT,
	QDOS_KEY_RIGHT,
	QDOS_KEY_LIST,
	QDOS_KEY_SAVE,
	QDOS_KEY_OPEN,	  ///< Edit the selection
	QDOS_KEY_CHECK,	  ///< Compile what is in the editor without keeping it
	QDOS_KEY_CATALOG, ///< Every word there is, to pick from
	QDOS_KEY_ABOUT,	  ///< What this firmware is
	QDOS_KEY_SETTINGS,
	QDOS_KEY_DEBUG, ///< The log of what has been said
	QDOS_KEY_UNDO,
	QDOS_KEY_ANGLE, ///< Unused; kept so the codes after it, which apps see, do not move
	QDOS_KEY_STO,	///< Store x in a register; the next digit says which
	QDOS_KEY_RCL,	///< Recall one

	/* Soft keys, labelled on screen because their meaning follows the mode */
	QDOS_KEY_SOFT1,
	QDOS_KEY_SOFT2,
	QDOS_KEY_SOFT3,
	QDOS_KEY_SOFT4,
	QDOS_KEY_SOFT5,

	/* Any other printable character, ASCII value in qdos_key_event.ch. How
	 * the full language reaches a keypad with no letters on it. */
	QDOS_KEY_CHAR,

	/*
	 * Added since, and only ever added here. These numbers are what
	 * `ui::key` hands a Quadrate program and what a native module compares
	 * against, and neither is rebuilt when QDOS is: a key put in the middle
	 * renumbers every one after it, and DOOM's ESC becomes something else.
	 */

	/* More words with a key of their own, a second run beside FN_FIRST..LAST */
	QDOS_KEY_ASIN,
	QDOS_KEY_ACOS,
	QDOS_KEY_ATAN,
	QDOS_KEY_EXP,
#define QDOS_KEY_FN2_FIRST QDOS_KEY_ASIN
#define QDOS_KEY_FN2_LAST QDOS_KEY_EXP

	QDOS_KEY_TRACE,		 ///< Follow the curve on a graph
	QDOS_KEY_FIT,		 ///< Scale a graph's y to what it shows
	QDOS_KEY_STD,		 ///< A graph's standard window
	QDOS_KEY_GRAPH,		 ///< The Y= page, and on it, draw
	QDOS_KEY_TOGGLE,	 ///< Switch the selected Y= slot on or off
	QDOS_KEY_VAR_X,		 ///< Type x, the variable a Y= slot is a function of
	QDOS_KEY_VAR_Y,		 ///< and y, for a surface
	QDOS_KEY_VAR_T,		 ///< t, for a parametric curve
	QDOS_KEY_VAR_THETA,	 ///< theta, for a polar one
	QDOS_KEY_ZOOM,		 ///< A graph's choice of windows
	QDOS_KEY_CALC,		 ///< What can be found on a curve, or from the lists
	QDOS_KEY_MENU,		 ///< The rest of what a page offers
	QDOS_KEY_TBL_START,	 ///< Where a table starts
	QDOS_KEY_TBL_STEP,	 ///< and how far apart its rows are
	QDOS_KEY_STATPLOT,	 ///< How the lists are drawn on the graph
	QDOS_KEY_LIST_CLEAR, ///< Empty a list
	QDOS_KEY_TO_Y,		 ///< A regression into a Y= slot
	QDOS_KEY_MODE,		 ///< Between the calculator and typing a line
	QDOS_KEY_PI,		 ///< pi, onto the stack or into the line
	QDOS_KEY_E,			 ///< e, likewise
	QDOS_KEY_NEW,		 ///< Start a program, asking for its name
	QDOS_KEY_RUN,		 ///< Run the program in the editor
	QDOS_KEY_COMPLEX,	 ///< Two reals into one complex number, or one back into two
	QDOS_KEY_I,			 ///< i, the imaginary unit

	QDOS_KEY__COUNT
} qdos_key;

/** @brief A single key press */
typedef struct {
	qdos_key key; ///< Logical key
	char ch;	  ///< ASCII character for QDOS_KEY_CHAR, otherwise 0
} qdos_key_event;

#ifdef __cplusplus
}
#endif

#endif // QDOS_KEYS_H
