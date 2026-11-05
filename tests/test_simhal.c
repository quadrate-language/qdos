/**
 * @file test_simhal.c
 * @brief The simulator backend's own lifecycle
 *
 * Mostly for shutdown: a watcher or a serial console that cannot be told to
 * stop leaves the process alive with its window gone, and SDL turns Ctrl-C
 * into an event. And the serial console, end to end, the way qdosctl uses it.
 */

#define _GNU_SOURCE

#include "check.h"

#include "hal/serial_console.h"
#include "hal/sim/sim_sdl3.h"
#include "hal/sim/sim_serial.h"

#include <qdos/hal.h>

#include <SDL3/SDL.h>

#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

/** Starting and stopping returns, rather than hanging on a watcher. */
static void test_shutdown_returns(void) {
	qdos_hal hal;
	memset(&hal, 0, sizeof(hal));
	qdos_sim_hal(&hal);

	CHECK(hal.init(&hal) == 0);

	// Hangs rather than failing if the watcher cannot be woken
	hal.shutdown(&hal);
	CHECK(1);
}

/** And again, because a backend that only starts once is not much of one. */
static void test_it_can_be_started_twice(void) {
	qdos_hal hal;

	for (int round = 0; round < 2; round++) {
		memset(&hal, 0, sizeof(hal));
		qdos_sim_hal(&hal);
		CHECK(hal.init(&hal) == 0);
		hal.shutdown(&hal);
	}
}

/** One "QDUP ..." line from the port, the prefix taken off; false if none came */
static bool read_reply(int fd, char* out, size_t cap) {
	char line[256];
	size_t n = 0;
	for (;;) {
		struct pollfd p = {.fd = fd, .events = POLLIN, .revents = 0};
		if (poll(&p, 1, 2000) <= 0) {
			return false;
		}
		char c;
		if (read(fd, &c, 1) != 1) {
			return false;
		}
		if (c == '\n') {
			line[n] = '\0';
			if (strncmp(line, "QDUP ", 5) == 0) {
				snprintf(out, cap, "%s", line + 5);
				return true;
			}
			n = 0;
		} else if (n + 1 < sizeof(line)) {
			line[n++] = c;
		}
	}
}

static void send_text(int fd, const char* text) {
	CHECK(write(fd, text, strlen(text)) == (ssize_t)strlen(text));
}

/** The first key the console has sent, polling the backend as the shell does */
static bool next_key(qdos_hal* hal, qdos_key_event* out) {
	for (int tries = 0; tries < 200; tries++) {
		if (hal->poll_key(hal, out)) {
			return true;
		}
		SDL_Delay(10);
	}
	return false;
}

/** qdosctl's view: the link, a HELLO, an upload, and a key typed at it. */
static void test_qdosctl_reaches_it(void) {
	const char* inbox = getenv("QDOS_INBOX");
	CHECK(inbox != NULL);

	qdos_hal hal;
	memset(&hal, 0, sizeof(hal));
	qdos_sim_hal(&hal);
	CHECK(hal.init(&hal) == 0);
	CHECK(hal.store_changed != NULL);
	CHECK(!hal.store_changed(&hal));

	// Found where qdosctl looks for it
	char link[512];
	snprintf(link, sizeof(link), "%s/%s", getenv("XDG_RUNTIME_DIR"), QDOS_SIM_SERIAL_LINK);
	const int fd = open(link, O_RDWR | O_NOCTTY);
	CHECK(fd >= 0);
	if (fd < 0) {
		hal.shutdown(&hal);
		return;
	}
	struct termios raw;
	tcgetattr(fd, &raw);
	cfmakeraw(&raw);
	tcsetattr(fd, TCSANOW, &raw);

	char reply[128];
	send_text(fd, "\x02HELLO\n");
	CHECK(read_reply(fd, reply, sizeof(reply)));
	CHECK_STR(reply, "OK");

	// An upload, with the checksum qdosctl would compute
	const char* body = "fn up( -- r:i64) { 5 }\n";
	char line[128];
	snprintf(line, sizeof(line), "\x02PUT %zu %08x up.qd\n", strlen(body),
			(unsigned)qdos_serial_crc32(0, (const uint8_t*)body, strlen(body)));
	send_text(fd, line);
	CHECK(read_reply(fd, reply, sizeof(reply)));
	CHECK_STR(reply, "READY");
	send_text(fd, body);
	CHECK(read_reply(fd, reply, sizeof(reply)));
	CHECK_STR(reply, "OK");

	char landed[512];
	snprintf(landed, sizeof(landed), "%s/up.qd", inbox);
	struct stat st;
	CHECK(stat(landed, &st) == 0 && st.st_size == (off_t)strlen(body));
	CHECK(hal.store_changed(&hal));

	// And typing at it is the keyboard
	send_text(fd, "x");
	qdos_key_event ev;
	CHECK(next_key(&hal, &ev));
	CHECK(ev.key == QDOS_KEY_CHAR && ev.ch == 'x');

	send_text(fd, "\x1b[A");
	CHECK(next_key(&hal, &ev));
	CHECK(ev.key == QDOS_KEY_UP);

	// A second session after the first has gone, as each qdosctl run is
	close(fd);
	const int again = open(link, O_RDWR | O_NOCTTY);
	CHECK(again >= 0);
	if (again >= 0) {
		send_text(again, "\x02"
						 "DEL up.qd\n");
		CHECK(read_reply(again, reply, sizeof(reply)));
		CHECK_STR(reply, "OK");
		CHECK(stat(landed, &st) != 0);
		close(again);
	}

	hal.shutdown(&hal);

	// The link goes with it
	CHECK(access(link, F_OK) != 0);
}

int main(void) {
	test_shutdown_returns();
	test_it_can_be_started_twice();
	test_qdosctl_reaches_it();
	return check_report("simhal");
}
