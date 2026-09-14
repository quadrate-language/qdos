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

/** @brief Display width in pixels. 8x8 font gives a 40x30 character console. */
#define QDOS_SCREEN_W 320
/** @brief Display height in pixels. */
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
	QDOS_KEY_CLEAR,
	QDOS_KEY_POWER,

	/* Any other printable character. Carries its ASCII value in
	 * qdos_key_event.ch — this is how the full Quadrate language reaches the
	 * shell on a keypad that has no letters on it (soft keyboard, or a host
	 * keyboard in the simulator). */
	QDOS_KEY_CHAR,

	QDOS_KEY__COUNT
} qdos_key;

/** @brief A single key press */
typedef struct {
	qdos_key key; ///< Logical key
	char ch;	  ///< ASCII character for QDOS_KEY_CHAR, otherwise 0
} qdos_key_event;

/** @brief Storage result codes */
typedef enum {
	QDOS_STORE_OK = 0,		  ///< Success
	QDOS_STORE_NOT_FOUND = 1, ///< No such entry
	QDOS_STORE_TOO_BIG = 2,	  ///< Entry does not fit in the supplied buffer
	QDOS_STORE_IO_ERROR = 3	  ///< Backend failure
} qdos_store_result;

typedef struct qdos_hal qdos_hal;

/** @brief A hardware backend */
struct qdos_hal {
	/**
	 * @brief Bring up the hardware
	 * @return 0 on success, non-zero on failure
	 */
	int (*init)(qdos_hal* hal);

	/** @brief Release the hardware. Safe to call after a failed init(). */
	void (*shutdown)(qdos_hal* hal);

	/**
	 * @brief Push a framebuffer to the display
	 * @param fb QDOS_SCREEN_W * QDOS_SCREEN_H bytes, one byte per pixel,
	 *           0 = off through 255 = full on. Grayscale so the same buffer
	 */
	void (*present)(qdos_hal* hal, const uint8_t* fb);

	/**
	 * @brief Fetch the next key press if one is waiting
	 * @param[out] out Receives the event when true is returned
	 * @return true if an event was produced, false if the queue is empty
	 */
	bool (*poll_key)(qdos_hal* hal, qdos_key_event* out);

	/** @brief Whether the machine should keep running */
	bool (*running)(qdos_hal* hal);

	/** @brief Yield until roughly the next display refresh */
	void (*idle)(qdos_hal* hal);

	/**
	 * @brief Read a stored entry
	 * @param name     Entry name
	 * @param buf      Destination buffer
	 * @param cap      Capacity of @p buf
	 * @param[out] len Receives the number of bytes written
	 */
	qdos_store_result (*store_read)(qdos_hal* hal, const char* name, void* buf, size_t cap, size_t* len);

	/** @brief Write a stored entry, replacing any previous value */
	qdos_store_result (*store_write)(qdos_hal* hal, const char* name, const void* buf, size_t len);

	void* impl; ///< Backend private state
};

#ifdef __cplusplus
}
#endif

#endif // QDOS_HAL_H
