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
static char g_inbox[128];

/* Stands in for /usr/bin/qdos-usb. Kept outside the scope directories so
 * clearing those between tests does not take it with them. */
static char g_helper[192];
static char g_helper_log[192];

static bool make_dirs(void) {
	snprintf(g_base, sizeof(g_base), "/tmp/qdos-store-XXXXXX");
	if (mkdtemp(g_base) == NULL) {
		fprintf(stderr, "  cannot make a temp directory\n");
		return false;
	}

	snprintf(g_user, sizeof(g_user), "%s/user", g_base);
	snprintf(g_system, sizeof(g_system), "%s/system", g_base);
	snprintf(g_inbox, sizeof(g_inbox), "%s/inbox", g_base);
	snprintf(g_helper, sizeof(g_helper), "%s/qdos-usb", g_base);
	snprintf(g_helper_log, sizeof(g_helper_log), "%s/usb.log", g_base);
	return true;
}

/** Install a stand-in helper that records its argument and exits with @p code */
static void write_helper(int code) {
	FILE* f = fopen(g_helper, "w");
	CHECK(f != NULL);
	if (!f) {
		return;
	}

	fprintf(f, "#!/bin/sh\necho \"$1\" >> %s\nexit %d\n", g_helper_log, code);
	fclose(f);
	CHECK(chmod(g_helper, 0755) == 0);
	unlink(g_helper_log);
}

/** What the helper was last asked to do, or "" if it was never run */
static void helper_log(char* out, size_t cap) {
	out[0] = '\0';

	FILE* f = fopen(g_helper_log, "r");
	if (!f) {
		return;
	}

	const size_t got = fread(out, 1, cap - 1, f);
	out[got] = '\0';
	fclose(f);
}

static void remove_tree(const char* dir) {
	DIR* d = opendir(dir);
	if (d != NULL) {
		const struct dirent* ent;
		while ((ent = readdir(d)) != NULL) {
			if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
				continue;
			}

			char path[512];
			snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
			if (unlink(path) != 0) {
				remove_tree(path);
			}
		}
		closedir(d);
	}
	rmdir(dir);
}

