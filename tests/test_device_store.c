/**
 * @file test_device_store.c
 * @brief The device backend's store, against real directories
 *
 * Every other storage test runs on an in-memory stub. A stub cannot be wrong
 * about the filesystem, so the rules that matter here -- that a name cannot
 * reach outside the store, and that a write cannot touch the system scope --
 * are only really tested where there are directories to get them wrong in.
 */

// POSIX interfaces on top of a strict c11 build
#define _POSIX_C_SOURCE 200809L

#include "check.h"

#include "../src/hal/device/device_linux.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static char g_base[64];
static char g_user[128];
static char g_system[128];

static bool make_dirs(void) {
	snprintf(g_base, sizeof(g_base), "/tmp/qdos-store-XXXXXX");
	if (mkdtemp(g_base) == NULL) {
		fprintf(stderr, "  cannot make a temp directory\n");
		return false;
	}

	snprintf(g_user, sizeof(g_user), "%s/user", g_base);
	snprintf(g_system, sizeof(g_system), "%s/system", g_base);
	return true;
}

static void remove_tree(const char* dir) {
	DIR* d = opendir(dir);
	if (d != NULL) {
		const struct dirent* ent;
		while ((ent = readdir(d)) != NULL) {
			if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
				continue;

			char path[512];
			snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
			if (unlink(path) != 0)
				remove_tree(path);
		}
		closedir(d);
	}
	rmdir(dir);
}

/** Empty both scopes, so no test inherits what another one stored */
static void reset_dirs(void) {
	remove_tree(g_user);
	remove_tree(g_system);

	// Only the system one is made here: the user store is the backend's to
	// create, since a machine out of the box has never been written to
	CHECK(mkdir(g_system, 0755) == 0);
}

/**
 * @brief A backend pointed at the temp directories
 *
 * init() fails: there is no panel here. It settles the store directories
 * before it goes looking for one, and the HAL documents shutdown() as safe
 * after a failed init, so the store is usable on its own.
 */
static void device_hal(qdos_hal* hal) {
	reset_dirs();

	setenv("QDOS_STORE", g_user, 1);
	setenv("QDOS_SYSTEM_STORE", g_system, 1);
	setenv("QDOS_FB", "/nonexistent/qdos-test-fb", 1);

	qdos_device_hal(hal);
	CHECK(hal->init(hal) != 0);
}

/** Write a file into a scope directory without going through the backend */
static void seed(const char* dir, const char* name, const char* text) {
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", dir, name);

	FILE* f = fopen(path, "wb");
	CHECK(f != NULL);
	if (!f)
		return;
	CHECK(fwrite(text, 1, strlen(text), f) == strlen(text));
	fclose(f);
}

static bool file_exists(const char* path) {
	struct stat sb;
	return stat(path, &sb) == 0;
}

/** What goes in comes back out, byte for byte. */
static void test_write_then_read(void) {
	qdos_hal hal;
	device_hal(&hal);

	static const char WRITTEN[] = "fn hyp( -- r:i64) { 5 }";
	CHECK(hal.store_write(&hal, "hyp.qd", WRITTEN, sizeof(WRITTEN) - 1) == QDOS_STORE_OK);

	// A machine with nothing stored yet has no store directory, so the first
	// write has to make one
	CHECK(file_exists(g_user));

	char buf[256];
	size_t len = 0;
	CHECK(hal.store_read(&hal, QDOS_SCOPE_USER, "hyp.qd", buf, sizeof(buf), &len) == QDOS_STORE_OK);
	CHECK(len == sizeof(WRITTEN) - 1);
	CHECK(memcmp(buf, WRITTEN, len) == 0);

	// Writing again replaces, rather than appending or leaving a tail behind
	CHECK(hal.store_write(&hal, "hyp.qd", "1", 1) == QDOS_STORE_OK);
	CHECK(hal.store_read(&hal, QDOS_SCOPE_USER, "hyp.qd", buf, sizeof(buf), &len) == QDOS_STORE_OK);
	CHECK(len == 1);
	CHECK(buf[0] == '1');

	// Nothing was ever written under this name
	CHECK(hal.store_read(&hal, QDOS_SCOPE_USER, "absent.qd", buf, sizeof(buf), &len) == QDOS_STORE_NOT_FOUND);

	hal.shutdown(&hal);
}

