/**
 * @file native.c
 * @brief Loading the native modules found in the stores
 */

// dlopen and friends on top of a strict c11 build
#define _POSIX_C_SOURCE 200809L

#include "native.h"

#include <quadrate/rt/runtime.h>

#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Shims, not the runtime's own functions: a module never sees a qd_context. */

static int api_push_int(qdos_native_ctx* ctx, int64_t value) {
	return qd_push_i((qd_context*)ctx, value);
}

static int api_push_float(qdos_native_ctx* ctx, double value) {
	return qd_push_f((qd_context*)ctx, value);
}

static int api_push_str(qdos_native_ctx* ctx, const char* value) {
	return qd_push_s((qd_context*)ctx, value);
}

static int api_pop_int(qdos_native_ctx* ctx, int64_t* out) {
	return qd_pop_i((qd_context*)ctx, out);
}

static int api_pop_float(qdos_native_ctx* ctx, double* out) {
	return qd_pop_f((qd_context*)ctx, out);
}

static int api_pop_str(qdos_native_ctx* ctx, char* buf, size_t cap) {
	return qd_pop_s((qd_context*)ctx, buf, cap);
}

static size_t api_depth(qdos_native_ctx* ctx) {
	const qd_context* c = (const qd_context*)ctx;
	return qd_stack_size(c->st);
}

static void api_fail(qdos_native_ctx* ctx, const char* message) {
	qd_set_error_msg((qd_context*)ctx, message ? message : "MODULE REFUSED");
}

/* One shell, one machine, so these are bound once rather than threaded through
 * every registration. */
static qdos_hal* g_hal;
static uint8_t* g_canvas;

/** PWR came while a program had the keypad; cleared on the next rearm */
static bool g_break;

bool qdos_natives_key(qdos_hal* hal, qdos_key_event* out) {
	if (g_break || !hal->poll_key(hal, out)) {
		return false;
	}

	// Never the program's to ignore: it is how one that will not stop is stopped
	if (out->key == QDOS_KEY_POWER) {
		g_break = true;
		return false;
	}
	return true;
}

bool qdos_natives_broken(void) {
	return g_break;
}

void qdos_natives_bind(qdos_hal* hal, uint8_t* canvas) {
	g_hal = hal;
	g_canvas = canvas;
}

static uint8_t* api_canvas(qdos_native_ctx* ctx, int* width, int* height) {
	(void)ctx;
	if (width != NULL) {
		*width = QDOS_SCREEN_W;
	}
	if (height != NULL) {
		*height = QDOS_SCREEN_H;
	}
	return g_canvas;
}

static void api_present(qdos_native_ctx* ctx) {
	(void)ctx;
	if (g_hal != NULL && g_canvas != NULL) {
		g_hal->present(g_hal, g_canvas);
	}
}

static bool api_key(qdos_native_ctx* ctx, qdos_key* key, char* ch) {
	(void)ctx;
	if (g_hal == NULL) {
		return false;
	}

	qdos_key_event event;
	if (!qdos_natives_key(g_hal, &event)) {
		return false;
	}

	if (key != NULL) {
		*key = event.key;
	}
	if (ch != NULL) {
		*ch = event.ch;
	}
	return true;
}

static uint32_t api_ticks(qdos_native_ctx* ctx) {
	(void)ctx;
	return (g_hal != NULL) ? g_hal->ticks_ms(g_hal) : 0;
}

static void api_wait(qdos_native_ctx* ctx, int timeout_ms) {
	(void)ctx;
	if (g_hal != NULL) {
		g_hal->wait(g_hal, timeout_ms);
	}
}

/** @brief The app the word now running belongs to, or empty for a loose module */
static const char* g_app = "";

static bool readable(qdos_store_scope scope, const char* key, char* buf, size_t cap) {
	return g_hal->store_path(g_hal, scope, key, buf, cap) && access(buf, R_OK) == 0;
}

