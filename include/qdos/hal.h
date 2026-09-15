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

	/* Editing and control */
	QDOS_KEY_ENTER,
	QDOS_KEY_BACKSPACE,
	QDOS_KEY_TAB,
	QDOS_KEY_CLEAR,
	QDOS_KEY_POWER,

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

	/** @brief Yield until roughly the next display refresh */
	void (*idle)(qdos_hal* hal);

	qdos_store_result (*store_read)(qdos_hal* hal, const char* name, void* buf, size_t cap, size_t* len);

	qdos_store_result (*store_write)(qdos_hal* hal, const char* name, const void* buf, size_t len);

	/** @brief Unordered, and may be NULL */
	qdos_store_result (*store_list)(qdos_hal* hal, qdos_store_visit visit, void* user);

	void* impl; ///< Backend private state
};

#ifdef __cplusplus
}
#endif

#endif // QDOS_HAL_H
