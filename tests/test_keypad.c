/**
 * @file test_keypad.c
 * @brief Keypad tests, including one against a real kernel input device
 */

// POSIX interfaces on top of a strict c11 build
#define _POSIX_C_SOURCE 200809L

#include "check.h"

#include "../src/hal/device/keypad.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/**
 * The key codes do not move. They are what `ui::key` gives a Quadrate
 * program and what a native module compares against, and neither is rebuilt
 * with QDOS: DOOM's controls are these numbers written out. A new key goes at
 * the end of the enum, never in the middle.
 */
static void test_key_codes_do_not_move(void) {
	CHECK(QDOS_KEY_0 == 1 && QDOS_KEY_9 == 10);
	CHECK(QDOS_KEY_DOT == 11);
	CHECK(QDOS_KEY_ADD == 12 && QDOS_KEY_DIV == 15);
	CHECK(QDOS_KEY_SIN == 20 && QDOS_KEY_OVER == 35);
	CHECK(QDOS_KEY_ENTER == 36);
	CHECK(QDOS_KEY_BACKSPACE == 37);
	CHECK(QDOS_KEY_CLEAR == 39);
	CHECK(QDOS_KEY_POWER == 40);
	CHECK(QDOS_KEY_UP == 41 && QDOS_KEY_DOWN == 42 && QDOS_KEY_LEFT == 43 && QDOS_KEY_RIGHT == 44);
	CHECK(QDOS_KEY_RCL == 56);
	CHECK(QDOS_KEY_SOFT1 == 57 && QDOS_KEY_SOFT5 == 61);
	CHECK(QDOS_KEY_CHAR == 62);

	// And what came after, from where it came
	CHECK(QDOS_KEY_ASIN == 63);
	CHECK(QDOS_KEY_GRAPH == 70);
	CHECK(QDOS_KEY_TOGGLE == 71 && QDOS_KEY_VAR_X == 72 && QDOS_KEY_VAR_Y == 73);
}

