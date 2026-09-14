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
#endif

#ifdef QDOS_WITH_DEVICE
#include "hal/device/device_linux.h"
#endif

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

	qdos_shell* shell = qdos_shell_create(&hal);
	if (!shell) {
		fprintf(stderr, "qdos: out of memory\n");
		hal.shutdown(&hal);
		return 1;
	}

	qdos_shell_run(shell);

	qdos_shell_destroy(shell);
	hal.shutdown(&hal);
	return 0;
}