/** Empty every scope, so no test inherits what another one stored */
static void reset_dirs(void) {
	remove_tree(g_user);
	remove_tree(g_system);
	remove_tree(g_inbox);

	// The user store is the backend's to create, since a machine out of the
	// box has never been written to. The other two are mounted, not made.
	CHECK(mkdir(g_system, 0755) == 0);
	CHECK(mkdir(g_inbox, 0755) == 0);
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
	setenv("QDOS_INBOX", g_inbox, 1);
	setenv("QDOS_USB_HELPER", g_helper, 1);
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
	if (!f) {
		return;
	}
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
	// One folder deep is an app and is allowed; everything here is not
	static const char* const REFUSED[] = {
			"",
			"..",
			".",
			"../escaped",
			"../../etc/passwd",
			"app/../escaped",
			"one/two/three",
			"app/",
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

/** Each scope is its own directory, and a read says which one it means. */
static void test_the_scopes_are_separate(void) {
	qdos_hal hal;
	device_hal(&hal);

	seed(g_system, "shipped.qd", "SYSTEM");
	seed(g_inbox, "uploaded.qd", "INBOX");

	char buf[64];
	size_t len = 0;
	CHECK(hal.store_read(&hal, QDOS_SCOPE_SYSTEM, "shipped.qd", buf, sizeof(buf), &len) == QDOS_STORE_OK);
	CHECK(len == 6);
	CHECK(memcmp(buf, "SYSTEM", 6) == 0);

	CHECK(hal.store_read(&hal, QDOS_SCOPE_INBOX, "uploaded.qd", buf, sizeof(buf), &len) == QDOS_STORE_OK);
	CHECK(len == 5);
	CHECK(memcmp(buf, "INBOX", 5) == 0);

	// Each name is absent from the scopes it does not belong to
	CHECK(hal.store_read(&hal, QDOS_SCOPE_USER, "shipped.qd", buf, sizeof(buf), &len) == QDOS_STORE_NOT_FOUND);
	CHECK(hal.store_read(&hal, QDOS_SCOPE_INBOX, "shipped.qd", buf, sizeof(buf), &len) == QDOS_STORE_NOT_FOUND);
	CHECK(hal.store_read(&hal, QDOS_SCOPE_SYSTEM, "uploaded.qd", buf, sizeof(buf), &len) == QDOS_STORE_NOT_FOUND);
	CHECK(hal.store_read(&hal, QDOS_SCOPE_USER, "uploaded.qd", buf, sizeof(buf), &len) == QDOS_STORE_NOT_FOUND);

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

/**
 * Nor the inbox. The card is a PC's to write, and editing an uploaded program
 * on the calculator must leave the file that was uploaded alone.
 */
static void test_a_write_cannot_touch_the_inbox(void) {
	qdos_hal hal;
	device_hal(&hal);

	seed(g_inbox, "both.qd", "CARD");

	CHECK(hal.store_write(&hal, "both.qd", "EDITED", 6) == QDOS_STORE_OK);

	char buf[64];
	size_t len = 0;

	CHECK(hal.store_read(&hal, QDOS_SCOPE_INBOX, "both.qd", buf, sizeof(buf), &len) == QDOS_STORE_OK);
	CHECK(len == 4);
	CHECK(memcmp(buf, "CARD", 4) == 0);

	CHECK(hal.store_read(&hal, QDOS_SCOPE_USER, "both.qd", buf, sizeof(buf), &len) == QDOS_STORE_OK);
	CHECK(len == 6);
	CHECK(memcmp(buf, "EDITED", 6) == 0);

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
	if (v->count < SEEN_MAX) {
		snprintf(v->names[v->count], sizeof(v->names[0]), "%s", name);
	}
	v->count++;
	return !(v->stop_after > 0 && v->count >= v->stop_after);
}

static bool saw(const visitor* v, const char* name) {
	for (int i = 0; i < v->count && i < SEEN_MAX; i++) {
		if (strcmp(v->names[i], name) == 0) {
			return true;
		}
	}
	return false;
}

/** An app is a folder, so writing into one has to make it, and listing show it. */
static void test_an_app_folder_is_written_and_listed(void) {
	qdos_hal hal;
	device_hal(&hal);

	CHECK(hal.store_write(&hal, "doom/main.qd", "fn main( -- ) { }", 17) == QDOS_STORE_OK);

	char buf[64];
	size_t len = 0;
	CHECK(hal.store_read(&hal, QDOS_SCOPE_USER, "doom/main.qd", buf, sizeof(buf), &len) == QDOS_STORE_OK);
	CHECK(len == 17);

	// The card shows the folder with the mark on it, not what is inside it
	visitor top = {.count = 0, .stop_after = 0};
	CHECK(hal.store_list(&hal, QDOS_SCOPE_USER, NULL, collect, &top) == QDOS_STORE_OK);
	CHECK(saw(&top, "doom/"));
	CHECK(!saw(&top, "main.qd"));

	// Naming the folder lists what it holds, bare
	visitor inside = {.count = 0, .stop_after = 0};
	CHECK(hal.store_list(&hal, QDOS_SCOPE_USER, "doom", collect, &inside) == QDOS_STORE_OK);
	CHECK(saw(&inside, "main.qd"));

	hal.shutdown(&hal);
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
	CHECK(hal.store_list(&hal, QDOS_SCOPE_USER, NULL, collect, &user) == QDOS_STORE_OK);
	CHECK(user.count == 2);
	CHECK(saw(&user, "one.qd"));
	CHECK(saw(&user, "two.qd"));
	CHECK(!saw(&user, "three.qd")); // the other scope is not this one

	visitor system = {0};
	CHECK(hal.store_list(&hal, QDOS_SCOPE_SYSTEM, NULL, collect, &system) == QDOS_STORE_OK);
	CHECK(system.count == 1);
	CHECK(saw(&system, "three.qd"));
	CHECK(!saw(&system, ".hidden"));
	CHECK(!saw(&system, ".")); // nor the directory itself

	seed(g_inbox, "four.qd", "4");
	visitor inbox = {0};
	CHECK(hal.store_list(&hal, QDOS_SCOPE_INBOX, NULL, collect, &inbox) == QDOS_STORE_OK);
	CHECK(inbox.count == 1);
	CHECK(saw(&inbox, "four.qd"));

	// A visitor that has seen enough stops the walk
	visitor early = {.stop_after = 1};
	CHECK(hal.store_list(&hal, QDOS_SCOPE_USER, NULL, collect, &early) == QDOS_STORE_OK);
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
	CHECK(hal.store_list(&hal, QDOS_SCOPE_SYSTEM, NULL, collect, &v) == QDOS_STORE_NOT_FOUND);
	CHECK(v.count == 0);

	char buf[64];
	CHECK(hal.store_read(&hal, QDOS_SCOPE_SYSTEM, "anything", buf, sizeof(buf), NULL) == QDOS_STORE_NOT_FOUND);

	hal.shutdown(&hal);
}

/**
 * While the card is shared over USB it is unmounted, so the inbox is simply
 * not there. The shell reads it whenever the APPS list is opened, so an absent
 * scope has to read as empty rather than as a failure.
 */
static void test_a_shared_inbox_reads_as_empty(void) {
	qdos_hal hal;
	device_hal(&hal);
	seed(g_inbox, "gone.qd", "1");

	// As the helper leaves it: the mount point is there, the contents are not
	remove_tree(g_inbox);

	char buf[64];
	CHECK(hal.store_read(&hal, QDOS_SCOPE_INBOX, "gone.qd", buf, sizeof(buf), NULL) == QDOS_STORE_NOT_FOUND);

	visitor v = {0};
	CHECK(hal.store_list(&hal, QDOS_SCOPE_INBOX, NULL, collect, &v) == QDOS_STORE_NOT_FOUND);
	CHECK(v.count == 0);

	// The other scopes are unaffected, so the calculator still works
	CHECK(hal.store_write(&hal, "still.qd", "1", 1) == QDOS_STORE_OK);
	CHECK(hal.store_read(&hal, QDOS_SCOPE_USER, "still.qd", buf, sizeof(buf), NULL) == QDOS_STORE_OK);

	hal.shutdown(&hal);
}

/* ---------------------------------------------------------------------- */
/* Handing the inbox to a PC                                              */
/*                                                                        */
/* The backend does not touch the gadget itself: it runs a script, so the  */
/* mounting and binding can be fixed on the machine. What is tested here   */
/* is that the script is run, with the right word, and that what it        */
/* reports comes back to the caller.                                      */
/* ---------------------------------------------------------------------- */

static void test_usb_runs_the_helper(void) {
	write_helper(0);

	qdos_hal hal;
	device_hal(&hal);
	CHECK(hal.usb_export != NULL);
	if (!hal.usb_export) {
		return;
	}

	char log[128];
	CHECK(hal.usb_export(&hal, true) == 0);
	helper_log(log, sizeof(log));
	CHECK_STR(log, "share\n");

	CHECK(hal.usb_export(&hal, false) == 0);
	helper_log(log, sizeof(log));
	CHECK_STR(log, "share\ntake\n");

	hal.shutdown(&hal);
}

/** A helper that fails says so, rather than the shell believing it worked. */
static void test_a_failing_helper_is_reported(void) {
	write_helper(1);

	qdos_hal hal;
	device_hal(&hal);
	CHECK(hal.usb_export != NULL);
	if (!hal.usb_export) {
		return;
	}

	CHECK(hal.usb_export(&hal, true) == -1);

	char log[128];
	helper_log(log, sizeof(log));
	CHECK_STR(log, "share\n"); // it really ran; it just did not succeed

	hal.shutdown(&hal);
}

/** No helper, no setting: the shell is told there is no gadget here. */
static void test_usb_is_not_offered_without_a_helper(void) {
	unlink(g_helper);

	qdos_hal hal;
	device_hal(&hal);
	CHECK(hal.usb_export == NULL);

	hal.shutdown(&hal);
}

/** Nor when the file is there but cannot be run. */
static void test_usb_is_not_offered_for_a_helper_that_cannot_run(void) {
	write_helper(0);
	CHECK(chmod(g_helper, 0644) == 0);

	qdos_hal hal;
	device_hal(&hal);
	CHECK(hal.usb_export == NULL);

	hal.shutdown(&hal);
	unlink(g_helper);
}

int main(void) {
	if (!make_dirs()) {
		return 1;
	}

	// init() has no panel to find here and says so on stderr, once per test
	printf("device_store: the framebuffer complaints below are the point\n");

	test_write_then_read();
	test_too_big_for_the_buffer();
	test_a_name_cannot_leave_the_store();
	test_an_app_folder_is_written_and_listed();
	test_the_scopes_are_separate();
	test_a_write_cannot_touch_the_system_store();
	test_a_write_cannot_touch_the_inbox();
	test_list_walks_a_scope();
	test_list_of_a_missing_directory();
	test_a_shared_inbox_reads_as_empty();
	test_usb_runs_the_helper();
	test_a_failing_helper_is_reported();
	test_usb_is_not_offered_without_a_helper();
	test_usb_is_not_offered_for_a_helper_that_cannot_run();

	remove_tree(g_base);
	return check_report("device_store");
}
