/**
 * @file filestore.c
 * @brief The store as three directories, for backends with a filesystem
 */

// fsync and friends on top of a strict c11 build
#define _POSIX_C_SOURCE 200809L

#include "filestore.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

bool qdos_filestore_is_dir(const char* dir, const char* name) {
	char path[512];
	if (snprintf(path, sizeof(path), "%s/%s", dir, name) >= (int)sizeof(path)) {
		return false;
	}

	struct stat sb;
	return stat(path, &sb) == 0 && S_ISDIR(sb.st_mode);
}

static const char* dir_for(const qdos_filestore* fs, qdos_store_scope scope) {
	switch (scope) {
	case QDOS_SCOPE_SYSTEM:
		return fs->system_dir;
	case QDOS_SCOPE_INBOX:
		return fs->inbox_dir;
	default:
		return fs->store_dir;
	}
}

static bool store_path(const char* dir, const char* name, char* buf, size_t cap) {
	if (!qdos_store_name_ok(name)) {
		return false;
	}

	const int written = snprintf(buf, cap, "%s/%s", dir, name);
	return written > 0 && (size_t)written < cap;
}

qdos_store_result qdos_filestore_read(
		const qdos_filestore* fs, qdos_store_scope scope, const char* name, void* buf, size_t cap, size_t* len) {
	char path[512];
	if (!store_path(dir_for(fs, scope), name, path, sizeof(path))) {
		return QDOS_STORE_IO_ERROR;
	}

	FILE* f = fopen(path, "rb");
	if (!f) {
		return QDOS_STORE_NOT_FOUND;
	}

	const size_t got = fread(buf, 1, cap, f);
	const bool overflowed = (got == cap) && (fgetc(f) != EOF);
	fclose(f);

	if (overflowed) {
		return QDOS_STORE_TOO_BIG;
	}
	if (len) {
		*len = got;
	}
	return QDOS_STORE_OK;
}

qdos_store_result qdos_filestore_write(const qdos_filestore* fs, const char* name, const void* buf, size_t len) {
	char path[512];
	if (!store_path(fs->store_dir, name, path, sizeof(path))) {
		return QDOS_STORE_IO_ERROR;
	}

	mkdir(fs->store_dir, 0755);

	// An app is written into a folder of its own, which may not be there yet
	char* slash = strrchr(path, '/');
	if (slash != NULL && strchr(name, '/') != NULL) {
		*slash = '\0';
		mkdir(path, 0755);
		*slash = '/';
	}

	FILE* f = fopen(path, "wb");
	if (!f) {
		return QDOS_STORE_IO_ERROR;
	}

	const size_t written = fwrite(buf, 1, len, f);
	// fsync before reporting success: power can vanish mid-write.
	fflush(f);
	fsync(fileno(f));
	const bool ok = (fclose(f) == 0) && (written == len);
	return ok ? QDOS_STORE_OK : QDOS_STORE_IO_ERROR;
}

qdos_store_result qdos_filestore_remove(const qdos_filestore* fs, const char* name) {
	char path[512];
	if (!store_path(fs->store_dir, name, path, sizeof(path))) {
		return QDOS_STORE_IO_ERROR;
	}

	if (remove(path) == 0) {
		return QDOS_STORE_OK;
	}
	return (errno == ENOENT) ? QDOS_STORE_NOT_FOUND : QDOS_STORE_IO_ERROR;
}

bool qdos_filestore_path(const qdos_filestore* fs, qdos_store_scope scope, const char* name, char* buf, size_t cap) {
	return store_path(dir_for(fs, scope), name, buf, cap);
}

qdos_store_result qdos_filestore_list(
		const qdos_filestore* fs, qdos_store_scope scope, const char* folder, qdos_store_visit visit, void* user) {
	char root[512];
	if (folder == NULL || *folder == '\0') {
		snprintf(root, sizeof(root), "%s", dir_for(fs, scope));
	} else if (!store_path(dir_for(fs, scope), folder, root, sizeof(root))) {
		return QDOS_STORE_IO_ERROR;
	}

	DIR* dir = opendir(root);
	if (!dir) {
		return QDOS_STORE_NOT_FOUND;
	}

	const struct dirent* ent;
	while ((ent = readdir(dir)) != NULL) {
		if (ent->d_name[0] == '.') {
			continue;
		}

		// A folder is listed with the mark on it, being an app and not a file
		char name[288];
		const int written =
				snprintf(name, sizeof(name), "%s%s", ent->d_name, qdos_filestore_is_dir(root, ent->d_name) ? "/" : "");
		if (written <= 0 || (size_t)written >= sizeof(name)) {
			continue;
		}

		if (!visit(name, user)) {
			break;
		}
	}

	closedir(dir);
	return QDOS_STORE_OK;
}