static bool api_path(qdos_native_ctx* ctx, const char* name, char* buf, size_t cap) {
	(void)ctx;
	if (g_hal == NULL || g_hal->store_path == NULL || name == NULL) {
		return false;
	}

	// Nearest first, as a program of one name shadows another
	static const qdos_store_scope ORDER[] = {QDOS_SCOPE_USER, QDOS_SCOPE_INBOX, QDOS_SCOPE_SYSTEM};

	char key[QDOS_PROGRAM_NAME_MAX * 2];
	const bool inside = g_app[0] != '\0' && qdos_app_key(g_app, name, key, sizeof(key));

	for (size_t i = 0; i < sizeof(ORDER) / sizeof(*ORDER); i++) {
		// What an app brought with it is in its own folder; anything else it
		// asks for is loose on the card
		if (inside && readable(ORDER[i], key, buf, cap)) {
			return true;
		}
		if (readable(ORDER[i], name, buf, cap)) {
			return true;
		}
	}

	buf[0] = '\0';
	return false;
}

static bool api_running(qdos_native_ctx* ctx) {
	(void)ctx;
	return (g_hal != NULL) && g_hal->running(g_hal) && !g_break;
}

static const qdos_native_api API = {
		.push_int = api_push_int,
		.push_float = api_push_float,
		.push_str = api_push_str,
		.pop_int = api_pop_int,
		.pop_float = api_pop_float,
		.pop_str = api_pop_str,
		.depth = api_depth,
		.fail = api_fail,
		.canvas = api_canvas,
		.present = api_present,
		.key = api_key,
		.ticks = api_ticks,
		.wait = api_wait,
		.running = api_running,
		.path = api_path,
};

static void (*g_on_call)(void* user);
static void* g_on_call_user;
static bool g_called;

void qdos_natives_on_call(void (*fn)(void* user), void* user) {
	g_on_call = fn;
	g_on_call_user = user;
	g_called = false;
}

void qdos_natives_rearm(void) {
	g_called = false;
	g_break = false;
}

/** @brief A module stopped by PWR returns as asked; the program above it must stop too */
static int broken_or(qd_context* ctx, int result) {
	if (g_break) {
		qd_set_error_msg(ctx, "BREAK");
		return 1;
	}
	return result;
}

/** @brief The set the registered words came from, for finding what owns one */
static const qdos_natives* g_set;

/** @brief Which module a word belongs to; its words sit inside its own table */
static const char* app_of(const qdos_native_word* word) {
	if (g_set == NULL) {
		return "";
	}

	for (size_t i = 0; i < g_set->count; i++) {
		const qdos_native_entry* entry = &g_set->entry[i];
		if (entry->module == NULL || entry->module->words == NULL) {
			continue;
		}
		if (word >= entry->module->words && word < entry->module->words + entry->module->word_count) {
			return entry->app;
		}
	}
	return "";
}

/** @brief The interpreter calls this; the module's own function is the userdata */
static int call_word(qd_context* ctx, void* userdata) {
	const qdos_native_word* word = (const qdos_native_word*)userdata;

	if (!g_called) {
		g_called = true;
		if (g_on_call != NULL) {
			g_on_call(g_on_call_user);
		}
	}

	const char* was = g_app;
	g_app = app_of(word);
	const int result = word->fn((qdos_native_ctx*)ctx, &API);
	g_app = was;

	return broken_or(ctx, result);
}

/* See qdos_natives_recover() in native.h for why these exist. */
#define NATIVE_LOADING_KEY "native.loading"
#define NATIVE_BLOCKED_KEY "native.blocked"

static void read_text(qdos_hal* hal, const char* key, char* out, size_t cap) {
	out[0] = '\0';
	if (hal->store_read == NULL) {
		return;
	}

	size_t len = 0;
	if (hal->store_read(hal, QDOS_SCOPE_USER, key, out, cap - 1, &len) != QDOS_STORE_OK) {
		return;
	}

	out[len < cap ? len : cap - 1] = '\0';
}

static void write_text(qdos_hal* hal, const char* key, const char* text) {
	if (hal->store_write == NULL) {
		return;
	}
	hal->store_write(hal, key, text, strlen(text));
}

/** @brief Whole name in the space separated list, not a substring of one */
static bool listed(const char* list, const char* name) {
	const size_t len = strlen(name);

	for (const char* p = list; *p != '\0';) {
		const char* end = strchr(p, ' ');
		const size_t span = (end != NULL) ? (size_t)(end - p) : strlen(p);
		if (span == len && strncmp(p, name, len) == 0) {
			return true;
		}
		if (end == NULL) {
			break;
		}
		p = end + 1;
	}
	return false;
}

