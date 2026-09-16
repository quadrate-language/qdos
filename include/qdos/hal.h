/**
 * @file hal.h
 * @brief Hardware abstraction for QDOS
 */

#ifndef QDOS_HAL_H
#define QDOS_HAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Sharp Memory LCD (LS027B7DH01); 25x10 cells at 16x24 */
#define QDOS_SCREEN_W 400
#define QDOS_SCREEN_H 240

/** @brief Logical keys on the calculator keypad */
typedef enum {
	QDOS_KEY_NONE = 0,

	/* Digits and decimal point */
	QDOS_KEY_0, QDOS_KEY_1, QDOS_KEY_2, QDOS_KEY_3, QDOS_KEY_4,
	QDOS_KEY_5, QDOS_KEY_6, QDOS_KEY_7, QDOS_KEY_8, QDOS_KEY_9,
	QDOS_KEY_DOT,

	/* Arithmetic */
	QDOS_KEY_ADD, QDOS_KEY_SUB, QDOS_KEY_MUL, QDOS_KEY_DIV,

	/* Stack — the operations an RPN keypad needs as dedicated keys */
	QDOS_KEY_DUP, QDOS_KEY_DROP, QDOS_KEY_SWAP,
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
	QDOS_KEY_OPEN,	///< Edit the selection
	QDOS_KEY_CHECK,   ///< Compile what is in the editor without keeping it
	QDOS_KEY_CATALOG, ///< Every word there is, to pick from
	QDOS_KEY_ABOUT,    ///< What this firmware is
	QDOS_KEY_SETTINGS,
	QDOS_KEY_DEBUG,    ///< The log of what has been said
	QDOS_KEY_UNDO,

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

typedef enum {
	QDOS_STORE_OK = 0,		  ///< Success
	QDOS_STORE_NOT_FOUND = 1, ///< No such entry
	QDOS_STORE_TOO_BIG = 2,	  ///< Entry does not fit in the supplied buffer
	QDOS_STORE_IO_ERROR = 3	  ///< Backend failure
} qdos_store_result;

typedef struct qdos_hal qdos_hal;

/**
 * @brief Which store. Only the user one is writable.
 *
 * Listed in the order a program of the same name shadows one before it, so a
 * word written on the calculator wins over an uploaded one, which wins over
 * one shipped in the firmware. Nothing is ever lost: the two read-only scopes
 * are still there under an override, and erasing it brings them back.
 */
typedef enum {
	QDOS_SCOPE_SYSTEM = 0, ///< Shipped with the firmware, on the read-only rootfs
	QDOS_SCOPE_INBOX = 1,  ///< Uploaded from a PC, read-only like the firmware's own
	QDOS_SCOPE_USER = 2,   ///< Written on the calculator
	QDOS_SCOPE__COUNT
} qdos_store_scope;

/** @brief Receives one stored entry name; false stops the walk */
typedef bool (*qdos_store_visit)(const char* name, void* user);

struct qdos_hal {
	int (*init)(qdos_hal* hal);

	/** @brief Safe after a failed init() */
	void (*shutdown)(qdos_hal* hal);

	/** @brief One grayscale byte per pixel, so mono and RGB share a buffer */
	void (*present)(qdos_hal* hal, const uint8_t* fb);

	bool (*poll_key)(qdos_hal* hal, qdos_key_event* out);

	bool (*running)(qdos_hal* hal);

	/**
	 * @brief Milliseconds since start, monotonic
	 *
	 * Only differences are ever taken, and they are taken unsigned, so the wrap
	 * at 49 days needs no handling.
	 */
	uint32_t (*ticks_ms)(qdos_hal* hal);

	/**
	 * @brief Yield until a key arrives or the timeout expires
	 *
	 * A negative timeout waits for a key and nothing else, which is what a
	 * calculator left alone should do: no timer, no wakeups, no repaints.
	 * Returning early is allowed -- the caller re-reads the clock regardless.
	 */
	void (*wait)(qdos_hal* hal, int timeout_ms);

	qdos_store_result (*store_read)(
			qdos_hal* hal, qdos_store_scope scope, const char* name, void* buf, size_t cap, size_t* len);

	/** @brief No scope: the system store cannot be written, by construction */
	qdos_store_result (*store_write)(qdos_hal* hal, const char* name, const void* buf, size_t len);

	/** @brief Unordered, and may be NULL */
	qdos_store_result (*store_list)(
			qdos_hal* hal, qdos_store_scope scope, qdos_store_visit visit, void* user);

	/**
	 * @brief Hand the inbox to a host over USB, or take it back
	 *
	 * While it is handed over the host owns those blocks, so the inbox scope
	 * reads as empty until it comes back. NULL where the machine has no USB
	 * gadget, and then the shell does not offer it.
	 *
	 * @return 0 on success
	 */
	int (*usb_export)(qdos_hal* hal, bool on);

	void* impl; ///< Backend private state
};

#ifdef __cplusplus
}
#endif

#endif // QDOS_HAL_H
