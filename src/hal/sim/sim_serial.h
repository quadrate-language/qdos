/**
 * @file sim_serial.h
 * @brief The simulator's serial console, on a pseudo-terminal
 *
 * The calculator's USB port is a serial console that qdosctl types at and
 * uploads through. The simulator offers the same on a pseudo-terminal, served
 * by the same code (src/hal/serial_console.c), so qdosctl drives either alike.
 * Desktop only: a browser has no terminals, and a phone no qdosctl.
 */

#ifndef QDOS_SIM_SERIAL_H
#define QDOS_SIM_SERIAL_H

#include <qdos/keys.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Where qdosctl looks for the simulator, under $XDG_RUNTIME_DIR (or /tmp) */
#define QDOS_SIM_SERIAL_LINK "qdos-sim-tty"

typedef struct sim_serial sim_serial;

typedef struct {
	const char* inbox;								   ///< Where uploads land
	void (*key)(void* user, const qdos_key_event* ev); ///< Called on the console's thread
	void (*touched)(void* user);					   ///< Likewise
	void* user;
} sim_serial_config;

/** @brief Open the port and serve it on a thread of its own; NULL if it could not be opened */
sim_serial* sim_serial_start(const sim_serial_config* config);

/** @brief The terminal qdosctl opens, such as /dev/pts/3 */
const char* sim_serial_path(const sim_serial* s);

/** @brief Stop serving and close the port; NULL is fine */
void sim_serial_stop(sim_serial* s);

#ifdef __cplusplus
}
#endif

#endif // QDOS_SIM_SERIAL_H