static void list_add(char* list, size_t cap, const char* name) {
	if (listed(list, name)) {
		return;
	}

	const size_t used = strlen(list);
	snprintf(list + used, cap - used, "%s%s", used > 0 ? " " : "", name);
}

bool qdos_natives_recover(qdos_natives* set, qdos_hal* hal) {
	if (set == NULL || hal == NULL || hal->store_read == NULL || hal->store_write == NULL) {
		return false;
	}

	read_text(hal, NATIVE_BLOCKED_KEY, set->blocked, sizeof(set->blocked));

	char crumb[QDOS_PROGRAM_NAME_MAX];
	read_text(hal, NATIVE_LOADING_KEY, crumb, sizeof(crumb));
	if (crumb[0] == '\0') {
		return false;
	}

	list_add(set->blocked, sizeof(set->blocked), crumb);
	write_text(hal, NATIVE_BLOCKED_KEY, set->blocked);
	write_text(hal, NATIVE_LOADING_KEY, "");

	snprintf(set->faulted, sizeof(set->faulted), "%s", crumb);
	return true;
}

void qdos_natives_unblock(qdos_natives* set, qdos_hal* hal) {
	if (set == NULL) {
		return;
	}

	set->blocked[0] = '\0';
	set->faulted[0] = '\0';

	if (hal != NULL && hal->store_write != NULL) {
		write_text(hal, NATIVE_BLOCKED_KEY, "");
	}
}

size_t qdos_natives_blocked_count(const qdos_natives* set) {
	if (set == NULL || set->blocked[0] == '\0') {
		return 0;
	}

	size_t n = 1;
	for (const char* p = set->blocked; *p != '\0'; p++) {
		if (*p == ' ') {
			n++;
		}
	}
	return n;
}

/* ------------------------------------------------------------------------ */
/* Loading                                                                  */
/* ------------------------------------------------------------------------ */

/** @brief Nothing of the module has run yet. Reasons fit the message line. */
static bool accept(const qdos_native_module* module, char* error, size_t cap) {
	if (module == NULL) {
		snprintf(error, cap, "NO DESCRIPTOR");
		return false;
	}
	if (module->magic != QDOS_NATIVE_MAGIC) {
		snprintf(error, cap, "NOT A MODULE");
		return false;
	}
	if (module->abi != QDOS_NATIVE_ABI) {
		snprintf(error, cap, "ABI %u, WANTED %u", module->abi, QDOS_NATIVE_ABI);
		return false;
	}
	if (module->arch == NULL || strcmp(module->arch, QDOS_NATIVE_ARCH) != 0) {
		snprintf(error, cap, "BUILT FOR %.10s", module->arch ? module->arch : "?");
		return false;
	}
	if (module->main == NULL && (module->words == NULL || module->word_count == 0)) {
		snprintf(error, cap, "NOTHING TO OFFER");
		return false;
	}
	if (module->word_count > QDOS_NATIVE_WORDS_MAX) {
		snprintf(error, cap, "OVER %d WORDS", QDOS_NATIVE_WORDS_MAX);
		return false;
	}
	if (module->word_count > 0 && module->words == NULL) {
		snprintf(error, cap, "WORDS ARE MISSING");
		return false;
	}

	for (size_t i = 0; i < module->word_count; i++) {
		const qdos_native_word* word = &module->words[i];
		if (word->name == NULL || word->signature == NULL || word->fn == NULL) {
			snprintf(error, cap, "WORD %zu IS EMPTY", i + 1);
			return false;
		}
	}

	error[0] = '\0';
	return true;
}

static qdos_native_entry* slot_for(qdos_natives* set, const char* name) {
	for (size_t i = 0; i < set->count; i++) {
		if (strcmp(set->entry[i].name, name) == 0) {
			return &set->entry[i];
		}
	}

	if (set->count >= QDOS_NATIVE_MAX) {
		return NULL;
	}

	qdos_native_entry* entry = &set->entry[set->count++];
	memset(entry, 0, sizeof(*entry));
	snprintf(entry->name, sizeof(entry->name), "%s", name);
	return entry;
}

static void mark_origin(qdos_native_entry* entry, qdos_store_scope scope) {
	switch (scope) {
	case QDOS_SCOPE_SYSTEM:
		entry->system = true;
		break;
	case QDOS_SCOPE_INBOX:
		entry->inbox = true;
		break;
	default:
		entry->user = true;
		break;
	}
}