static void test_mapping(void) {
	qdos_key_event ev;

	// Digits, from both the main row and the numeric keypad
	CHECK(qdos_keypad_map(KEY_1, false, false, &ev) && ev.key == QDOS_KEY_1);
	CHECK(qdos_keypad_map(KEY_KP1, false, false, &ev) && ev.key == QDOS_KEY_1);
	CHECK(qdos_keypad_map(KEY_0, false, false, &ev) && ev.key == QDOS_KEY_0);
	CHECK(qdos_keypad_map(KEY_9, false, false, &ev) && ev.key == QDOS_KEY_9);

	// Operators
	CHECK(qdos_keypad_map(KEY_KPPLUS, false, false, &ev) && ev.key == QDOS_KEY_ADD);
	CHECK(qdos_keypad_map(KEY_MINUS, false, false, &ev) && ev.key == QDOS_KEY_SUB);
	CHECK(qdos_keypad_map(KEY_KPASTERISK, false, false, &ev) && ev.key == QDOS_KEY_MUL);
	CHECK(qdos_keypad_map(KEY_SLASH, false, false, &ev) && ev.key == QDOS_KEY_DIV);

	// Control
	CHECK(qdos_keypad_map(KEY_ENTER, false, false, &ev) && ev.key == QDOS_KEY_ENTER);
	CHECK(qdos_keypad_map(KEY_BACKSPACE, false, false, &ev) && ev.key == QDOS_KEY_BACKSPACE);
	CHECK(qdos_keypad_map(KEY_ESC, false, false, &ev) && ev.key == QDOS_KEY_CLEAR);
	CHECK(qdos_keypad_map(KEY_POWER, false, false, &ev) && ev.key == QDOS_KEY_POWER);

	// Space carries its character so it can reach the input line
	CHECK(qdos_keypad_map(KEY_SPACE, false, false, &ev) && ev.key == QDOS_KEY_CHAR && ev.ch == ' ');

	// Keys the calculator has no use for must be rejected, not mapped to
	// something arbitrary
	CHECK(!qdos_keypad_map(KEY_F9, false, false, &ev));
	CHECK(!qdos_keypad_map(KEY_LEFTSHIFT, false, false, &ev));
	CHECK(!qdos_keypad_map(KEY_CAPSLOCK, false, false, &ev));

	CHECK(qdos_keypad_map(KEY_UP, false, false, &ev) && ev.key == QDOS_KEY_UP);
	CHECK(qdos_keypad_map(KEY_DOWN, false, false, &ev) && ev.key == QDOS_KEY_DOWN);
	CHECK(qdos_keypad_map(KEY_LEFT, false, false, &ev) && ev.key == QDOS_KEY_LEFT);
	CHECK(qdos_keypad_map(KEY_RIGHT, false, false, &ev) && ev.key == QDOS_KEY_RIGHT);
	CHECK(qdos_keypad_map(KEY_F1, false, false, &ev) && ev.key == QDOS_KEY_SOFT1);
	CHECK(qdos_keypad_map(KEY_F5, false, false, &ev) && ev.key == QDOS_KEY_SOFT5);

	// Line mode needs a full keyboard: a keypad has no letters, but the machine
	// is used with a USB keyboard long before it has its own keys
	CHECK(qdos_keypad_map(KEY_SEMICOLON, true, false, &ev) && ev.key == QDOS_KEY_CHAR && ev.ch == ':');
	CHECK(qdos_keypad_map(KEY_SEMICOLON, false, false, &ev) && ev.ch == ';');
	CHECK(qdos_keypad_map(KEY_A, false, false, &ev) && ev.key == QDOS_KEY_CHAR && ev.ch == 'a');
	CHECK(qdos_keypad_map(KEY_A, true, false, &ev) && ev.ch == 'A');
	CHECK(qdos_keypad_map(KEY_LEFTBRACE, true, false, &ev) && ev.ch == '{');
	CHECK(qdos_keypad_map(KEY_RIGHTBRACE, true, false, &ev) && ev.ch == '}');
	CHECK(qdos_keypad_map(KEY_APOSTROPHE, true, false, &ev) && ev.ch == '"');
	CHECK(qdos_keypad_map(KEY_EQUAL, true, false, &ev) && ev.key == QDOS_KEY_ADD);
	CHECK(qdos_keypad_map(KEY_8, true, false, &ev) && ev.key == QDOS_KEY_MUL);
	CHECK(qdos_keypad_map(KEY_COMMA, true, false, &ev) && ev.ch == '<');

	// A digit is the calculator's digit key from either row
	CHECK(qdos_keypad_map(KEY_1, false, false, &ev) && ev.key == QDOS_KEY_1);
	CHECK(qdos_keypad_map(KEY_KP1, false, false, &ev) && ev.key == QDOS_KEY_1);
	CHECK(qdos_keypad_map(KEY_1, true, false, &ev) && ev.ch == '!');

	// Shift itself is a modifier, never an event
	CHECK(!qdos_keypad_map(KEY_LEFTSHIFT, false, false, &ev));
	CHECK(!qdos_keypad_map(KEY_RIGHTSHIFT, false, false, &ev));

	// evdev gives the key, not the label, so the layout decides the character.
	// On a Swedish keyboard the braces a function declaration needs are on
	// AltGr, and the parentheses sit one key left of where US puts them.
	CHECK(qdos_keypad_set_layout("se"));
	CHECK(qdos_keypad_layout() == QDOS_LAYOUT_SE);

	CHECK(qdos_keypad_map(KEY_7, false, true, &ev) && ev.ch == '{');
	CHECK(qdos_keypad_map(KEY_0, false, true, &ev) && ev.ch == '}');
	CHECK(qdos_keypad_map(KEY_8, true, false, &ev) && ev.ch == '(');
	CHECK(qdos_keypad_map(KEY_9, true, false, &ev) && ev.ch == ')');
	CHECK(qdos_keypad_map(KEY_2, true, false, &ev) && ev.ch == '"');
	CHECK(qdos_keypad_map(KEY_DOT, true, false, &ev) && ev.ch == ':');
	CHECK(qdos_keypad_map(KEY_SLASH, false, false, &ev) && ev.key == QDOS_KEY_SUB);
	CHECK(qdos_keypad_map(KEY_7, true, false, &ev) && ev.key == QDOS_KEY_DIV);
	CHECK(qdos_keypad_map(KEY_MINUS, false, false, &ev) && ev.key == QDOS_KEY_ADD);
	CHECK(qdos_keypad_map(KEY_BACKSLASH, true, false, &ev) && ev.key == QDOS_KEY_MUL);
	CHECK(qdos_keypad_map(KEY_102ND, true, false, &ev) && ev.ch == '>');

	// Letters are the same on both, and the calculator keys never move
	CHECK(qdos_keypad_map(KEY_A, false, false, &ev) && ev.ch == 'a');
	CHECK(qdos_keypad_map(KEY_KP1, false, false, &ev) && ev.key == QDOS_KEY_1);

	CHECK(qdos_keypad_set_layout("us"));
	CHECK(qdos_keypad_map(KEY_LEFTBRACE, true, false, &ev) && ev.ch == '{');
	CHECK(qdos_keypad_map(KEY_9, true, false, &ev) && ev.ch == '(');

	CHECK(!qdos_keypad_set_layout("klingon"));
	CHECK(qdos_keypad_layout() == QDOS_LAYOUT_US); // unchanged by a bad name
	CHECK(!qdos_keypad_set_layout(NULL));

	CHECK(!qdos_keypad_map(KEY_1, false, false, NULL));
}

