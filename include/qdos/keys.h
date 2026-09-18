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
	QDOS_KEY_ANGLE, ///< Degrees or radians, the one setting that changes answers
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
