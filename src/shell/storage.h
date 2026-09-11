/**
 * @file storage.h
 * @brief Persisting values and sessions through the HAL
 *
 * Values are encoded explicitly, little-endian, behind a magic and a version.
 * A calculator's stored registers have to survive a firmware update, and a raw
 * struct write would tie them to one compiler's padding and field order.
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

/** @brief Longest string a register holds, including the terminator */
#define QDOS_VALUE_STRING_MAX 240

/**
 * @brief Type of a stored value
 */
typedef enum {
	QDOS_VALUE_EMPTY = 0, ///< Register exists but holds nothing
	QDOS_VALUE_INT = 1,
	QDOS_VALUE_FLOAT = 2,
	QDOS_VALUE_STRING = 3
} qdos_value_type;

/**
 * @brief A value as stored
 */
typedef struct {
	qdos_value_type type;
	int64_t i;						 ///< Valid when type is QDOS_VALUE_INT
	double f;						 ///< Valid when type is QDOS_VALUE_FLOAT
	char s[QDOS_VALUE_STRING_MAX];	 ///< Valid when type is QDOS_VALUE_STRING
} qdos_value;

/** @brief Bytes a single encoded value occupies at most */
#define QDOS_VALUE_ENCODED_MAX (16 + QDOS_VALUE_STRING_MAX)

/**
 * @brief Encode a value into a buffer
 *
 * @param[out] out Destination, at least QDOS_VALUE_ENCODED_MAX bytes
 * @param[out] len Receives the number of bytes written
 * @return false if @p value is malformed
 */
bool qdos_value_encode(const qdos_value* value, uint8_t* out, size_t* len);

/**
 * @brief Decode a value written by qdos_value_encode()
 *
 * Rejects a wrong magic, an unknown version and a truncated record, so a
 * store written by different firmware reads as absent rather than as garbage.
 *
 * @return false if the bytes are not a value this build understands
 */
bool qdos_value_decode(const uint8_t* in, size_t len, qdos_value* value);

/**
 * @brief Write a value to a named entry
 */
qdos_store_result qdos_storage_save(qdos_hal* hal, const char* key, const qdos_value* value);

/**
 * @brief Read a named entry
 *
 * @return QDOS_STORE_NOT_FOUND if absent, QDOS_STORE_IO_ERROR if unreadable
 */
qdos_store_result qdos_storage_load(qdos_hal* hal, const char* key, qdos_value* value);

/**
 * @brief Mark a named entry empty
 *
 * The HAL has no delete, so the entry is overwritten with QDOS_VALUE_EMPTY.
 */
qdos_store_result qdos_storage_erase(qdos_hal* hal, const char* key);

/**
 * @brief Name the entry backing a numbered register
 *
 * @param slot 0 through QDOS_REGISTER_MAX
 * @return false if @p slot is out of range or does not fit @p buf
 */
bool qdos_register_key(int64_t slot, char* buf, size_t cap);

/** @brief Highest numbered register */
#define QDOS_REGISTER_MAX 99

/**
 * @brief Save the interpreter's stack so it can be restored after a power cycle
 *
 * A calculator is expected to come back holding what it held. Bottom of stack
 * is written first, so restoring pushes in the same order.
 */
qdos_store_result qdos_storage_save_session(qdos_hal* hal, qd_interp* interp);

/**
 * @brief Push a saved stack back onto the interpreter
 *
 * @return QDOS_STORE_NOT_FOUND when there is no saved session, which is not an
 *         error — it is what a first boot looks like
 */
qdos_store_result qdos_storage_restore_session(qdos_hal* hal, qd_interp* interp);

#ifdef __cplusplus
}
#endif

#endif // QDOS_STORAGE_H
