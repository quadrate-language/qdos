/**
 * @file test_native.c
 * @brief Loading real shared objects through the native module ABI
 *
 * The modules are built alongside this binary, so the whole path is exercised:
 * descriptor, dynamic linker, function table, and a word called from Quadrate.
 */

#include "check.h"

#include "shell/native.h"

#include <qdos/hal.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef MODULE_DIR
#define MODULE_DIR "."
#endif

/* ---- a store holding whatever the test wants it to hold ----------------- */

#define STORE_MAX 8

static struct {
	const char* name[STORE_MAX];
	size_t count;
} g_scope[QDOS_SCOPE__COUNT];

static void store_reset(void) {
	memset(g_scope, 0, sizeof(g_scope));
}

static void store_put(qdos_store_scope scope, const char* file) {
	if (g_scope[scope].count < STORE_MAX) {
		g_scope[scope].name[g_scope[scope].count++] = file;
	}
}

/** A name with a '/' in it sits in a folder; see stub_list in test_shell.c */
static qdos_store_result stub_list(
		qdos_hal* hal, qdos_store_scope scope, const char* folder, qdos_store_visit visit, void* user) {
	(void)hal;

	for (size_t i = 0; i < g_scope[scope].count; i++) {
		const char* name = g_scope[scope].name[i];
		const char* slash = strchr(name, '/');

		if (folder != NULL) {
			if (slash == NULL || strncmp(name, folder, (size_t)(slash - name)) != 0 || folder[slash - name] != '\0') {
				continue;
			}
			if (!visit(slash + 1, user)) {
				break;
			}
			continue;
		}

		if (slash != NULL) {
			char dir[QDOS_PROGRAM_NAME_MAX];
			snprintf(dir, sizeof(dir), "%.*s/", (int)(slash - name), name);
			if (!visit(dir, user)) {
				break;
			}
			continue;
		}

		if (!visit(name, user)) {
			break;
		}
	}
	return QDOS_STORE_OK;
}

static bool stub_path(qdos_hal* hal, qdos_store_scope scope, const char* name, char* buf, size_t cap) {
	(void)hal;
	(void)scope;

	// The built modules are all in one directory, whatever folder the store
	// says they sit in
	const char* slash = strrchr(name, '/');
	const int written = snprintf(buf, cap, "%s/%s", MODULE_DIR, slash ? slash + 1 : name);
	return written > 0 && (size_t)written < cap;
}

/* Enough of a machine for a program to draw on and read keys from */
static uint8_t g_canvas[QDOS_SCREEN_W * QDOS_SCREEN_H];
static uint8_t g_shown[QDOS_SCREEN_W * QDOS_SCREEN_H];
static int g_presents;
static bool g_alive = true;

static void stub_present(qdos_hal* hal, const uint8_t* fb) {
	(void)hal;
	memcpy(g_shown, fb, sizeof(g_shown));
	g_presents++;
}

static bool stub_poll(qdos_hal* hal, qdos_key_event* out) {
	(void)hal;
	(void)out;
	return false;
}

static bool stub_alive(qdos_hal* hal) {
	(void)hal;
	return g_alive;
}

static uint32_t stub_ticks(qdos_hal* hal) {
	(void)hal;
	return 1234;
}

static void stub_wait(qdos_hal* hal, int timeout_ms) {
	(void)hal;
	(void)timeout_ms;
}

static void stub_hal(qdos_hal* hal) {
	memset(hal, 0, sizeof(*hal));
	hal->store_list = stub_list;
	hal->store_path = stub_path;
	hal->present = stub_present;
	hal->poll_key = stub_poll;
	hal->running = stub_alive;
	hal->ticks_ms = stub_ticks;
	hal->wait = stub_wait;

	memset(g_canvas, 0, sizeof(g_canvas));
	memset(g_shown, 0, sizeof(g_shown));
	g_presents = 0;
	g_alive = true;
	qdos_natives_bind(hal, g_canvas);
}

/* ---- naming ------------------------------------------------------------- */