static void test_poll_guards(void) {
	qdos_key_event ev;
	CHECK(!qdos_keypad_poll(-1, &ev)); // no keypad attached is not an error
	CHECK(!qdos_keypad_poll(0, NULL));
	qdos_keypad_close(-1); // must not crash
}

/* ---------------------------------------------------------------------- */
/* Poll over a pipe                                                       */
/*                                                                        */
/* qdos_keypad_poll() wants a descriptor that yields input_event records, */
/* and a pipe is one. The real device test below needs /dev/uinput and is */
/* skipped wherever that is not there, so the modifier state machine is   */
/* driven here instead, where it always runs.                             */
/* ---------------------------------------------------------------------- */

typedef struct {
	int read_fd;
	int write_fd;
} fake_pad;

static bool fake_pad_open(fake_pad* pad) {
	int fds[2];
	if (pipe(fds) != 0) {
		return false;
	}

	// qdos_keypad_open() opens the device non-blocking; the read loop relies on
	// that to stop at the end of the queue rather than waiting for more
	fcntl(fds[0], F_SETFL, O_NONBLOCK);
	pad->read_fd = fds[0];
	pad->write_fd = fds[1];
	return true;
}

static void fake_pad_close(fake_pad* pad) {
	close(pad->read_fd);
	close(pad->write_fd);
}

static void feed(const fake_pad* pad, uint16_t type, uint16_t code, int32_t value) {
	struct input_event ev;
	memset(&ev, 0, sizeof(ev));
	ev.type = type;
	ev.code = code;
	ev.value = value;
	CHECK(write(pad->write_fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev));
}

/** 1 is a press, as the kernel reports it */
static void press(const fake_pad* pad, uint16_t code) {
	feed(pad, EV_KEY, code, 1);
}

static void release(const fake_pad* pad, uint16_t code) {
	feed(pad, EV_KEY, code, 0);
}

