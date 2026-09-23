/**
 * @file sim_sdl3.c
 * @brief SDL3 simulator backend
 */

#include "sim_sdl3.h"

#include <SDL3/SDL.h>

#include "../wallclock.h"
#include "keypad_ui.h"

#include <dirent.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

/**
 * @brief The panel is 1bpp and the kernel cuts at 128 (drm_fb_gray8_to_mono_line)
 *
 * Doing the same here means the simulator shows what the hardware will, and a
 * stray mid-grey surfaces now rather than once it is on glass.
 */
#define PANEL_THRESHOLD 128

/* Reflective silver, not white: the panel has no backlight. */
static const uint8_t PANEL_INK[3] = {0x1A, 0x1C, 0x1A};
static const uint8_t PANEL_PAPER[3] = {0xC9, 0xCE, 0xC6};

/* --- On-screen keypad ----------------------------------------------------- */

/* The case adds a border on every side; see keypad_ui.h for the geometry */
#define WINDOW_W QDOS_WINDOW_W
#define WINDOW_H QDOS_WINDOW_H

/**
 * @brief Integer scale from panel pixels to window pixels
 *
 * 1 is closest to the hardware: the panel is 173 DPI against a monitor's ~110,
 * so even unscaled the window is about 1.6x life size. Whole numbers only --
 * a fraction would render some of the font's 2px stems 3px wide.
 *
 * Raise it with QDOS_SIM_SCALE to inspect individual pixels.
 */
#define SIM_SCALE_DEFAULT 1
#define SIM_SCALE_MAX 8

/** Directory holding simulated persistent storage. */
#define SIM_STORE_DIR "qdos-store"
/* The shipped programs as they sit in the source tree; the device installs
 * them to /usr/share/qdos/programs. */
#define SIM_SYSTEM_DIR "programs/system"
/* Stands in for the card a PC drops .qd files onto. There is no gadget to hand
 * it over with, so sharing it here means the shell stops reading it and says
 * so, and your own file manager is the PC. */
#define SIM_INBOX_DIR "qdos-inbox"

/** @brief As many app folders as the card is watched into */
#define QDOS_WATCH_SUBS 16

typedef struct {
	SDL_Window* window;
	SDL_Renderer* renderer;
	SDL_Texture* texture;
	bool running;

	/** The card is a PC's to write, and so nothing the shell may read */
	bool shared;
	int scale;
	const char* pending;   ///< Rest of a text button still to be delivered
	qdos_pad_layer layer;  ///< Which keypad face is showing
	qdos_pad_layer locked; ///< What a one-press layer hands back to

	/** Held down by the mouse, drawn sunk until the button comes back up */
	const qdos_pad_button* pressed;

	/** Android has taken the surface away, so frames are kept but not shown */
	bool background;

	/*
	 * SDL_WaitEvent takes no extra descriptor, so the card's watch gets a
	 * thread that pushes an event to wake the loop. A timeout on the wait
	 * would have cost the wakeups an idle machine is not supposed to have.
	 */
	int watch_fd;
	int watch_id;

	/** Closing the watched descriptor does not reliably wake a blocked read */
	int wake_fd[2];

	int sub_id[QDOS_WATCH_SUBS]; ///< One watch per app folder
	size_t sub_count;

	SDL_Thread* watcher;
	volatile bool store_dirty;

	/** Staging buffer: the HAL speaks 8-bit gray, the texture wants RGB. */
	uint8_t rgb[WINDOW_W * WINDOW_H * 3];
} sim_state;

#define WATCH_EVENTS (IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM | IN_DELETE | IN_CREATE)

static const char* dir_for(qdos_store_scope scope);
static bool is_dir(const char* dir, const char* name);