/** A record too big for the buffer says so instead of handing back a piece. */
static void test_too_big_for_the_buffer(void) {
	qdos_hal hal;
	device_hal(&hal);

	char ten[10];
	memset(ten, 'x', sizeof(ten));
	CHECK(hal.store_write(&hal, "ten", ten, sizeof(ten)) == QDOS_STORE_OK);

	char buf[16];
	size_t len = 0;

	// Room to spare
	CHECK(hal.store_read(&hal, QDOS_SCOPE_USER, "ten", buf, sizeof(buf), &len) == QDOS_STORE_OK);
	CHECK(len == 10);

	// An exact fit is not too big, which is the edge worth pinning down
	CHECK(hal.store_read(&hal, QDOS_SCOPE_USER, "ten", buf, 10, &len) == QDOS_STORE_OK);
	CHECK(len == 10);

	// One short is
	CHECK(hal.store_read(&hal, QDOS_SCOPE_USER, "ten", buf, 9, &len) == QDOS_STORE_TOO_BIG);

	// The length is optional, since a caller may only want the bytes
	CHECK(hal.store_read(&hal, QDOS_SCOPE_USER, "ten", buf, sizeof(buf), NULL) == QDOS_STORE_OK);

	hal.shutdown(&hal);
}

/**
 * A name is a name, not a path. Register keys come from what the user typed,
 * so a name that walks out of the store must be refused rather than followed.
 */
static void test_a_name_cannot_leave_the_store(void) {
	qdos_hal hal;
	device_hal(&hal);

	char buf[64];
	static const char* const REFUSED[] = {
			"",
			"..",
			"../escaped",
			"../../etc/passwd",
			"sub/dir",
			"sub\\dir",
			"/absolute",
	};

	for (size_t i = 0; i < sizeof(REFUSED) / sizeof(*REFUSED); i++) {
		CHECK(hal.store_write(&hal, REFUSED[i], "x", 1) == QDOS_STORE_IO_ERROR);
		CHECK(hal.store_read(&hal, QDOS_SCOPE_USER, REFUSED[i], buf, sizeof(buf), NULL) == QDOS_STORE_IO_ERROR);
	}

	// A refusal is not a quiet write somewhere else
	char escaped[256];
	snprintf(escaped, sizeof(escaped), "%s/escaped", g_base);
	CHECK(!file_exists(escaped));

	// A name longer than the path it would compose is refused too, rather than
	// silently writing to the truncation
	char huge[700];
	memset(huge, 'n', sizeof(huge) - 1);
	huge[sizeof(huge) - 1] = '\0';
	CHECK(hal.store_write(&hal, huge, "x", 1) == QDOS_STORE_IO_ERROR);

	hal.shutdown(&hal);
}

/** The two scopes are two directories, and a read says which one it means. */
static void test_the_scopes_are_separate(void) {
	qdos_hal hal;
	device_hal(&hal);

	seed(g_system, "shipped.qd", "SYSTEM");

	char buf[64];
	size_t len = 0;
	CHECK(hal.store_read(&hal, QDOS_SCOPE_SYSTEM, "shipped.qd", buf, sizeof(buf), &len) == QDOS_STORE_OK);
	CHECK(len == 6);
	CHECK(memcmp(buf, "SYSTEM", 6) == 0);

	// The same name is absent from the other scope
	CHECK(hal.store_read(&hal, QDOS_SCOPE_USER, "shipped.qd", buf, sizeof(buf), &len) == QDOS_STORE_NOT_FOUND);

	hal.shutdown(&hal);
}

/**
 * A write cannot reach the system scope. store_write takes no scope at all,
 * which is the whole guarantee -- but the guarantee is about which directory
 * it picks, and only a real directory can show that.
 */