/** Poll once and report the key, or QDOS_KEY_NONE if nothing was delivered */
static qdos_key polled(const fake_pad* pad, char* ch) {
	qdos_key_event ev;
	memset(&ev, 0, sizeof(ev));
	if (!qdos_keypad_poll(pad->read_fd, &ev)) {
		if (ch) {
			*ch = 0;
		}
		return QDOS_KEY_NONE;
	}
	if (ch) {
		*ch = ev.ch;
	}
	return ev.key;
}

/** Only presses of keys the calculator knows come back out. */
static void test_poll_filters_the_queue(void) {
	fake_pad pad;
	if (!fake_pad_open(&pad)) {
		CHECK(0 && "cannot make a pipe");
		return;
	}

	// Anything that is not a key press is not a key: the kernel frames every
	// report with EV_SYN, and EV_MSC arrives alongside on real keyboards
	feed(&pad, EV_SYN, SYN_REPORT, 0);
	feed(&pad, EV_MSC, MSC_SCAN, 0x1e);
	press(&pad, KEY_1);
	CHECK(polled(&pad, NULL) == QDOS_KEY_1);

	// A release is the other edge of a press already reported, and value 2 is
	// autorepeat. Neither is a new press.
	release(&pad, KEY_2);
	feed(&pad, EV_KEY, KEY_2, 2);
	CHECK(polled(&pad, NULL) == QDOS_KEY_NONE);

	press(&pad, KEY_2);
	CHECK(polled(&pad, NULL) == QDOS_KEY_2);

	// A key with no meaning here is skipped over rather than ending the walk,
	// or one stray key would swallow the press behind it
	press(&pad, KEY_F9);
	press(&pad, KEY_3);
	CHECK(polled(&pad, NULL) == QDOS_KEY_3);

	CHECK(polled(&pad, NULL) == QDOS_KEY_NONE); // drained

	fake_pad_close(&pad);
}

/**
 * Modifiers are held between polls. Shift arrives as its own event well before
 * the key it shifts, and the two are almost never read in the same poll.
 */
static void test_poll_holds_the_modifiers(void) {
	fake_pad pad;
	if (!fake_pad_open(&pad)) {
		CHECK(0 && "cannot make a pipe");
		return;
	}

	char ch = 0;

	press(&pad, KEY_LEFTSHIFT);
	CHECK(polled(&pad, NULL) == QDOS_KEY_NONE); // a modifier is not a press

	press(&pad, KEY_1);
	CHECK(polled(&pad, &ch) == QDOS_KEY_CHAR);
	CHECK(ch == '!'); // the shift held across a separate poll

	release(&pad, KEY_LEFTSHIFT);
	CHECK(polled(&pad, NULL) == QDOS_KEY_NONE);

	press(&pad, KEY_1);
	CHECK(polled(&pad, NULL) == QDOS_KEY_1); // and let go again

	// The right-hand one is the same key as far as the layout is concerned
	press(&pad, KEY_RIGHTSHIFT);
	CHECK(polled(&pad, NULL) == QDOS_KEY_NONE);
	press(&pad, KEY_A);
	CHECK(polled(&pad, &ch) == QDOS_KEY_CHAR);
	CHECK(ch == 'A');
	release(&pad, KEY_RIGHTSHIFT);
	CHECK(polled(&pad, NULL) == QDOS_KEY_NONE);

	fake_pad_close(&pad);
}

