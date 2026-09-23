/**
 * @file hal.h
 * @brief Hardware abstraction for QDOS
 */

#ifndef QDOS_HAL_H
#define QDOS_HAL_H

#include <qdos/keys.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Sharp Memory LCD (LS027B7DH01); 25x10 cells at 16x24 */
#define QDOS_SCREEN_W 400
#define QDOS_SCREEN_H 240

/** @brief Which face of the keypad the next press will come from */
typedef enum {
	QDOS_MOD_NONE = 0,
	QDOS_MOD_ALPHA, ///< Letters, locked until it is pressed again
	QDOS_MOD_SYMBOL ///< Quadrate's syntax, for one press
} qdos_keypad_mod;

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

/** @brief Marks a listed name as a folder rather than a file: `doom/` */
#define QDOS_STORE_DIR_MARK '/'

/**
 * @brief Whether a name is one a store may hold
 *
 * A name is a file, or one folder and a file in it: `doom/main.qd`. One level
 * and no more, so an app is a flat directory of its own parts and the store
 * stays something a FAT card and a fixed-size buffer can both hold.
 */
bool qdos_store_name_ok(const char* name);

struct qdos_hal {
	int (*init)(qdos_hal* hal);

	/** @brief Safe after a failed init() */
	void (*shutdown)(qdos_hal* hal);

	/** @brief One grayscale byte per pixel, so mono and RGB share a buffer */
	void (*present)(qdos_hal* hal, const uint8_t* fb);

	bool (*poll_key)(qdos_hal* hal, qdos_key_event* out);

	/** @brief Which keypad face is live. NULL where the keypad has only one. */
	qdos_keypad_mod (*modifier)(qdos_hal* hal);

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

	/**
	 * @brief Unordered, and may be NULL
	 *
	 * @p folder is NULL for the scope itself, or an app's name to list what is
	 * inside it. Names come back bare; a folder carries QDOS_STORE_DIR_MARK.
	 */
	qdos_store_result (*store_list)(
			qdos_hal* hal, qdos_store_scope scope, const char* folder, qdos_store_visit visit, void* user);

	/**
	 * @brief Where an entry sits in a filesystem, if it sits in one at all
	 *
	 * The store speaks in bytes everywhere else; the dynamic linker takes a
	 * path. NULL where there is none, and then there are no modules.
	 */
	bool (*store_path)(qdos_hal* hal, qdos_store_scope scope, const char* name, char* buf, size_t cap);

	/**
	 * @brief Has anything arrived on the card since this was last asked?
	 *
	 * Answered from a watch rather than by looking: the machine sleeps with no
	 * timer, and examining the store would cost the wakeups that buys. Reading
	 * clears it. NULL where the backend cannot tell.
	 */
	bool (*store_changed)(qdos_hal* hal);

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

	/**
	 * @brief Seconds since local midnight, from the wall clock
	 *
	 * False while the clock has not been set: the Pi has no RTC, so until
	 * something sets the time it reads as 1970, and showing a time of day then
	 * would be showing a wrong one. NULL where the backend has no clock.
	 */
	bool (*time_of_day)(qdos_hal* hal, int* seconds);

	/** @brief Charge left, 0 to 100, or -1 with no battery to ask. NULL likewise. */
	int (*battery)(qdos_hal* hal);

	/**
	 * @brief Set by the shell, for a backend that can be killed without warning
	 *
	 * Called to save the session now. Only from inside poll_key or wait, which
	 * the shell reaches between keys. NULL until there is a session to save.
	 */
	void (*save)(void* user);
	void* save_user;

	void* impl; ///< Backend private state
};

#ifdef __cplusplus
}
#endif

#endif // QDOS_HAL_H
