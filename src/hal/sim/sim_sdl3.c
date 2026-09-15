/**
 * @file sim_sdl3.c
 * @brief SDL3 simulator backend
 */

#include "sim_sdl3.h"

#include <SDL3/SDL.h>

#include "keypad_ui.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

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

#define WINDOW_W QDOS_SCREEN_W
#define WINDOW_H (QDOS_SCREEN_H + QDOS_PAD_H)

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

typedef struct {
	SDL_Window* window;
	SDL_Renderer* renderer;
	SDL_Texture* texture;
	bool running;
	int scale;
	const char* pending; ///< Rest of a text button still to be delivered
	bool shifted;

	/** Staging buffer: the HAL speaks 8-bit gray, the texture wants RGB. */
	uint8_t rgb[WINDOW_W * WINDOW_H * 3];
} sim_state;

static int sim_init(qdos_hal* hal) {
	sim_state* st = (sim_state*)hal->impl;

	if (!SDL_Init(SDL_INIT_VIDEO)) {
		fprintf(stderr, "qdos: SDL_Init failed: %s\n", SDL_GetError());
		return 1;
	}

	st->scale = SIM_SCALE_DEFAULT;
	const char* scale_env = getenv("QDOS_SIM_SCALE");
	if (scale_env != NULL) {
		const int wanted = atoi(scale_env);
		if (wanted >= 1 && wanted <= SIM_SCALE_MAX)
			st->scale = wanted;
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

	st->texture = SDL_CreateTexture(st->renderer, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING,
			WINDOW_W, WINDOW_H);
	if (!st->texture) {
		fprintf(stderr, "qdos: SDL_CreateTexture failed: %s\n", SDL_GetError());
		return 1;
	}

	// Nearest-neighbour: real pixels, not smoothed.
	SDL_SetTextureScaleMode(st->texture, SDL_SCALEMODE_NEAREST);

	SDL_StartTextInput(st->window);

	st->running = true;
	return 0;
}

static void sim_shutdown(qdos_hal* hal) {
	sim_state* st = (sim_state*)hal->impl;
	if (!st)
		return;

	if (st->texture)
		SDL_DestroyTexture(st->texture);
	if (st->renderer)
		SDL_DestroyRenderer(st->renderer);
	if (st->window)
		SDL_DestroyWindow(st->window);

	st->texture = NULL;
	st->renderer = NULL;
	st->window = NULL;
	SDL_Quit();
}

/** @brief Redraw the keypad over the last panel image and show it */
static void push_frame(sim_state* st) {
	qdos_pad_draw(st->rgb, WINDOW_W, QDOS_SCREEN_H, st->shifted);

	SDL_UpdateTexture(st->texture, NULL, st->rgb, WINDOW_W * 3);
	SDL_RenderClear(st->renderer);
	SDL_RenderTexture(st->renderer, st->texture, NULL, NULL);
	SDL_RenderPresent(st->renderer);
}

static void sim_present(qdos_hal* hal, const uint8_t* fb) {
	sim_state* st = (sim_state*)hal->impl;

	for (size_t i = 0; i < QDOS_SCREEN_W * QDOS_SCREEN_H; i++) {
		const bool lit = fb[i] < PANEL_THRESHOLD;
		const uint8_t* c = lit ? PANEL_INK : PANEL_PAPER;
		st->rgb[i * 3 + 0] = c[0];
		st->rgb[i * 3 + 1] = c[1];
		st->rgb[i * 3 + 2] = c[2];
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
		case '.': out->key = QDOS_KEY_DOT; return;
		case '+': out->key = QDOS_KEY_ADD; return;
		case '-': out->key = QDOS_KEY_SUB; return;
		case '*': out->key = QDOS_KEY_MUL; return;
		case '/': out->key = QDOS_KEY_DIV; return;
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
			case SDL_EVENT_MOUSE_BUTTON_DOWN: {
				const qdos_pad_button* b = qdos_pad_at(
						(int)event.button.x / st->scale, (int)event.button.y / st->scale);
				if (b == NULL)
					break;

				if (qdos_pad_is_shift(b)) {
					st->shifted = !st->shifted;
					push_frame(st);
					break;
				}

				const qdos_pad_action* a = qdos_pad_action_for(b, st->shifted);
				const bool was_shifted = st->shifted;
				st->shifted = false;
				if (a == NULL) {
					if (was_shifted)
						push_frame(st);
					break;
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

static bool sim_running(qdos_hal* hal) {
	return ((sim_state*)hal->impl)->running;
}

static void sim_idle(qdos_hal* hal) {
	(void)hal;
	SDL_Delay(16);
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
	return (scope == QDOS_SCOPE_SYSTEM) ? env_or("QDOS_SYSTEM_STORE", SIM_SYSTEM_DIR)
									    : env_or("QDOS_STORE", SIM_STORE_DIR);
}

static bool store_path(const char* dir, const char* name, char* buf, size_t cap) {
	// Reject anything that could escape the store directory
	if (!name || !*name || strchr(name, '/') || strchr(name, '\\') || strcmp(name, "..") == 0)
		return false;

	const int written = snprintf(buf, cap, "%s/%s", dir, name);
	return written > 0 && (size_t)written < cap;
}

static qdos_store_result sim_store_read(
		qdos_hal* hal, qdos_store_scope scope, const char* name, void* buf, size_t cap, size_t* len) {
	(void)hal;

	char path[512];
	if (!store_path(dir_for(scope), name, path, sizeof(path)))
		return QDOS_STORE_IO_ERROR;

	FILE* f = fopen(path, "rb");
	if (!f)
		return QDOS_STORE_NOT_FOUND;

	const size_t got = fread(buf, 1, cap, f);
	// A full buffer with bytes left is too-small, not a short read.
	const bool overflowed = (got == cap) && (fgetc(f) != EOF);
	fclose(f);

	if (overflowed)
		return QDOS_STORE_TOO_BIG;

	if (len)
		*len = got;
	return QDOS_STORE_OK;
}

static qdos_store_result sim_store_write(qdos_hal* hal, const char* name, const void* buf, size_t len) {
	(void)hal;

	char path[512];
	if (!store_path(dir_for(QDOS_SCOPE_USER), name, path, sizeof(path)))
		return QDOS_STORE_IO_ERROR;

	mkdir(dir_for(QDOS_SCOPE_USER), 0755); // may already exist, which is fine

	FILE* f = fopen(path, "wb");
	if (!f)
		return QDOS_STORE_IO_ERROR;

	const size_t written = fwrite(buf, 1, len, f);
	const bool ok = (fclose(f) == 0) && (written == len);
	return ok ? QDOS_STORE_OK : QDOS_STORE_IO_ERROR;
}

static qdos_store_result sim_store_list(
		qdos_hal* hal, qdos_store_scope scope, qdos_store_visit visit, void* user) {
	(void)hal;

	DIR* dir = opendir(dir_for(scope));
	if (!dir)
		return QDOS_STORE_NOT_FOUND;

	const struct dirent* ent;
	while ((ent = readdir(dir)) != NULL) {
		if (ent->d_name[0] == '.')
			continue;
		if (!visit(ent->d_name, user))
			break;
	}

	closedir(dir);
	return QDOS_STORE_OK;
}

static sim_state g_sim;

void qdos_sim_hal(qdos_hal* hal) {
	memset(&g_sim, 0, sizeof(g_sim));

	hal->init = sim_init;
	hal->shutdown = sim_shutdown;
	hal->present = sim_present;
	hal->poll_key = sim_poll_key;
	hal->running = sim_running;
	hal->idle = sim_idle;
	hal->store_read = sim_store_read;
	hal->store_write = sim_store_write;
	hal->store_list = sim_store_list;
	hal->impl = &g_sim;
}