/** AltGr is the third level, and the Swedish layout is where QDOS needs it. */
static void test_poll_holds_altgr(void) {
	fake_pad pad;
	if (!fake_pad_open(&pad)) {
		CHECK(0 && "cannot make a pipe");
		return;
	}

	CHECK(qdos_keypad_set_layout("se"));
	char ch = 0;

	press(&pad, KEY_RIGHTALT);
	CHECK(polled(&pad, NULL) == QDOS_KEY_NONE);

	press(&pad, KEY_7);
	CHECK(polled(&pad, &ch) == QDOS_KEY_CHAR);
	CHECK(ch == '{'); // a declaration's brace, which SE puts on AltGr

	release(&pad, KEY_RIGHTALT);
	CHECK(polled(&pad, NULL) == QDOS_KEY_NONE);

	press(&pad, KEY_7);
	CHECK(polled(&pad, NULL) == QDOS_KEY_7); // plain again, and a digit key

	// SE leaves shift+4 undefined. Nothing is better than the wrong character.
	press(&pad, KEY_LEFTSHIFT);
	CHECK(polled(&pad, NULL) == QDOS_KEY_NONE);
	press(&pad, KEY_4);
	CHECK(polled(&pad, NULL) == QDOS_KEY_NONE);
	release(&pad, KEY_LEFTSHIFT);
	CHECK(polled(&pad, NULL) == QDOS_KEY_NONE);

	press(&pad, KEY_4);
	CHECK(polled(&pad, NULL) == QDOS_KEY_4);

	CHECK(qdos_keypad_set_layout("us"));
	fake_pad_close(&pad);
}

/** A record cut short is the end of the queue, not a key. */
static void test_poll_ignores_a_partial_record(void) {
	fake_pad pad;
	if (!fake_pad_open(&pad)) {
		CHECK(0 && "cannot make a pipe");
		return;
	}

	struct input_event ev;
	memset(&ev, 0, sizeof(ev));
	ev.type = EV_KEY;
	ev.code = KEY_5;
	ev.value = 1;
	CHECK(write(pad.write_fd, &ev, sizeof(ev) / 2) == (ssize_t)(sizeof(ev) / 2));

	CHECK(polled(&pad, NULL) == QDOS_KEY_NONE);

	fake_pad_close(&pad);
}

/* ---------------------------------------------------------------------- */
/* Virtual keypad via uinput                                              */
/* ---------------------------------------------------------------------- */

static const int INJECTED_KEYS[] = {
		KEY_1,
		KEY_SPACE,
		KEY_2,
		KEY_KPPLUS,
		KEY_ENTER,
		KEY_ESC,
		KEY_BACKSPACE,
};
#define INJECTED_COUNT ((int)(sizeof(INJECTED_KEYS) / sizeof(INJECTED_KEYS[0])))

/** Create a virtual keypad. Returns the uinput fd, or -1 if unavailable. */
static int uinput_create(void) {
	int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
	if (fd < 0) {
		return -1;
	}

	if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0) {
		goto fail;
	}
	for (int i = 0; i < INJECTED_COUNT; i++) {
		if (ioctl(fd, UI_SET_KEYBIT, INJECTED_KEYS[i]) < 0) {
			goto fail;
		}
	}

	struct uinput_setup setup;
	memset(&setup, 0, sizeof(setup));
	setup.id.bustype = BUS_VIRTUAL;
	setup.id.vendor = 0x4744; // "GD"
	setup.id.product = 0x0501;
	snprintf(setup.name, sizeof(setup.name), "qdos-test-keypad");

	if (ioctl(fd, UI_DEV_SETUP, &setup) < 0) {
		goto fail;
	}
	if (ioctl(fd, UI_DEV_CREATE) < 0) {
		goto fail;
	}

	return fd;
fail:
	close(fd);
	return -1;
}

/**
 */
static bool uinput_event_path(int uifd, char* path, size_t cap) {
	char sysname[64];
	if (ioctl(uifd, UI_GET_SYSNAME(sizeof(sysname)), sysname) < 0) {
		return false;
	}

	char dirpath[128];
	snprintf(dirpath, sizeof(dirpath), "/sys/devices/virtual/input/%s", sysname);

	DIR* dir = opendir(dirpath);
	if (!dir) {
		return false;
	}

	bool found = false;
	const struct dirent* entry;
	while ((entry = readdir(dir)) != NULL) {
		if (strncmp(entry->d_name, "event", 5) == 0) {
			snprintf(path, cap, "/dev/input/%s", entry->d_name);
			found = true;
			break;
		}
	}
	closedir(dir);
	return found;
}