/** @brief The card and every app folder on it; inotify does not recurse */
static void sim_watch_inbox(sim_state* st) {
	if (st->watch_fd < 0) {
		return;
	}

	if (st->watch_id >= 0) {
		inotify_rm_watch(st->watch_fd, st->watch_id);
	}
	for (size_t i = 0; i < st->sub_count; i++) {
		inotify_rm_watch(st->watch_fd, st->sub_id[i]);
	}
	st->sub_count = 0;

	const char* inbox = dir_for(QDOS_SCOPE_INBOX);
	st->watch_id = inotify_add_watch(st->watch_fd, inbox, WATCH_EVENTS);

	DIR* dir = opendir(inbox);
	if (dir == NULL) {
		return;
	}

	const struct dirent* ent;
	while ((ent = readdir(dir)) != NULL && st->sub_count < QDOS_WATCH_SUBS) {
		if (ent->d_name[0] == '.' || !is_dir(inbox, ent->d_name)) {
			continue;
		}

		char path[512];
		if (snprintf(path, sizeof(path), "%s/%s", inbox, ent->d_name) >= (int)sizeof(path)) {
			continue;
		}

		const int id = inotify_add_watch(st->watch_fd, path, WATCH_EVENTS);
		if (id >= 0) {
			st->sub_id[st->sub_count++] = id;
		}
	}
	closedir(dir);
}

static int SDLCALL sim_watch_thread(void* data) {
	sim_state* st = (sim_state*)data;

	struct pollfd fds[2] = {
			{.fd = st->watch_fd, .events = POLLIN, .revents = 0},
			{.fd = st->wake_fd[0], .events = POLLIN, .revents = 0},
	};

	for (;;) {
		if (poll(fds, 2, -1) < 0) {
			break;
		}
		if (fds[1].revents != 0) {
			break; // shutdown
		}

		char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
		if (read(st->watch_fd, buf, sizeof(buf)) <= 0) {
			break;
		}

		st->store_dirty = true;

		// A folder that has just arrived is not being watched yet
		sim_watch_inbox(st);

		SDL_Event wake;
		SDL_zero(wake);
		wake.type = SDL_EVENT_USER;
		SDL_PushEvent(&wake);
	}
	return 0;
}

static bool sim_time_of_day(qdos_hal* hal, int* seconds) {
	(void)hal;
	return qdos_wallclock(seconds);
}

/* The desktop's own battery would say nothing about the calculator's, so the
 * simulator has one that never drains */
#define SIM_BATTERY 100

static int sim_battery(qdos_hal* hal) {
	(void)hal;
	return SIM_BATTERY;
}

static bool sim_store_changed(qdos_hal* hal) {
	sim_state* st = (sim_state*)hal->impl;
	const bool changed = st->store_dirty;
	st->store_dirty = false;
	return changed;
}

static const char* dir_for(qdos_store_scope scope);

/**
 * @brief Android may kill the process at any point once it is in the background
 *
 * A watch, because SDL never queues these: it hands them to watches on this
 * thread, from inside the pump, so the shell is between keys.
 */
static bool sim_watch_lifecycle(void* user, SDL_Event* event) {
	qdos_hal* hal = (qdos_hal*)user;
	sim_state* st = (sim_state*)hal->impl;

	if (event->type == SDL_EVENT_WILL_ENTER_BACKGROUND) {
		st->background = true;
		if (hal->save != NULL) {
			hal->save(hal->save_user);
		}
	} else if (event->type == SDL_EVENT_DID_ENTER_FOREGROUND) {
		// Drawn once the window says it is back, which is queued
		st->background = false;
	}
	return true;
}