/** @brief Keeps where it came from */
static void release(qdos_native_entry* entry) {
	if (entry->module != NULL && entry->module->close != NULL) {
		entry->module->close();
	}
	if (entry->handle != NULL) {
		dlclose(entry->handle);
	}

	entry->handle = NULL;
	entry->module = NULL;
	entry->error[0] = '\0';
}

static bool make_dir(const char* dir) {
	return mkdir(dir, 0700) == 0 || errno == EEXIST;
}

/**
 * @brief The file to dlopen: the module itself, or a copy of it
 *
 * Android maps nothing executable out of shared storage, where its inbox is,
 * so with QDOS_NATIVE_CACHE set each module is copied into that private
 * directory first, laid out as the stores are: `<cache>/<scope>/doom/libdoom.so`.
 * The copy is renamed into place, so a module still mapped from an earlier
 * load keeps the file it has.
 */
static bool loadable(qdos_store_scope scope, const char* key, const char* path, char* buf, size_t cap) {
	const char* cache = getenv("QDOS_NATIVE_CACHE");
	if (cache == NULL || *cache == '\0') {
		const int written = snprintf(buf, cap, "%s", path);
		return written > 0 && (size_t)written < cap;
	}

	char dir[512];
	int written = snprintf(dir, sizeof(dir), "%s/%d", cache, (int)scope);
	if (written <= 0 || (size_t)written >= sizeof(dir) || !make_dir(cache) || !make_dir(dir)) {
		return false;
	}

	const char* slash = strchr(key, '/');
	if (slash != NULL) {
		const size_t len = strlen(dir);
		written = snprintf(dir + len, sizeof(dir) - len, "/%.*s", (int)(slash - key), key);
		if (written <= 0 || (size_t)written >= sizeof(dir) - len || !make_dir(dir)) {
			return false;
		}
	}

	char tmp[520];
	written = snprintf(buf, cap, "%s/%d/%s", cache, (int)scope, key);
	if (written <= 0 || (size_t)written >= cap ||
			snprintf(tmp, sizeof(tmp), "%s.tmp", buf) >= (int)sizeof(tmp)) {
		return false;
	}

	FILE* in = fopen(path, "rb");
	if (in == NULL) {
		return false;
	}
	FILE* out = fopen(tmp, "wb");
	if (out == NULL) {
		fclose(in);
		return false;
	}

	char chunk[8192];
	size_t n;
	bool ok = true;
	while (ok && (n = fread(chunk, 1, sizeof(chunk), in)) > 0) {
		ok = fwrite(chunk, 1, n, out) == n;
	}
	ok = ok && !ferror(in);
	fclose(in);
	ok = (fclose(out) == 0) && ok;

	if (!ok || rename(tmp, buf) != 0) {
		unlink(tmp);
		return false;
	}
	return true;
}

typedef struct {
	qdos_natives* set;
	qdos_hal* hal;
	qdos_store_scope scope;
	const char* app; ///< The folder being walked, or NULL for the card itself
	int loaded;
} load_walk;

