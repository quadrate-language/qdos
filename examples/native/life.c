/**
 * @file life.c
 * @brief A native program: Conway's Life, at one cell per pixel
 *
 * A module with a main() rather than words, so it is listed and run as `life`
 * and takes the screen while it does. The panel is one bit per pixel and so is
 * Life, which makes 400x240 a board rather than a compromise.
 *
 *     ./cross/build-module.sh examples/native/life.c
 *     cc -shared -fPIC -I include examples/native/life.c -o qdos-store/liblife.so
 *
 * ESC leaves, Enter reseeds, any other key pauses and unpauses.
 */

#include <qdos/native.h>

#include <stdlib.h>
#include <string.h>

/* The panel's own levels: under 128 is ink, at or over it is paper */
#define ALIVE 0x00
#define DEAD 0xFF

/** Frames a second to aim at; the panel cannot do much better than this */
#define TARGET_MS 50

static uint32_t g_seed = 2463534242u;

static uint32_t next_random(void) {
	g_seed ^= g_seed << 13;
	g_seed ^= g_seed >> 17;
	g_seed ^= g_seed << 5;
	return g_seed;
}

static void seed_board(uint8_t* board, int w, int h) {
	for (int i = 0; i < w * h; i++) {
		board[i] = (next_random() & 3u) == 0 ? ALIVE : DEAD;
	}
}

/** @brief How many of the eight neighbours are alive, wrapping at the edges */
static int neighbours(const uint8_t* board, int w, int h, int x, int y) {
	int alive = 0;

	for (int dy = -1; dy <= 1; dy++) {
		const int ny = (y + dy + h) % h;
		for (int dx = -1; dx <= 1; dx++) {
			if (dx == 0 && dy == 0) {
				continue;
			}
			const int nx = (x + dx + w) % w;
			if (board[(size_t)ny * w + nx] == ALIVE) {
				alive++;
			}
		}
	}
	return alive;
}

static void step(const uint8_t* from, uint8_t* to, int w, int h) {
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			const size_t at = (size_t)y * w + x;
			const int around = neighbours(from, w, h, x, y);

			if (from[at] == ALIVE) {
				to[at] = (around == 2 || around == 3) ? ALIVE : DEAD;
			} else {
				to[at] = (around == 3) ? ALIVE : DEAD;
			}
		}
	}
}

static int life_main(qdos_native_ctx* ctx, const qdos_native_api* api) {
	int w = 0;
	int h = 0;
	uint8_t* canvas = api->canvas(ctx, &w, &h);
	if (canvas == NULL || w <= 0 || h <= 0) {
		api->fail(ctx, "life: NO CANVAS");
		return 1;
	}

	// The canvas is one of the two boards, so a generation is drawn by being
	// computed rather than copied afterwards
	uint8_t* other = malloc((size_t)w * h);
	if (other == NULL) {
		api->fail(ctx, "life: OUT OF MEMORY");
		return 1;
	}

	g_seed ^= api->ticks(ctx) * 2654435761u;
	seed_board(canvas, w, h);

	bool running = true;
	bool paused = false;

	while (running && api->running(ctx)) {
		const uint32_t started = api->ticks(ctx);

		if (!paused) {
			step(canvas, other, w, h);
			memcpy(canvas, other, (size_t)w * h);
		}
		api->present(ctx);

		qdos_key key = QDOS_KEY_NONE;
		char ch = 0;
		while (api->key(ctx, &key, &ch)) {
			if (key == QDOS_KEY_CLEAR || key == QDOS_KEY_POWER) {
				running = false;
			} else if (key == QDOS_KEY_ENTER) {
				seed_board(canvas, w, h);
			} else {
				paused = !paused;
			}
		}

		// Give the machine back for whatever is left of the frame. A loop that
		// spins costs battery on a display that holds its own image.
		const uint32_t spent = api->ticks(ctx) - started;
		api->wait(ctx, spent < TARGET_MS ? (int)(TARGET_MS - spent) : 0);
	}

	free(other);
	return 0;
}

const qdos_native_module qdos_module = {
		QDOS_NATIVE_HEADER,
		NULL, // no words: there is nothing here to call from Quadrate
		0,
		NULL, // nothing to set up
		NULL, // nor to put away
		life_main,
};