static int sim_init(qdos_hal* hal) {
	sim_state* st = (sim_state*)hal->impl;

	if (!SDL_Init(SDL_INIT_VIDEO)) {
		fprintf(stderr, "qdos: SDL_Init failed: %s\n", SDL_GetError());
		return 1;
	}

	SDL_AddEventWatch(sim_watch_lifecycle, hal);

#ifdef SDL_PLATFORM_ANDROID
	// SDL 3.4's Android wait queues this sentinel on every pass, and queueing
	// wakes the wait, so it never sleeps: a whole core, at an idle prompt
	SDL_SetEventEnabled(SDL_EVENT_POLL_SENTINEL, false);
#endif

	st->scale = SIM_SCALE_DEFAULT;
	const char* scale_env = getenv("QDOS_SIM_SCALE");
	if (scale_env != NULL) {
		const int wanted = atoi(scale_env);
		if (wanted >= 1 && wanted <= SIM_SCALE_MAX) {
			st->scale = wanted;
		}
	}

	st->window = SDL_CreateWindow("QDOS", WINDOW_W * st->scale, WINDOW_H * st->scale, 0);
	if (!st->window) {
		fprintf(stderr, "qdos: SDL_CreateWindow failed: %s\n", SDL_GetError());
		return 1;
	}

	st->renderer = SDL_CreateRenderer(st->window, NULL);
	if (!st->renderer) {
		fprintf(stderr, "qdos: SDL_CreateRenderer failed: %s\n", SDL_GetError());
		return 1;
	}

	st->texture =
			SDL_CreateTexture(st->renderer, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, WINDOW_W, WINDOW_H);
	if (!st->texture) {
		fprintf(stderr, "qdos: SDL_CreateTexture failed: %s\n", SDL_GetError());
		return 1;
	}

#ifdef SDL_PLATFORM_ANDROID
	// A phone is rarely a whole multiple of the case, so it fills the width
	// and the pixel-art filter keeps the stems even at the fraction
	SDL_SetRenderLogicalPresentation(st->renderer, WINDOW_W, WINDOW_H, SDL_LOGICAL_PRESENTATION_LETTERBOX);
	SDL_SetTextureScaleMode(st->texture, SDL_SCALEMODE_PIXELART);
#else
	SDL_SetRenderLogicalPresentation(st->renderer, WINDOW_W, WINDOW_H, SDL_LOGICAL_PRESENTATION_INTEGER_SCALE);

	// Nearest-neighbour: real pixels, not smoothed.
	SDL_SetTextureScaleMode(st->texture, SDL_SCALEMODE_NEAREST);

	// On a phone this raises the system keyboard over the keypad, which has
	// letters of its own
	SDL_StartTextInput(st->window);
#endif

	mkdir(dir_for(QDOS_SCOPE_INBOX), 0755);

	st->watch_fd = inotify_init1(IN_CLOEXEC);
	if (st->watch_fd >= 0 && pipe(st->wake_fd) == 0) {
		sim_watch_inbox(st);
		st->watcher = SDL_CreateThread(sim_watch_thread, "qdos-watch", st);
	}

	st->running = true;
	return 0;
}

static void sim_shutdown(qdos_hal* hal) {
	sim_state* st = (sim_state*)hal->impl;
	if (!st) {
		return;
	}

	if (st->texture) {
		SDL_DestroyTexture(st->texture);
	}
	if (st->renderer) {
		SDL_DestroyRenderer(st->renderer);
	}
	if (st->window) {
		SDL_DestroyWindow(st->window);
	}

	// Told to stop before being waited for: closing the descriptor it is
	// blocked on leaves the read blocked and the wait below never returns
	if (st->watcher != NULL) {
		const char stop = 'x';
		ssize_t ignored = write(st->wake_fd[1], &stop, 1);
		(void)ignored;

		SDL_WaitThread(st->watcher, NULL);
		st->watcher = NULL;
	}

	if (st->wake_fd[0] >= 0) {
		close(st->wake_fd[0]);
		close(st->wake_fd[1]);
		st->wake_fd[0] = -1;
		st->wake_fd[1] = -1;
	}
	if (st->watch_fd >= 0) {
		close(st->watch_fd);
		st->watch_fd = -1;
	}

	st->texture = NULL;
	st->renderer = NULL;
	st->window = NULL;
	SDL_Quit();
}

/** @brief Redraw the keypad over the last panel image and show it */
static void push_frame(sim_state* st) {
	qdos_frame_draw(st->rgb, WINDOW_W);
	qdos_pad_draw(st->rgb, WINDOW_W, QDOS_PAD_X, QDOS_PAD_Y, st->layer, st->pressed);

	if (st->background) {
		return;
	}

	SDL_UpdateTexture(st->texture, NULL, st->rgb, WINDOW_W * 3);
	SDL_RenderClear(st->renderer);
	SDL_RenderTexture(st->renderer, st->texture, NULL, NULL);
	SDL_RenderPresent(st->renderer);
}