static void test_module_names(void) {
	char name[QDOS_PROGRAM_NAME_MAX];

	CHECK(qdos_module_name("libfoo.so", name, sizeof(name)));
	CHECK(strcmp(name, "foo") == 0);

	// The shapes that are not a module
	CHECK(!qdos_module_name("foo.qd", name, sizeof(name)));
	CHECK(!qdos_module_name("foo.so", name, sizeof(name)));
	CHECK(!qdos_module_name("libfoo.qd", name, sizeof(name)));
	CHECK(!qdos_module_name("lib.so", name, sizeof(name)));
	CHECK(!qdos_module_name("libfoo.so.1", name, sizeof(name)));
	CHECK(!qdos_module_name("", name, sizeof(name)));
	CHECK(!qdos_module_name(NULL, name, sizeof(name)));

	// A name that could not be written in Quadrate is not one either
	CHECK(!qdos_module_name("libmy-mod.so", name, sizeof(name)));

	char key[QDOS_PROGRAM_NAME_MAX];
	CHECK(qdos_module_key("foo", key, sizeof(key)));
	CHECK(strcmp(key, "libfoo.so") == 0);
	CHECK(!qdos_module_key("my-mod", key, sizeof(key)));
}

/* ---- the happy path ----------------------------------------------------- */

/** Load libdemo.so and call its words from Quadrate source. */
static void test_a_module_brings_words(void) {
	store_reset();
	store_put(QDOS_SCOPE_SYSTEM, "libdemo.so");

	qdos_hal hal;
	stub_hal(&hal);

	qdos_natives set;
	memset(&set, 0, sizeof(set));
	CHECK(qdos_natives_load(&set, &hal, QDOS_SCOPE_SYSTEM) == 1);
	CHECK(set.count == 1);
	CHECK(set.entry[0].error[0] == '\0');
	CHECK(set.entry[0].system);

	qd_interp* interp = qd_interp_create(256);
	CHECK(interp != NULL);
	CHECK(qdos_natives_register(&set, interp) == 6);

	// Scoped by the file it came from
	CHECK(qd_interp_eval(interp, "21 demo::double"));
	qd_interp_value value;
	CHECK(qd_interp_peek(interp, 0, &value));
	CHECK(value.type == QD_INTERP_VALUE_INT);
	CHECK(value.i == 42);

	// Floats through the table, and a result computed inside the module
	CHECK(qd_interp_eval(interp, "clear 3.0 4.0 demo::hypot"));
	CHECK(qd_interp_peek(interp, 0, &value));
	CHECK(value.type == QD_INTERP_VALUE_FLOAT);
	CHECK(value.f > 4.999 && value.f < 5.001);

	// open() ran before any word did
	CHECK(qd_interp_eval(interp, "clear demo::name"));
	CHECK(qd_interp_peek(interp, 0, &value));
	CHECK(strstr(value.text, "opened") != NULL);

	// Strings in, and the depth the module sees is the caller's
	CHECK(qd_interp_eval(interp, "clear \"hello\" demo::sink"));
	CHECK(qd_interp_peek(interp, 0, &value));
	CHECK(value.i == 5);

	CHECK(qd_interp_eval(interp, "clear 1 2 3 demo::deep"));
	CHECK(qd_interp_peek(interp, 0, &value));
	CHECK(value.i == 3);

	qd_interp_destroy(interp);
	qdos_natives_unload(&set);
	CHECK(set.count == 0);
}

/**
 * A module inside an app folder is that app's own half.
 *
 * It loads and its words are callable, but it is marked as belonging to the
 * app so that the list shows the app rather than the library under it.
 */