static void test_a_write_cannot_touch_the_system_store(void) {
	qdos_hal hal;
	device_hal(&hal);

	seed(g_system, "both.qd", "SYSTEM");

	CHECK(hal.store_write(&hal, "both.qd", "USER", 4) == QDOS_STORE_OK);

	char buf[64];
	size_t len = 0;

	// The shipped copy is as it was
	CHECK(hal.store_read(&hal, QDOS_SCOPE_SYSTEM, "both.qd", buf, sizeof(buf), &len) == QDOS_STORE_OK);
	CHECK(len == 6);
	CHECK(memcmp(buf, "SYSTEM", 6) == 0);

	// And the override sits beside it, in the user scope
	CHECK(hal.store_read(&hal, QDOS_SCOPE_USER, "both.qd", buf, sizeof(buf), &len) == QDOS_STORE_OK);
	CHECK(len == 4);
	CHECK(memcmp(buf, "USER", 4) == 0);

	hal.shutdown(&hal);
}

/* ---------------------------------------------------------------------- */
/* Enumeration                                                            */
/* ---------------------------------------------------------------------- */

#define SEEN_MAX 16

typedef struct {
	char names[SEEN_MAX][64];
	int count;
	int stop_after; ///< Return false once this many have been handed over
} visitor;

static bool collect(const char* name, void* user) {
	visitor* v = user;
	if (v->count < SEEN_MAX)
		snprintf(v->names[v->count], sizeof(v->names[0]), "%s", name);
	v->count++;
	return !(v->stop_after > 0 && v->count >= v->stop_after);
}

static bool saw(const visitor* v, const char* name) {
	for (int i = 0; i < v->count && i < SEEN_MAX; i++) {
		if (strcmp(v->names[i], name) == 0)
			return true;
	}
	return false;
}

/** Listing walks one scope's directory, and skips what is not a record. */
static void test_list_walks_a_scope(void) {
	qdos_hal hal;
	device_hal(&hal);

	CHECK(hal.store_write(&hal, "one.qd", "1", 1) == QDOS_STORE_OK);
	CHECK(hal.store_write(&hal, "two.qd", "2", 1) == QDOS_STORE_OK);
	seed(g_system, "three.qd", "3");
	seed(g_system, ".hidden", "no"); // and whatever else the rootfs leaves lying about

	visitor user = {0};
	CHECK(hal.store_list(&hal, QDOS_SCOPE_USER, collect, &user) == QDOS_STORE_OK);
	CHECK(user.count == 2);
	CHECK(saw(&user, "one.qd"));
	CHECK(saw(&user, "two.qd"));
	CHECK(!saw(&user, "three.qd")); // the other scope is not this one

	visitor system = {0};
	CHECK(hal.store_list(&hal, QDOS_SCOPE_SYSTEM, collect, &system) == QDOS_STORE_OK);
	CHECK(system.count == 1);
	CHECK(saw(&system, "three.qd"));
	CHECK(!saw(&system, ".hidden"));
	CHECK(!saw(&system, ".")); // nor the directory itself

	// A visitor that has seen enough stops the walk
	visitor early = {.stop_after = 1};
	CHECK(hal.store_list(&hal, QDOS_SCOPE_USER, collect, &early) == QDOS_STORE_OK);
	CHECK(early.count == 1);

	hal.shutdown(&hal);
}

/** A scope with no directory behind it is empty, not broken. */
static void test_list_of_a_missing_directory(void) {
	char missing[128];
	snprintf(missing, sizeof(missing), "%s/never-made", g_base);

	qdos_hal hal;
	device_hal(&hal);
	setenv("QDOS_SYSTEM_STORE", missing, 1);
	CHECK(hal.init(&hal) != 0); // again, to pick the new directory up

	visitor v = {0};
	CHECK(hal.store_list(&hal, QDOS_SCOPE_SYSTEM, collect, &v) == QDOS_STORE_NOT_FOUND);
	CHECK(v.count == 0);

	char buf[64];
	CHECK(hal.store_read(&hal, QDOS_SCOPE_SYSTEM, "anything", buf, sizeof(buf), NULL) == QDOS_STORE_NOT_FOUND);

	hal.shutdown(&hal);
}

int main(void) {
	if (!make_dirs())
		return 1;

	// init() has no panel to find here and says so on stderr, once per test
	printf("device_store: the framebuffer complaints below are the point\n");

	test_write_then_read();
	test_too_big_for_the_buffer();
	test_a_name_cannot_leave_the_store();
	test_the_scopes_are_separate();
	test_a_write_cannot_touch_the_system_store();
	test_list_walks_a_scope();
	test_list_of_a_missing_directory();

	remove_tree(g_base);
	return check_report("device_store");
}
