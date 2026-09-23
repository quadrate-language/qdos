/**
 * @file native.h
 * @brief Loading the native modules found in the stores
 *
 * See include/qdos/native.h for what a module is built against.
 */

#ifndef QDOS_SHELL_NATIVE_H
#define QDOS_SHELL_NATIVE_H

#include <qdos/hal.h>
#include <qdos/native.h>

#include <quadrate/interp/interp.h>

#include "storage.h"

#ifdef __cplusplus
extern "C" {
#endif

#define QDOS_NATIVE_MAX 8
#define QDOS_NATIVE_WORDS_MAX 64

/** @brief Room for why a module was refused, on a display this wide */
#define QDOS_NATIVE_ERROR_MAX 26
#define QDOS_NATIVE_BLOCKED_MAX 128

typedef struct {
	char name[QDOS_PROGRAM_NAME_MAX]; ///< `foo`, from libfoo.so

	/** @brief The app this belongs to, or empty for one loose on the card.
	 * A module inside an app is that app's own half and gets no row of its own. */
	char app[QDOS_PROGRAM_NAME_MAX];

	void* handle;					   ///< NULL if refused
	const qdos_native_module* module;  ///< Inside the mapping, valid until unload
	char error[QDOS_NATIVE_ERROR_MAX]; ///< Why it was refused, or empty

	bool system;
	bool inbox;
	bool user;
} qdos_native_entry;

typedef struct {
	qdos_native_entry entry[QDOS_NATIVE_MAX];
	size_t count;

	char blocked[QDOS_NATIVE_BLOCKED_MAX]; ///< Names never to open again
	char faulted[QDOS_PROGRAM_NAME_MAX];   ///< What was blocked this boot
} qdos_natives;

/**
 * @brief Load every module in one scope
 *
 * Called for each scope in turn, nearest last, so a module found again replaces
 * the one before it. Every scope has to be loaded before anything is
 * registered: replacing a module closes it, and its words would dangle.
 *
 * @return Modules accepted from this scope
 */
int qdos_natives_load(qdos_natives* set, qdos_hal* hal, qdos_store_scope scope);

/**
 * @brief Make the loaded words callable
 *
 * Safe for more than one interpreter: the lint compiles against a scratch one
 * and needs the same vocabulary.
 *
 * @return Words registered
 */
int qdos_natives_register(const qdos_natives* set, qd_interp* interp);

/** @brief Close every module. The interpreter must be destroyed first. */
void qdos_natives_unload(qdos_natives* set);

const qdos_native_entry* qdos_natives_find(const qdos_natives* set, const char* name);

/**
 * @brief Find out whether a module took the machine down last time
 *
 * A module's name is written to the store while it is opened and cleared once
 * it is in. A name still there at the next boot belongs to one that did not
 * survive -- and since init respawns the shell, opening it again is a loop with
 * no way out. Call before the first load.
 *
 * @return true if something was blocked just now; the name is in set->faulted
 */
bool qdos_natives_recover(qdos_natives* set, qdos_hal* hal);

void qdos_natives_unblock(qdos_natives* set, qdos_hal* hal);
size_t qdos_natives_blocked_count(const qdos_natives* set);

/**
 * @brief What to do before the first native word runs
 *
 * The shell writes the session here, so a module faulting costs a reboot rather
 * than the stack. Called at most once between rearms.
 */
void qdos_natives_on_call(void (*fn)(void* user), void* user);

/** @brief Also clears a break */
void qdos_natives_rearm(void);

/**
 * @brief The next key for a running program, which PWR never is
 *
 * PWR breaks the program instead: this and api->key return nothing more,
 * api->running() goes false, and the next native word fails with BREAK.
 */
bool qdos_natives_key(qdos_hal* hal, qdos_key_event* out);
bool qdos_natives_broken(void);

/** @brief Hand modules the panel to draw on and the keypad to read */
void qdos_natives_bind(qdos_hal* hal, uint8_t* canvas);

/** @brief Something to run, rather than words to call */
static inline bool qdos_native_is_app(const qdos_native_entry* entry) {
	return entry != NULL && entry->module != NULL && entry->module->main != NULL;
}

#ifdef __cplusplus
}
#endif

#endif // QDOS_SHELL_NATIVE_H