static void test_a_module_inside_an_app_belongs_to_it(void) {
	store_reset();
	store_put(QDOS_SCOPE_INBOX, "doom/libdemo.so");

	qdos_hal hal;
	stub_hal(&hal);

	qdos_natives set;
	memset(&set, 0, sizeof(set));
	CHECK(qdos_natives_load(&set, &hal, QDOS_SCOPE_INBOX) == 1);
	CHECK(set.count == 1);
	CHECK(strcmp(set.entry[0].app, "doom") == 0);

	// Still scoped by the file, so the app calls it the way anything would
	qd_interp* interp = qd_interp_create(256);
	CHECK(interp != NULL);
	CHECK(qdos_natives_register(&set, interp) == 6);
	CHECK(qd_interp_eval(interp, "21 demo::double"));

	qd_interp_value value;
	CHECK(qd_interp_peek(interp, 0, &value));
	CHECK(value.i == 42);

	qd_interp_destroy(interp);
	qdos_natives_unload(&set);
}

/** One loose on the card belongs to nothing, and is listed in its own right. */
static void test_a_module_on_the_card_belongs_to_no_app(void) {
	store_reset();
	store_put(QDOS_SCOPE_INBOX, "libdemo.so");

	qdos_hal hal;
	stub_hal(&hal);

	qdos_natives set;
	memset(&set, 0, sizeof(set));
	CHECK(qdos_natives_load(&set, &hal, QDOS_SCOPE_INBOX) == 1);
	CHECK(set.entry[0].app[0] == '\0');

	qdos_natives_unload(&set);
}

/** A word that gives up reports through the same line every error uses. */
static void test_a_word_can_fail(void) {
	store_reset();
	store_put(QDOS_SCOPE_SYSTEM, "libdemo.so");

	qdos_hal hal;
	stub_hal(&hal);

	qdos_natives set;
	memset(&set, 0, sizeof(set));
	CHECK(qdos_natives_load(&set, &hal, QDOS_SCOPE_SYSTEM) == 1);

	qd_interp* interp = qd_interp_create(256);
	qdos_natives_register(&set, interp);

	CHECK(!qd_interp_eval(interp, "demo::refuse"));
	CHECK(strstr(qd_interp_error(interp), "AS ADVERTISED") != NULL);

	// The interpreter is still good for the next line
	CHECK(qd_interp_eval(interp, "2 demo::double"));

	qd_interp_destroy(interp);
	qdos_natives_unload(&set);
}

/** The signature is what keeps a word from being entered without its arguments. */
static void test_the_signature_is_checked(void) {
	store_reset();
	store_put(QDOS_SCOPE_SYSTEM, "libdemo.so");

	qdos_hal hal;
	stub_hal(&hal);

	qdos_natives set;
	memset(&set, 0, sizeof(set));
	qdos_natives_load(&set, &hal, QDOS_SCOPE_SYSTEM);

	qd_interp* interp = qd_interp_create(256);
	qdos_natives_register(&set, interp);

	// Nothing on the stack, and `double` takes one
	CHECK(!qd_interp_eval(interp, "demo::double"));

	qd_interp_destroy(interp);
	qdos_natives_unload(&set);
}

/* ---- programs ------------------------------------------------------------ */

/** A module with a main takes its bare name: `app`, not `app::`. */
static void test_a_program_takes_its_bare_name(void) {
	store_reset();
	store_put(QDOS_SCOPE_SYSTEM, "libapp.so");

	qdos_hal hal;
	stub_hal(&hal);

	qdos_natives set;
	memset(&set, 0, sizeof(set));
	CHECK(qdos_natives_load(&set, &hal, QDOS_SCOPE_SYSTEM) == 1);
	CHECK(qdos_native_is_app(&set.entry[0]));

	// The main, and the one word it also offers
	qd_interp* interp = qd_interp_create(256);
	CHECK(qdos_natives_register(&set, interp) == 2);

	CHECK(qd_interp_eval(interp, "app"));
	CHECK(qd_interp_eval(interp, "app::runs"));

	qd_interp_value value;
	CHECK(qd_interp_peek(interp, 0, &value));
	CHECK(value.i == 1); // the bare name ran it, the scoped one only asked

	qd_interp_destroy(interp);
	qdos_natives_unload(&set);
}

