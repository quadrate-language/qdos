/**
 * @file storename.c
 * @brief The one rule both backends have to agree on about store names
 */

#include <qdos/hal.h>

#include <string.h>

static bool component_ok(const char* start, size_t len) {
	if (len == 0) {
		return false;
	}
	if (len == 1 && start[0] == '.') {
		return false;
	}
	if (len == 2 && start[0] == '.' && start[1] == '.') {
		return false;
	}
	return true;
}

bool qdos_store_name_ok(const char* name) {
	if (name == NULL || *name == '\0' || strchr(name, '\\') != NULL) {
		return false;
	}

	const char* slash = strchr(name, '/');
	if (slash == NULL) {
		return component_ok(name, strlen(name));
	}

	// One level and no more: an app folder holds files, not other apps
	if (strchr(slash + 1, '/') != NULL) {
		return false;
	}

	return component_ok(name, (size_t)(slash - name)) && component_ok(slash + 1, strlen(slash + 1));
}