static void sim_present(qdos_hal* hal, const uint8_t* fb) {
	sim_state* st = (sim_state*)hal->impl;

	// One window pixel per panel pixel, inset by the case. Thresholded exactly
	// as the kernel does, so a stray mid-grey shows up here and not on glass.
	for (int y = 0; y < QDOS_SCREEN_H; y++) {
		for (int x = 0; x < QDOS_SCREEN_W; x++) {
			const bool lit = fb[(size_t)y * QDOS_SCREEN_W + x] < PANEL_THRESHOLD;
			const uint8_t* c = lit ? PANEL_INK : PANEL_PAPER;
			uint8_t* p = &st->rgb[(((size_t)(y + QDOS_PANEL_Y)) * WINDOW_W + x + QDOS_PANEL_X) * 3];
			p[0] = c[0];
			p[1] = c[1];
			p[2] = c[2];
		}
	}

	push_frame(st);
}

/** @brief Map a typed character to a logical key */
static void map_char(char ch, qdos_key_event* out) {
	out->ch = 0;

	if (ch >= '0' && ch <= '9') {
		out->key = (qdos_key)(QDOS_KEY_0 + (ch - '0'));
		return;
	}

	switch (ch) {
	case '.':
		out->key = QDOS_KEY_DOT;
		return;
	case '+':
		out->key = QDOS_KEY_ADD;
		return;
	case '-':
		out->key = QDOS_KEY_SUB;
		return;
	case '*':
		out->key = QDOS_KEY_MUL;
		return;
	case '/':
		out->key = QDOS_KEY_DIV;
		return;
	default:
		out->key = QDOS_KEY_CHAR;
		out->ch = ch;
		return;
	}
}

static bool sim_poll_key(qdos_hal* hal, qdos_key_event* out) {
	sim_state* st = (sim_state*)hal->impl;

	// A text button delivers one character per poll, like typing it
	if (st->pending != NULL && *st->pending != '\0') {
		out->key = QDOS_KEY_CHAR;
		out->ch = *st->pending++;
		return true;
	}

	SDL_Event event;
	while (SDL_PollEvent(&event)) {
		switch (event.type) {
		// Up before down, so a release is never mistaken for a press. The
		// key sinks on the way down and comes back on the way up, which is
		// the only acknowledgement the simulator can offer -- on the real
		// machine there is a key under your finger doing it for you.
		case SDL_EVENT_MOUSE_BUTTON_UP:
			if (st->pressed != NULL) {
				st->pressed = NULL;
				push_frame(st);
			}
			break;

		case SDL_EVENT_MOUSE_BUTTON_DOWN: {
			SDL_ConvertEventToRenderCoordinates(st->renderer, &event);
			const qdos_pad_button* b = qdos_pad_at((int)event.button.x, (int)event.button.y);
			if (b == NULL) {
				break;
			}

			// Shown before the key is acted on, so the press lands even
			// where the action itself changes nothing on screen
			st->pressed = b;
			push_frame(st);

			qdos_pad_layer selects;
			if (qdos_pad_modifier(b, &selects)) {
				if (st->layer == selects) {
					st->layer = QDOS_PAD_PLAIN;
					st->locked = QDOS_PAD_PLAIN;
				} else {
					st->layer = selects;
					// Letters lock, since a name is more than one press;
					// symbols do not, and hand back to what was showing
					if (selects == QDOS_PAD_ALPHA) {
						st->locked = QDOS_PAD_ALPHA;
					}
				}
				push_frame(st);
				break;
			}

			const qdos_pad_action* a = qdos_pad_action_for(b, st->layer);
			const qdos_pad_layer was = st->layer;
			st->layer = st->locked;
			if (a == NULL) {
				if (was != st->layer) {
					push_frame(st);
				}
				break;
			}
			if (was != st->layer) {
				push_frame(st);
			}

			if (a->key != QDOS_KEY_NONE) {
				out->key = a->key;
				out->ch = 0;
				return true;
			}
			st->pending = a->text;
			out->key = QDOS_KEY_CHAR;
			out->ch = *st->pending++;
			return true;
		}

		case SDL_EVENT_QUIT:
			st->running = false;
			return false;

		// The surface was lost, and nothing else would repaint an idle shell
		case SDL_EVENT_WINDOW_RESTORED:
		case SDL_EVENT_WINDOW_EXPOSED:
			push_frame(st);
			break;

		case SDL_EVENT_TEXT_INPUT:
			if (event.text.text[0]) {
				map_char(event.text.text[0], out);
				return true;
			}
			break;

		case SDL_EVENT_KEY_DOWN:
			switch (event.key.key) {
			case SDLK_RETURN:
			case SDLK_KP_ENTER:
				out->key = QDOS_KEY_ENTER;
				out->ch = 0;
				return true;
			case SDLK_BACKSPACE:
				out->key = QDOS_KEY_BACKSPACE;
				out->ch = 0;
				return true;
			case SDLK_TAB:
				out->key = QDOS_KEY_TAB;
				out->ch = 0;
				return true;
			case SDLK_UP:
				out->key = QDOS_KEY_UP;
				out->ch = 0;
				return true;
			case SDLK_DOWN:
				out->key = QDOS_KEY_DOWN;
				out->ch = 0;
				return true;
			case SDLK_LEFT:
				out->key = QDOS_KEY_LEFT;
				out->ch = 0;
				return true;
			case SDLK_RIGHT:
				out->key = QDOS_KEY_RIGHT;
				out->ch = 0;
				return true;
			case SDLK_F1:
			case SDLK_F2:
			case SDLK_F3:
			case SDLK_F4:
			case SDLK_F5:
				out->key = (qdos_key)(QDOS_KEY_SOFT1 + (event.key.key - SDLK_F1));
				out->ch = 0;
				return true;
			case SDLK_ESCAPE:
				out->key = QDOS_KEY_CLEAR;
				out->ch = 0;
				return true;
			case SDLK_F10:
				out->key = QDOS_KEY_POWER;
				out->ch = 0;
				return true;
			default:
				break;
			}
			break;

		default:
			break;
		}
	}
	return false;
}

