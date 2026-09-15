/**
 * @file sim_sdl3.c
 * @brief SDL3 simulator backend
 */

#include "sim_sdl3.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

/**
 * @brief Integer scale from panel pixels to window pixels
 *
 * Kept whole: the font is drawn pixel by pixel, and a fractional scale would
 * render some of its 2px stems 3px wide and others 2px.
 */
#define SIM_SCALE 2

/** Directory holding simulated persistent storage. */
#define SIM_STORE_DIR "qdos-store"

typedef struct {
	SDL_Window* window;
	SDL_Renderer* renderer;
	SDL_Texture* texture;
	bool running;

	/** Staging buffer: the HAL speaks 8-bit gray, the texture wants RGB. */
	uint8_t rgb[QDOS_SCREEN_W * QDOS_SCREEN_H * 3];
} sim_state;

static int sim_init(qdos_hal* hal) {
	sim_state* st = (sim_state*)hal->impl;

	if (!SDL_Init(SDL_INIT_VIDEO)) {
		fprintf(stderr, "qdos: SDL_Init failed: %s\n", SDL_GetError());
		return 1;
	}

	st->window = SDL_CreateWindow("QDOS", QDOS_SCREEN_W * SIM_SCALE, QDOS_SCREEN_H * SIM_SCALE, 0);
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
			QDOS_SCREEN_W, QDOS_SCREEN_H);
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

static void sim_present(qdos_hal* hal, const uint8_t* fb) {
	sim_state* st = (sim_state*)hal->impl;

	for (size_t i = 0; i < QDOS_SCREEN_W * QDOS_SCREEN_H; i++) {
		st->rgb[i * 3 + 0] = fb[i];
		st->rgb[i * 3 + 1] = fb[i];
		st->rgb[i * 3 + 2] = fb[i];
	}

	SDL_UpdateTexture(st->texture, NULL, st->rgb, QDOS_SCREEN_W * 3);
	SDL_RenderClear(st->renderer);
	SDL_RenderTexture(st->renderer, st->texture, NULL, NULL);
	SDL_RenderPresent(st->renderer);
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

	SDL_Event event;
	while (SDL_PollEvent(&event)) {
		switch (event.type) {
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
static bool store_path(const char* name, char* buf, size_t cap) {
	// Reject anything that could escape the store directory
	if (!name || !*name || strchr(name, '/') || strchr(name, '\\') || strcmp(name, "..") == 0)
		return false;

	const int written = snprintf(buf, cap, "%s/%s", SIM_STORE_DIR, name);
	return written > 0 && (size_t)written < cap;
}

static qdos_store_result sim_store_read(qdos_hal* hal, const char* name, void* buf, size_t cap, size_t* len) {
	(void)hal;

	char path[512];
	if (!store_path(name, path, sizeof(path)))
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
	if (!store_path(name, path, sizeof(path)))
		return QDOS_STORE_IO_ERROR;

	mkdir(SIM_STORE_DIR, 0755); // may already exist, which is fine

	FILE* f = fopen(path, "wb");
	if (!f)
		return QDOS_STORE_IO_ERROR;

	const size_t written = fwrite(buf, 1, len, f);
	const bool ok = (fclose(f) == 0) && (written == len);
	return ok ? QDOS_STORE_OK : QDOS_STORE_IO_ERROR;
}

static qdos_store_result sim_store_list(qdos_hal* hal, qdos_store_visit visit, void* user) {
	(void)hal;

	DIR* dir = opendir(SIM_STORE_DIR);
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