static void inject(int uifd, int code) {
	struct input_event ev[2];
	memset(ev, 0, sizeof(ev));

	ev[0].type = EV_KEY;
	ev[0].code = (unsigned short)code;
	ev[0].value = 1; // press

	ev[1].type = EV_SYN;
	ev[1].code = SYN_REPORT;

	if (write(uifd, ev, sizeof(ev)) != (ssize_t)sizeof(ev)) {
		fprintf(stderr, "  warning: injection write failed: %s\n", strerror(errno));
	}
}

/** Wait briefly for an event, since delivery is not instant. */
static bool poll_with_timeout(int fd, qdos_key_event* out, int attempts) {
	for (int i = 0; i < attempts; i++) {
		if (qdos_keypad_poll(fd, out)) {
			return true;
		}
		const struct timespec pause = {.tv_sec = 0, .tv_nsec = 10 * 1000 * 1000};
		nanosleep(&pause, NULL);
	}
	return false;
}

static void test_real_input_device(void) {
	const int uifd = uinput_create();
	if (uifd < 0) {
		printf("keypad: SKIP real-device test (/dev/uinput unavailable: %s)\n", strerror(errno));
		return;
	}

	char path[128];
	bool located = false;
	// udev needs a moment to create the node and apply permissions
	for (int i = 0; i < 100 && !located; i++) {
		located = uinput_event_path(uifd, path, sizeof(path));
		if (!located) {
			const struct timespec pause = {.tv_sec = 0, .tv_nsec = 10 * 1000 * 1000};
			nanosleep(&pause, NULL);
		}
	}
	if (!located) {
		printf("keypad: SKIP real-device test (event node never appeared)\n");
		ioctl(uifd, UI_DEV_DESTROY);
		close(uifd);
		return;
	}

	int fd = -1;
	for (int i = 0; i < 100 && fd < 0; i++) {
		fd = qdos_keypad_open(path);
		if (fd < 0) {
			const struct timespec pause = {.tv_sec = 0, .tv_nsec = 10 * 1000 * 1000};
			nanosleep(&pause, NULL);
		}
	}
	if (fd < 0) {
		printf("keypad: SKIP real-device test (cannot read %s: %s)\n", path, strerror(errno));
		ioctl(uifd, UI_DEV_DESTROY);
		close(uifd);
		return;
	}

	printf("keypad: driving a real kernel input device at %s\n", path);

	// Drain anything queued from device creation
	qdos_key_event ev;
	while (qdos_keypad_poll(fd, &ev)) {
	}

	// "1 2 +" then Enter, through actual kernel events
	const struct {
		int code;
		qdos_key expected;
	} script[] = {
			{KEY_1, QDOS_KEY_1},
			{KEY_SPACE, QDOS_KEY_CHAR},
			{KEY_2, QDOS_KEY_2},
			{KEY_KPPLUS, QDOS_KEY_ADD},
			{KEY_ENTER, QDOS_KEY_ENTER},
			{KEY_ESC, QDOS_KEY_CLEAR},
			{KEY_BACKSPACE, QDOS_KEY_BACKSPACE},
	};

	for (size_t i = 0; i < sizeof(script) / sizeof(script[0]); i++) {
		inject(uifd, script[i].code);
		if (poll_with_timeout(fd, &ev, 50)) {
			CHECK(ev.key == script[i].expected);
		} else {
			CHECK(0 && "injected key never arrived");
		}
	}

	qdos_keypad_close(fd);
	ioctl(uifd, UI_DEV_DESTROY);
	close(uifd);
}

int main(void) {
	test_key_codes_do_not_move();
	test_mapping();
	test_poll_guards();
	test_poll_filters_the_queue();
	test_poll_holds_the_modifiers();
	test_poll_holds_altgr();
	test_poll_ignores_a_partial_record();
	test_real_input_device();
	return check_report("keypad");
}