static qdos_keypad_mod sim_modifier(qdos_hal* hal) {
	switch (((sim_state*)hal->impl)->layer) {
	case QDOS_PAD_ALPHA:
		return QDOS_MOD_ALPHA;
	case QDOS_PAD_SYMBOL:
		return QDOS_MOD_SYMBOL;
	default:
		return QDOS_MOD_NONE;
	}
}

static bool sim_running(qdos_hal* hal) {
	return ((sim_state*)hal->impl)->running;
}

static uint32_t sim_ticks_ms(qdos_hal* hal) {
	(void)hal;
	return (uint32_t)SDL_GetTicks();
}

static void sim_wait(qdos_hal* hal, int timeout_ms) {
	(void)hal;
	// A NULL event leaves it on the queue, so sim_poll_key still sees it.
	if (timeout_ms < 0) {
		SDL_WaitEvent(NULL);
	} else {
		SDL_WaitEventTimeout(NULL, timeout_ms);
	}
}

/**
 * @brief Build the on-disk path for a stored entry
 * @return true if the name is safe and the path fits
 */
static const char* env_or(const char* name, const char* fallback) {
	const char* v = getenv(name);
	return (v != NULL && *v != '\0') ? v : fallback;
}

static const char* dir_for(qdos_store_scope scope) {
	switch (scope) {
	case QDOS_SCOPE_SYSTEM:
		return env_or("QDOS_SYSTEM_STORE", SIM_SYSTEM_DIR);
	case QDOS_SCOPE_INBOX:
		return env_or("QDOS_INBOX", SIM_INBOX_DIR);
	default:
		return env_or("QDOS_STORE", SIM_STORE_DIR);
	}
}

/** @brief The device unmounts the inbox while a gadget has it; same answer */
static bool scope_is_reachable(const sim_state* st, qdos_store_scope scope) {
	return !(st->shared && scope == QDOS_SCOPE_INBOX);
}

static bool store_path(const char* dir, const char* name, char* buf, size_t cap) {
	if (!qdos_store_name_ok(name)) {
		return false;
	}

	const int written = snprintf(buf, cap, "%s/%s", dir, name);
	return written > 0 && (size_t)written < cap;
}

