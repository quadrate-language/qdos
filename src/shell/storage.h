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

/** @brief Longest string a register holds, with the terminator */
#define QDOS_VALUE_STRING_MAX 240

typedef enum {
	QDOS_VALUE_EMPTY = 0, ///< Register exists but holds nothing
	QDOS_VALUE_INT = 1,
	QDOS_VALUE_FLOAT = 2,
	QDOS_VALUE_STRING = 3
} qdos_value_type;

typedef struct {
	qdos_value_type type;
	int64_t i;						 ///< Valid when type is QDOS_VALUE_INT
	double f;						 ///< Valid when type is QDOS_VALUE_FLOAT
	char s[QDOS_VALUE_STRING_MAX];	 ///< Valid when type is QDOS_VALUE_STRING
} qdos_value;

#define QDOS_VALUE_ENCODED_MAX (16 + QDOS_VALUE_STRING_MAX)

/** @brief Encode into a buffer of at least QDOS_VALUE_ENCODED_MAX bytes */
bool qdos_value_encode(const qdos_value* value, uint8_t* out, size_t* len);

/** @brief Decode; false if the bytes are not a value this build understands */
bool qdos_value_decode(const uint8_t* in, size_t len, qdos_value* value);

/** @brief Write a value to a named entry */
qdos_store_result qdos_storage_save(qdos_hal* hal, const char* key, const qdos_value* value);

qdos_store_result qdos_storage_load(qdos_hal* hal, const char* key, qdos_value* value);

/** @brief Mark an entry empty; there is no delete in the HAL */
qdos_store_result qdos_storage_erase(qdos_hal* hal, const char* key);

/** @brief Name the entry backing a numbered register */
bool qdos_register_key(int64_t slot, char* buf, size_t cap);

/** @brief Highest numbered register */
#define QDOS_REGISTER_MAX 99

/** @brief Longest program source QDOS stores, including the terminator */
#define QDOS_PROGRAM_MAX 4096

/** @brief Longest program name, including the terminator */
#define QDOS_PROGRAM_NAME_MAX 64

/**
 * @brief Name the entry backing a program
 *
 * Plain `<name>.qd` source, so a file copied from a PC and one QDOS saved
 * itself are the same thing.
 */
bool qdos_program_key(const char* name, char* buf, size_t cap);

/** @brief Write a program's source, replacing any previous definition */
qdos_store_result qdos_program_save(qdos_hal* hal, const char* name, const char* source);

/** @brief Read a program's source; NOT_FOUND if absent or erased */
qdos_store_result qdos_program_load(qdos_hal* hal, const char* name, char* buf, size_t cap);

/** @brief Drop a stored program */
qdos_store_result qdos_program_erase(qdos_hal* hal, const char* name);

/**
 * @brief Declare every stored program into @p interp, at boot
 *
 * A program that fails to parse is skipped rather than fatal: one bad upload
 * must not cost the user their calculator. -1 if the backend cannot enumerate.
 */
int qdos_programs_restore(qdos_hal* hal, qd_interp* interp);

/** @brief Save the interpreter's stack so it can be restored after a power cycle */
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