/** What it draws on the canvas is what reaches the panel. */
static void test_a_program_draws_on_the_panel(void) {
	store_reset();
	store_put(QDOS_SCOPE_SYSTEM, "libapp.so");

	qdos_hal hal;
	stub_hal(&hal);

	qdos_natives set;
	memset(&set, 0, sizeof(set));
	qdos_natives_load(&set, &hal, QDOS_SCOPE_SYSTEM);

	qd_interp* interp = qd_interp_create(256);
	qdos_natives_register(&set, interp);

	CHECK(g_presents == 0);
	CHECK(qd_interp_eval(interp, "app"));
	CHECK(g_presents == 1);

	// The corner it blacks in, on the panel rather than only in its own memory
	CHECK(g_shown[0] == 0x00);
	CHECK(g_shown[7 * QDOS_SCREEN_W + 7] == 0x00);
	CHECK(g_shown[8 * QDOS_SCREEN_W + 8] == 0xFF);

	qd_interp_destroy(interp);
	qdos_natives_unload(&set);
}

/** A library has no main, and so no bare name to collide with anything. */
static void test_a_library_has_no_bare_name(void) {
	store_reset();
	store_put(QDOS_SCOPE_SYSTEM, "libdemo.so");

	qdos_hal hal;
	stub_hal(&hal);

	qdos_natives set;
	memset(&set, 0, sizeof(set));
	qdos_natives_load(&set, &hal, QDOS_SCOPE_SYSTEM);
	CHECK(!qdos_native_is_app(&set.entry[0]));

	qd_interp* interp = qd_interp_create(256);
	CHECK(qdos_natives_register(&set, interp) == 6); // the words, and nothing more
	CHECK(!qd_interp_eval(interp, "demo"));

	qd_interp_destroy(interp);
	qdos_natives_unload(&set);
}

/* ---- refusals ----------------------------------------------------------- */

/** Each of these is turned away with a reason, and nothing of it runs. */
static void test_a_bad_module_is_refused(void) {
	static const struct {
		const char* file;
		const char* says;
	} BAD[] = {
			{"libbadmagic.so", "NOT A MODULE"},
			{"libbadabi.so", "ABI"},
			{"libbadarch.so", "BUILT FOR"},
			{"libbadwords.so", "NOTHING TO OFFER"}, // neither words nor a main
			{"libbadopen.so", "REFUSED ITSELF"},
			{"libnosym.so", "NO DESCRIPTOR"},
			{"libmissing.so", "WILL NOT LOAD"},
	};

	for (size_t i = 0; i < sizeof(BAD) / sizeof(*BAD); i++) {
		store_reset();
		store_put(QDOS_SCOPE_SYSTEM, BAD[i].file);

		qdos_hal hal;
		stub_hal(&hal);

		qdos_natives set;
		memset(&set, 0, sizeof(set));
		CHECK(qdos_natives_load(&set, &hal, QDOS_SCOPE_SYSTEM) == 0);

		// It is still listed, so the machine can say what happened to it
		CHECK(set.count == 1);
		CHECK(set.entry[0].module == NULL);
		if (strstr(set.entry[0].error, BAD[i].says) == NULL) {
			fprintf(stderr, "  %s said '%s', wanted '%s'\n", BAD[i].file, set.entry[0].error, BAD[i].says);
		}
		CHECK(strstr(set.entry[0].error, BAD[i].says) != NULL);

		// And it brings no words with it
		qd_interp* interp = qd_interp_create(256);
		CHECK(qdos_natives_register(&set, interp) == 0);
		qd_interp_destroy(interp);

		qdos_natives_unload(&set);
	}
}

/** Anything that is not a module is walked past rather than opened. */
static void test_other_files_are_ignored(void) {
	store_reset();
	store_put(QDOS_SCOPE_SYSTEM, "hello.qd");
	store_put(QDOS_SCOPE_SYSTEM, "session");
	store_put(QDOS_SCOPE_SYSTEM, "settings.angle");

	qdos_hal hal;
	stub_hal(&hal);

	qdos_natives set;
	memset(&set, 0, sizeof(set));
	CHECK(qdos_natives_load(&set, &hal, QDOS_SCOPE_SYSTEM) == 0);
	CHECK(set.count == 0);
}

