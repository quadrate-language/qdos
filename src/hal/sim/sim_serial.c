/**
 * @file sim_serial.c
 * @brief The simulator's serial console, on a pseudo-terminal
 *
 * The master end is held here and the slave is what qdosctl opens, found
 * through a link at a fixed place. Nothing here knows what the bytes mean:
 * that is src/hal/serial_console.c, as on the device.
 *
 * A pty master reports a hang-up for as long as no one has the slave open, so
 * waiting on it then would spin. Between sessions the thread waits instead for
 * the slave to be opened, which inotify reports, and an idle simulator still
 * costs no wakeups.
 */

#define _GNU_SOURCE

#include "sim_serial.h"

#include "../serial_console.h"

#include <SDL3/SDL.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <termios.h>
#include <unistd.h>

struct sim_serial {
	int master;
	int opens;	 ///< inotify, for the slave being opened
	int stop[2]; ///< Written to end the thread
	char slave[64];
	char link[PATH_MAX];
	qdos_serial console;
	sim_serial_config config;
	SDL_Thread* thread;
};

/** @brief Readable (1), timed out (0) or told to stop (-1), within @p deadline (0 for none) */
static int wait_readable(sim_serial* s, Uint64 deadline) {
	for (;;) {
		int left = -1;
		if (deadline != 0) {
			const Uint64 now = SDL_GetTicks();
			if (now >= deadline) {
				return 0;
			}
			left = (int)(deadline - now);
		}

		struct pollfd fds[2] = {
				{.fd = s->master, .events = POLLIN, .revents = 0},
				{.fd = s->stop[0], .events = POLLIN, .revents = 0},
		};
		const int r = poll(fds, 2, left);
		if (r < 0) {
			if (errno == EINTR) {
				continue;
			}
			return -1;
		}
		if (r == 0) {
			return 0;
		}
		if (fds[1].revents != 0) {
			return -1;
		}
		if (fds[0].revents & POLLIN) {
			return 1;
		}

		// No one has the slave open: wait until someone does. An open
		// reported earlier only costs one more look at the master.
		struct pollfd idle[2] = {
				{.fd = s->opens, .events = POLLIN, .revents = 0},
				{.fd = s->stop[0], .events = POLLIN, .revents = 0},
		};
		const int q = poll(idle, 2, left);
		if (q < 0 && errno != EINTR) {
			return -1;
		}
		if (q == 0) {
			return 0;
		}
		if (idle[1].revents != 0) {
			return -1;
		}
		char events[1024] __attribute__((aligned(__alignof__(struct inotify_event))));
		while (read(s->opens, events, sizeof(events)) > 0) {
		}
	}
}

static int port_read(void* user, uint8_t* buf, size_t cap, int timeout_ms) {
	sim_serial* s = (sim_serial*)user;
	const Uint64 deadline = timeout_ms < 0 ? 0 : SDL_GetTicks() + (Uint64)timeout_ms;

	for (;;) {
		const int w = wait_readable(s, deadline);
		if (w <= 0) {
			return w;
		}
		const ssize_t n = read(s->master, buf, cap);
		if (n > 0) {
			return (int)n;
		}
		// EIO is the last client having gone between the poll and the read
		if (n < 0 && errno != EAGAIN && errno != EINTR && errno != EIO) {
			return -1;
		}
	}
}

/** With nothing reading, the pty's buffer fills and the rest is dropped, as a port with no host drops it */
static void port_write(void* user, const char* text, size_t len) {
	sim_serial* s = (sim_serial*)user;
	while (len > 0) {
		const ssize_t n = write(s->master, text, len);
		if (n <= 0) {
			return;
		}
		text += n;
		len -= (size_t)n;
	}
}

static void port_key(void* user, const qdos_key_event* ev) {
	sim_serial* s = (sim_serial*)user;
	s->config.key(s->config.user, ev);
}

static void port_touched(void* user) {
	sim_serial* s = (sim_serial*)user;
	s->config.touched(s->config.user);
}

static int SDLCALL serial_thread(void* data) {
	sim_serial* s = (sim_serial*)data;
	while (qdos_serial_step(&s->console)) {
	}
	return 0;
}

/** @brief The link qdosctl finds the simulator by, replacing one left by another */
static void make_link(sim_serial* s) {
	const char* dir = getenv("XDG_RUNTIME_DIR");
	if (dir == NULL || *dir == '\0') {
		dir = "/tmp";
	}
	if (snprintf(s->link, sizeof(s->link), "%s/%s", dir, QDOS_SIM_SERIAL_LINK) >= (int)sizeof(s->link)) {
		s->link[0] = '\0';
		return;
	}
	unlink(s->link);
	if (symlink(s->slave, s->link) != 0) {
		s->link[0] = '\0';
	}
}

/** @brief Only our own: a second simulator may have taken the link since */
static void remove_link(const sim_serial* s) {
	if (s->link[0] == '\0') {
		return;
	}
	char target[sizeof(s->slave)];
	const ssize_t n = readlink(s->link, target, sizeof(target) - 1);
	if (n > 0) {
		target[n] = '\0';
		if (strcmp(target, s->slave) == 0) {
			unlink(s->link);
		}
	}
}

static void close_fds(sim_serial* s) {
	if (s->master >= 0) {
		close(s->master);
	}
	if (s->opens >= 0) {
		close(s->opens);
	}
	if (s->stop[0] >= 0) {
		close(s->stop[0]);
		close(s->stop[1]);
	}
}

sim_serial* sim_serial_start(const sim_serial_config* config) {
	sim_serial* s = (sim_serial*)calloc(1, sizeof(*s));
	if (s == NULL) {
		return NULL;
	}
	s->master = -1;
	s->opens = -1;
	s->stop[0] = -1;
	s->stop[1] = -1;
	s->config = *config;

	s->master = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
	const char* name = NULL;
	if (s->master < 0 || grantpt(s->master) != 0 || unlockpt(s->master) != 0 || (name = ptsname(s->master)) == NULL ||
			snprintf(s->slave, sizeof(s->slave), "%s", name) >= (int)sizeof(s->slave)) {
		close_fds(s);
		free(s);
		return NULL;
	}

	// Raw from the start, as a serial port is: no echo of what qdosctl sends
	// back to it, and nothing translated, before it has set the line itself
	struct termios raw;
	if (tcgetattr(s->master, &raw) == 0) {
		cfmakeraw(&raw);
		tcsetattr(s->master, TCSANOW, &raw);
	}

	s->opens = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (s->opens < 0 || inotify_add_watch(s->opens, s->slave, IN_OPEN) < 0 || pipe2(s->stop, O_CLOEXEC) != 0) {
		close_fds(s);
		free(s);
		return NULL;
	}

	s->console = (qdos_serial){
			.read = port_read,
			.write = port_write,
			.key = port_key,
			.touched = port_touched,
			.inbox = config->inbox,
			.user = s,
	};

	s->thread = SDL_CreateThread(serial_thread, "qdos-serial", s);
	if (s->thread == NULL) {
		close_fds(s);
		free(s);
		return NULL;
	}

	make_link(s);
	return s;
}

const char* sim_serial_path(const sim_serial* s) {
	return s->slave;
}

void sim_serial_stop(sim_serial* s) {
	if (s == NULL) {
		return;
	}

	const char stop = 'x';
	ssize_t ignored = write(s->stop[1], &stop, 1);
	(void)ignored;
	SDL_WaitThread(s->thread, NULL);

	remove_link(s);
	close_fds(s);
	free(s);
}
