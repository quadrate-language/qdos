/**
 * @file storage.h
 * @brief Persisting values and sessions through the HAL
 */

#ifndef QDOS_STORAGE_H
#define QDOS_STORAGE_H

#include <qdos/hal.h>

#include <quadrate/interp/interp.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QDOS_VALUE_STRING_MAX 240

typedef enum {
	QDOS_VALUE_EMPTY = 0,
	QDOS_VALUE_INT = 1,
	QDOS_VALUE_FLOAT = 2,
	QDOS_VALUE_STRING = 3
} qdos_value_type;

typedef struct {
	qdos_value_type type;
	int64_t i;
	double f;
	char s[QDOS_VALUE_STRING_MAX];
} qdos_value;

#define QDOS_VALUE_ENCODED_MAX (16 + QDOS_VALUE_STRING_MAX)

bool qdos_value_encode(const qdos_value* value, uint8_t* out, size_t* len);

/** @brief False if the bytes are not a value this build understands */
bool qdos_value_decode(const uint8_t* in, size_t len, qdos_value* value);

qdos_store_result qdos_storage_save(qdos_hal* hal, const char* key, const qdos_value* value);

qdos_store_result qdos_storage_load(qdos_hal* hal, const char* key, qdos_value* value);

/** @brief Mark an entry empty; there is no delete in the HAL */
qdos_store_result qdos_storage_erase(qdos_hal* hal, const char* key);

bool qdos_register_key(int64_t slot, char* buf, size_t cap);

#define QDOS_REGISTER_MAX 99

#define QDOS_PROGRAM_MAX 4096

#define QDOS_PROGRAM_NAME_MAX 64

/** @brief Plain `<name>.qd`, so an uploaded file and a saved one are alike */
bool qdos_program_key(const char* name, char* buf, size_t cap);

/**
 * @brief The entry point a folder has to hold to be an app
 *
 * A folder on the card is an app named after the folder, and this is the file
 * inside it the shell runs. Everything else in there -- its modules, its data
 * -- belongs to it and is not listed separately.
 */
#define QDOS_APP_MAIN "main.qd"

/** @brief The word an app's entry point declares */
#define QDOS_APP_ENTRY "main"

/** @brief `doom` and `main.qd` make the store key `doom/main.qd` */
bool qdos_app_key(const char* app, const char* leaf, char* buf, size_t cap);

/** @brief The app a listed folder is: `doom/` is `doom`, a file is nothing */
bool qdos_app_name(const char* entry, char* out, size_t cap);

/** @brief Whether a folder of this name, holding an entry point, is on the card */
bool qdos_app_exists(qdos_hal* hal, const char* name);

/** @brief `libfoo.so` is the module `foo`, reached as `foo::bar` */
bool qdos_module_name(const char* entry, char* out, size_t cap);

/** @brief The store entry a module name belongs to: `foo` is `libfoo.so` */
bool qdos_module_key(const char* name, char* buf, size_t cap);

qdos_store_result qdos_program_save(qdos_hal* hal, const char* name, const char* source);

/** @brief NOT_FOUND if absent or erased */
qdos_store_result qdos_program_load(qdos_hal* hal, qdos_store_scope scope, const char* name, char* buf, size_t cap);

qdos_store_result qdos_program_erase(qdos_hal* hal, const char* name);

/**
 * @brief Declare the stored programs of one scope into @p interp
 *
 * A program that fails to parse is skipped rather than fatal: one bad upload
 * must not cost the user their calculator. -1 if the backend cannot enumerate.
 */
int qdos_programs_restore(qdos_hal* hal, qdos_store_scope scope, qd_interp* interp);

typedef struct {
	char name[QDOS_PROGRAM_NAME_MAX];
	bool system; ///< Shipped in the firmware
	bool inbox;	 ///< Uploaded from a PC
	bool user;	 ///< Written here
	bool app;	 ///< A folder with an entry point, rather than a loose file
} qdos_program_entry;

/**
 * @brief Every installed program, sorted, each marked with where it came from
 * @return Number written, which may be less than found if @p cap is small
 */
size_t qdos_programs_gather(qdos_hal* hal, qdos_program_entry* out, size_t cap);

bool qdos_program_in_scope(qdos_hal* hal, qdos_store_scope scope, const char* name);

/** @brief Whether a program of this name is shipped with the firmware */
bool qdos_program_is_system(qdos_hal* hal, const char* name);

bool qdos_program_is_user(qdos_hal* hal, const char* name);

/** @brief Whether a program of this name was uploaded from a PC */
bool qdos_program_is_inbox(qdos_hal* hal, const char* name);

/**
 * @brief Whether this name comes from a scope that cannot be written
 *
 * Shipped and uploaded programs are both read-only here: they can be overridden
 * by saving one of the same name, but not dropped. An upload is dropped by
 * taking the file off the card it came from.
 */
bool qdos_program_is_readonly(qdos_hal* hal, const char* name);

qdos_store_result qdos_storage_save_session(qdos_hal* hal, qd_interp* interp);

/**
 * @brief Push a saved stack back onto the interpreter
 * @return NOT_FOUND when there is no saved session, which is a first boot
 */
qdos_store_result qdos_storage_restore_session(qdos_hal* hal, qd_interp* interp);

#ifdef __cplusplus
}
#endif

#endif // QDOS_STORAGE_H