/* ---- scopes ------------------------------------------------------------- */

/** A module found again nearer home replaces the one before it. */
static void test_a_nearer_scope_wins(void) {
	store_reset();
	store_put(QDOS_SCOPE_SYSTEM, "libdemo.so");
	store_put(QDOS_SCOPE_USER, "libdemo.so");

	qdos_hal hal;
	stub_hal(&hal);

	qdos_natives set;
	memset(&set, 0, sizeof(set));
	CHECK(qdos_natives_load(&set, &hal, QDOS_SCOPE_SYSTEM) == 1);
	CHECK(qdos_natives_load(&set, &hal, QDOS_SCOPE_USER) == 1);

	// One module, both origins, so the list can say it is an override
	CHECK(set.count == 1);
	CHECK(set.entry[0].system);
	CHECK(set.entry[0].user);

	// And its words registered once each, not twice
	qd_interp* interp = qd_interp_create(256);
	CHECK(qdos_natives_register(&set, interp) == 6);
	qd_interp_destroy(interp);

	qdos_natives_unload(&set);
}

/** Two modules keep out of each other's way, being scoped by their files. */
static void test_two_modules_do_not_collide(void) {
	store_reset();
	store_put(QDOS_SCOPE_SYSTEM, "libdemo.so");
	store_put(QDOS_SCOPE_SYSTEM, "libother.so");

	qdos_hal hal;
	stub_hal(&hal);

	qdos_natives set;
	memset(&set, 0, sizeof(set));
	CHECK(qdos_natives_load(&set, &hal, QDOS_SCOPE_SYSTEM) == 2);

	qd_interp* interp = qd_interp_create(256);
	CHECK(qdos_natives_register(&set, interp) == 12);

	CHECK(qd_interp_eval(interp, "5 demo::double"));
	CHECK(qd_interp_eval(interp, "clear 5 other::double"));

	qd_interp_destroy(interp);
	qdos_natives_unload(&set);
}

/** A backend that keeps bytes rather than files simply has no modules. */
static void test_without_a_path_there_are_no_modules(void) {
	store_reset();
	store_put(QDOS_SCOPE_SYSTEM, "libdemo.so");

	qdos_hal hal;
	stub_hal(&hal);
	hal.store_path = NULL;

	qdos_natives set;
	memset(&set, 0, sizeof(set));
	CHECK(qdos_natives_load(&set, &hal, QDOS_SCOPE_SYSTEM) == 0);
	CHECK(set.count == 0);
}

/** More modules than there is room for stops rather than overruns. */
static void test_the_set_has_a_limit(void) {
	static const char* NAMES[] = {
			"liba.so", "libb.so", "libc.so", "libd.so", "libe.so", "libf.so", "libg.so", "libh.so"};

	store_reset();
	for (size_t i = 0; i < sizeof(NAMES) / sizeof(*NAMES); i++) {
		store_put(QDOS_SCOPE_SYSTEM, NAMES[i]);
	}
	store_put(QDOS_SCOPE_SYSTEM, "libdemo.so"); // one past the eight

	qdos_hal hal;
	stub_hal(&hal);

	qdos_natives set;
	memset(&set, 0, sizeof(set));
	qdos_natives_load(&set, &hal, QDOS_SCOPE_SYSTEM);
	CHECK(set.count <= QDOS_NATIVE_MAX);

	qdos_natives_unload(&set);
}

int main(void) {
	test_module_names();
	test_a_module_brings_words();
	test_a_module_inside_an_app_belongs_to_it();
	test_a_module_on_the_card_belongs_to_no_app();
	test_a_word_can_fail();
	test_the_signature_is_checked();
	test_a_program_takes_its_bare_name();
	test_a_program_draws_on_the_panel();
	test_a_library_has_no_bare_name();
	test_a_bad_module_is_refused();
	test_other_files_are_ignored();
	test_a_nearer_scope_wins();
	test_two_modules_do_not_collide();
	test_without_a_path_there_are_no_modules();
	test_the_set_has_a_limit();
	return check_report("native");
}
