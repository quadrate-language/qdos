/**
 * @file main.c
 * @brief QDOS entry point
 */

#include <qdos/hal.h>
#include <qdos/shell.h>

#include "ui/splash.h"

#include <stdio.h>
#include <string.h>

#ifdef QDOS_WITH_SIM
#include "hal/sim/sim_sdl3.h"
#ifdef __ANDROID__
// SDLActivity calls SDL_main, which this renames main to
#include <SDL3/SDL_main.h>
#endif
#endif

#ifdef QDOS_WITH_DEVICE
#include "hal/device/device_linux.h"
#endif

#ifdef __EMSCRIPTEN__
/** @brief The panel goes blank and stays so until the next press */
static void power_off(qdos_hal* hal) {
	static uint8_t blank[QDOS_SCREEN_W * QDOS_SCREEN_H];
	memset(blank, 0xFF, sizeof(blank));
	hal->present(hal, blank);

	qdos_key_event ev;
	for (;;) {
		hal->wait(hal, -1);
		if (hal->poll_key(hal, &ev)) {
			break;
		}
	}
	if (hal->modifier_reset != NULL) {
		hal->modifier_reset(hal);
	}
}
#endif

/** @brief Until the machine is switched off; false if it never started */
static bool run_shell(qdos_hal* hal) {
	qdos_shell* shell = qdos_shell_create(hal);
	if (!shell) {
		fprintf(stderr, "qdos: out of memory\n");
		return false;
	}

	qdos_shell_run(shell);
	qdos_shell_destroy(shell);
	return true;
}

static void usage(const char* argv0) {
	fprintf(stderr,
			"usage: %s [--sim | --device] [--splash]\n"
			"\n"
			"  --sim     SDL3 simulator (default where built in)\n"
			"  --device  Linux framebuffer and evdev\n"
			"  --splash  Paint the startup screen and exit\n",
			argv0);
}

int main(int argc, char** argv) {
	bool want_device = false;
	bool splash_only = false;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--device") == 0) {
			want_device = true;
		} else if (strcmp(argv[i], "--sim") == 0) {
			want_device = false;
		} else if (strcmp(argv[i], "--splash") == 0) {
			splash_only = true;
		} else {
			usage(argv[0]);
			return 2;
		}
	}

	qdos_hal hal;
	memset(&hal, 0, sizeof(hal));

	if (want_device) {
#ifdef QDOS_WITH_DEVICE
		qdos_device_hal(&hal);
#else
		fprintf(stderr, "qdos: built without the device backend\n");
		return 1;
#endif
	} else {
#ifdef QDOS_WITH_SIM
		qdos_sim_hal(&hal);
#else
		fprintf(stderr, "qdos: built without the simulator backend\n");
		return 1;
#endif
	}

	if (hal.init(&hal) != 0) {
		hal.shutdown(&hal);
		return 1;
	}

	// No shutdown(): handing the console back repaints over the splash.
	if (splash_only) {
		qdos_splash_draw(&hal);
		return 0;
	}

#ifdef __EMSCRIPTEN__
	// A page has nothing to exit to. Off is a blank panel until a key, which
	// starts the machine afresh, as waking the ESP32 from deep sleep does.
	qdos_splash_draw(&hal);
	while (run_shell(&hal)) {
		power_off(&hal);
		qdos_splash_draw(&hal);
	}
	const bool ok = false;
#else
	const bool ok = run_shell(&hal);
#endif

	hal.shutdown(&hal);
	return ok ? 0 : 1;
}