static bool is_dir(const char* dir, const char* name) {
	char path[512];
	if (snprintf(path, sizeof(path), "%s/%s", dir, name) >= (int)sizeof(path)) {
		return false;
	}

	struct stat sb;
	return stat(path, &sb) == 0 && S_ISDIR(sb.st_mode);
}

static qdos_store_result sim_store_read(
		qdos_hal* hal, qdos_store_scope scope, const char* name, void* buf, size_t cap, size_t* len) {
	if (!scope_is_reachable((sim_state*)hal->impl, scope)) {
		return QDOS_STORE_NOT_FOUND;
	}

	char path[512];
	if (!store_path(dir_for(scope), name, path, sizeof(path))) {
		return QDOS_STORE_IO_ERROR;
	}

	FILE* f = fopen(path, "rb");
	if (!f) {
		return QDOS_STORE_NOT_FOUND;
	}

	const size_t got = fread(buf, 1, cap, f);
	// A full buffer with bytes left is too-small, not a short read.
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

static qdos_store_result sim_store_write(qdos_hal* hal, const char* name, const void* buf, size_t len) {
	(void)hal;

	char path[512];
	if (!store_path(dir_for(QDOS_SCOPE_USER), name, path, sizeof(path))) {
		return QDOS_STORE_IO_ERROR;
	}

	mkdir(dir_for(QDOS_SCOPE_USER), 0755); // may already exist, which is fine

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
	const bool ok = (fclose(f) == 0) && (written == len);
	return ok ? QDOS_STORE_OK : QDOS_STORE_IO_ERROR;
}

static bool sim_store_path(qdos_hal* hal, qdos_store_scope scope, const char* name, char* buf, size_t cap) {
	if (!scope_is_reachable((sim_state*)hal->impl, scope)) {
		return false;
	}
	return store_path(dir_for(scope), name, buf, cap);
}

/**
 * @brief Hand the card over, or take it back
 *
 * No gadget to simulate, but the half the shell copes with is real: the inbox
 * goes quiet, and is read afresh when it comes back.
 */
static int sim_usb_export(qdos_hal* hal, bool on) {
	sim_state* st = (sim_state*)hal->impl;

	st->shared = on;
	if (on) {
		printf("qdos: card shared -- drop .qd, lib*.so or an app folder in %s/\n", dir_for(QDOS_SCOPE_INBOX));
	} else {
		printf("qdos: card taken back\n");
	}
	fflush(stdout);

	return 0;
}

static qdos_store_result sim_store_list(
		qdos_hal* hal, qdos_store_scope scope, const char* folder, qdos_store_visit visit, void* user) {
	// An empty mount point rather than an error
	if (!scope_is_reachable((sim_state*)hal->impl, scope)) {
		return QDOS_STORE_OK;
	}

	char root[512];
	if (folder == NULL || *folder == '\0') {
		snprintf(root, sizeof(root), "%s", dir_for(scope));
	} else if (!store_path(dir_for(scope), folder, root, sizeof(root))) {
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
		const int written = snprintf(name, sizeof(name), "%s%s", ent->d_name, is_dir(root, ent->d_name) ? "/" : "");
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

static sim_state g_sim;

void qdos_sim_hal(qdos_hal* hal) {
	memset(&g_sim, 0, sizeof(g_sim));
	g_sim.watch_fd = -1;
	g_sim.watch_id = -1;
	g_sim.wake_fd[0] = -1;
	g_sim.wake_fd[1] = -1;

	hal->init = sim_init;
	hal->shutdown = sim_shutdown;
	hal->present = sim_present;
	hal->poll_key = sim_poll_key;
	hal->modifier = sim_modifier;
	hal->running = sim_running;
	hal->ticks_ms = sim_ticks_ms;
	hal->wait = sim_wait;
	hal->store_read = sim_store_read;
	hal->store_write = sim_store_write;
	hal->store_list = sim_store_list;
	hal->store_path = sim_store_path;
	hal->usb_export = sim_usb_export;
	hal->store_changed = sim_store_changed;
	hal->time_of_day = sim_time_of_day;
	hal->battery = sim_battery;
	hal->impl = &g_sim;
}
