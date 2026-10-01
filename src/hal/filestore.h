/**
 * @file filestore.h
 * @brief The store as three directories, for backends with a filesystem
 *
 * Linux and the ESP32 both hold the store as files, one directory per scope,
 * and both go through the same POSIX calls to reach them: on the ESP32 they
 * land in the VFS rather than a kernel, which is nothing this code can see.
 */

#ifndef QDOS_FILESTORE_H
#define QDOS_FILESTORE_H

#include <qdos/hal.h>

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	const char* system_dir; ///< Read-only, shipped with the firmware
	const char* inbox_dir;	///< Read-only, uploaded from a PC
	const char* store_dir;	///< The user scope, the only one written
} qdos_filestore;

/** @brief Whether @p dir / @p name is a directory */
bool qdos_filestore_is_dir(const char* dir, const char* name);

qdos_store_result qdos_filestore_read(
		const qdos_filestore* fs, qdos_store_scope scope, const char* name, void* buf, size_t cap, size_t* len);

qdos_store_result qdos_filestore_write(const qdos_filestore* fs, const char* name, const void* buf, size_t len);

qdos_store_result qdos_filestore_remove(const qdos_filestore* fs, const char* name);

bool qdos_filestore_path(const qdos_filestore* fs, qdos_store_scope scope, const char* name, char* buf, size_t cap);

qdos_store_result qdos_filestore_list(
		const qdos_filestore* fs, qdos_store_scope scope, const char* folder, qdos_store_visit visit, void* user);

#ifdef __cplusplus
}
#endif

#endif // QDOS_FILESTORE_H