static bool load_one(const char* file, void* userdata) {
	load_walk* walk = (load_walk*)userdata;

	// A folder is an app; its modules are inside it, and belong to it
	char folder[QDOS_PROGRAM_NAME_MAX];
	if (walk->app == NULL && qdos_app_name(file, folder, sizeof(folder))) {
		load_walk inside = *walk;
		inside.app = folder;
		walk->hal->store_list(walk->hal, walk->scope, folder, load_one, &inside);
		walk->loaded += inside.loaded;
		return true;
	}

	char name[QDOS_PROGRAM_NAME_MAX];
	if (!qdos_module_name(file, name, sizeof(name))) {
		return true;
	}

	char key[QDOS_PROGRAM_NAME_MAX * 2];
	if (walk->app == NULL) {
		snprintf(key, sizeof(key), "%s", file);
	} else if (!qdos_app_key(walk->app, file, key, sizeof(key))) {
		return true;
	}

	char path[512];
	if (!walk->hal->store_path(walk->hal, walk->scope, key, path, sizeof(path))) {
		return true;
	}

	qdos_native_entry* entry = slot_for(walk->set, name);
	if (entry == NULL) {
		return false; // no room, and nothing to be gained by reading the rest
	}

	release(entry);
	mark_origin(entry, walk->scope);
	snprintf(entry->app, sizeof(entry->app), "%s", walk->app != NULL ? walk->app : "");

	if (listed(walk->set->blocked, name)) {
		snprintf(entry->error, sizeof(entry->error), "FAULTED, NOT LOADED");
		return true;
	}

	// Down before anything of it runs, up once it is in
	write_text(walk->hal, NATIVE_LOADING_KEY, name);

	// RTLD_NOW: an unresolved symbol is a refusal now, not mid-calculation
	char open_path[512];
	void* handle = loadable(walk->scope, key, path, open_path, sizeof(open_path))
			? dlopen(open_path, RTLD_NOW | RTLD_LOCAL)
			: NULL;
	if (handle == NULL) {
		snprintf(entry->error, sizeof(entry->error), "WILL NOT LOAD");
		write_text(walk->hal, NATIVE_LOADING_KEY, "");
		return true;
	}

	const qdos_native_module* module = (const qdos_native_module*)dlsym(handle, QDOS_NATIVE_SYMBOL);
	if (!accept(module, entry->error, sizeof(entry->error))) {
		dlclose(handle);
		write_text(walk->hal, NATIVE_LOADING_KEY, "");
		return true;
	}

	if (module->open != NULL && module->open(&API) != 0) {
		snprintf(entry->error, sizeof(entry->error), "REFUSED ITSELF");
		dlclose(handle);
		write_text(walk->hal, NATIVE_LOADING_KEY, "");
		return true;
	}

	write_text(walk->hal, NATIVE_LOADING_KEY, "");

	entry->handle = handle;
	entry->module = module;
	walk->loaded++;
	return true;
}

int qdos_natives_load(qdos_natives* set, qdos_hal* hal, qdos_store_scope scope) {
	if (set == NULL || hal == NULL) {
		return 0;
	}

	if (hal->store_path == NULL || hal->store_list == NULL) {
		return 0;
	}

	load_walk walk = {.set = set, .hal = hal, .scope = scope, .app = NULL, .loaded = 0};
	hal->store_list(hal, scope, NULL, load_one, &walk);
	return walk.loaded;
}

/** @brief Running the module, as the interpreter reaches it */
static int call_main(qd_context* ctx, void* userdata) {
	const qdos_native_module* module = (const qdos_native_module*)userdata;

	if (!g_called) {
		g_called = true;
		if (g_on_call != NULL) {
			g_on_call(g_on_call_user);
		}
	}

	const char* was = g_app;
	if (g_set != NULL) {
		for (size_t i = 0; i < g_set->count; i++) {
			if (g_set->entry[i].module == module) {
				g_app = g_set->entry[i].app;
			}
		}
	}

	const int result = module->main((qdos_native_ctx*)ctx, &API);
	g_app = was;

	return broken_or(ctx, result);
}

int qdos_natives_register(const qdos_natives* set, qd_interp* interp) {
	if (set == NULL || interp == NULL) {
		return 0;
	}

	g_set = set;
	int registered = 0;
	for (size_t i = 0; i < set->count; i++) {
		const qdos_native_entry* entry = &set->entry[i];
		if (entry->module == NULL) {
			continue;
		}

		// A program takes the bare name: `doom`, not `doom::`
		if (entry->module->main != NULL &&
				qd_interp_register(interp, entry->name, "( -- )", call_main, (void*)entry->module)) {
			registered++;
		}

		for (size_t w = 0; w < entry->module->word_count; w++) {
			const qdos_native_word* word = &entry->module->words[w];

			char scoped[QDOS_PROGRAM_NAME_MAX * 2];
			snprintf(scoped, sizeof(scoped), "%s::%s", entry->name, word->name);

			if (qd_interp_register(interp, scoped, word->signature, call_word, (void*)word)) {
				registered++;
			}
		}
	}
	return registered;
}

void qdos_natives_unload(qdos_natives* set) {
	if (set == NULL) {
		return;
	}

	for (size_t i = 0; i < set->count; i++) {
		release(&set->entry[i]);
	}

	set->count = 0;
}

const qdos_native_entry* qdos_natives_find(const qdos_natives* set, const char* name) {
	if (set == NULL || name == NULL) {
		return NULL;
	}

	for (size_t i = 0; i < set->count; i++) {
		if (strcmp(set->entry[i].name, name) == 0) {
			return &set->entry[i];
		}
	}
	return NULL;
}
