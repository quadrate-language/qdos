/**
 * @file test_keypad.c
 * @brief Keypad tests, including one against a real kernel input device
 *
 * The mapping tests run anywhere. The device test creates a virtual keypad
 * through /dev/uinput, injects real key presses, and reads them back through
 * the same code the Pi will use — so the evdev path is exercised against the
 * kernel rather than a mock, without hardware and without root.
 *
 * It skips when /dev/uinput is unavailable, which is the normal case in a
 * container or on a locked-down host.
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

static void test_mapping(void) {
	qdos_key_event ev;

	// Digits, from both the main row and the numeric keypad
	CHECK(qdos_keypad_map(KEY_1, &ev) && ev.key == QDOS_KEY_1);
	CHECK(qdos_keypad_map(KEY_KP1, &ev) && ev.key == QDOS_KEY_1);
	CHECK(qdos_keypad_map(KEY_0, &ev) && ev.key == QDOS_KEY_0);
	CHECK(qdos_keypad_map(KEY_9, &ev) && ev.key == QDOS_KEY_9);

	// Operators
	CHECK(qdos_keypad_map(KEY_KPPLUS, &ev) && ev.key == QDOS_KEY_ADD);
	CHECK(qdos_keypad_map(KEY_MINUS, &ev) && ev.key == QDOS_KEY_SUB);
	CHECK(qdos_keypad_map(KEY_KPASTERISK, &ev) && ev.key == QDOS_KEY_MUL);
	CHECK(qdos_keypad_map(KEY_SLASH, &ev) && ev.key == QDOS_KEY_DIV);

	// Control
	CHECK(qdos_keypad_map(KEY_ENTER, &ev) && ev.key == QDOS_KEY_ENTER);
	CHECK(qdos_keypad_map(KEY_BACKSPACE, &ev) && ev.key == QDOS_KEY_BACKSPACE);
	CHECK(qdos_keypad_map(KEY_ESC, &ev) && ev.key == QDOS_KEY_CLEAR);
	CHECK(qdos_keypad_map(KEY_POWER, &ev) && ev.key == QDOS_KEY_POWER);

	// Space carries its character so it can reach the input line
	CHECK(qdos_keypad_map(KEY_SPACE, &ev) && ev.key == QDOS_KEY_CHAR && ev.ch == ' ');

	// Keys the calculator has no use for must be rejected, not mapped to
	// something arbitrary
	CHECK(!qdos_keypad_map(KEY_F1, &ev));
	CHECK(!qdos_keypad_map(KEY_LEFTSHIFT, &ev));
	CHECK(!qdos_keypad_map(KEY_CAPSLOCK, &ev));

	CHECK(!qdos_keypad_map(KEY_1, NULL));
}

static void test_poll_guards(void) {
	qdos_key_event ev;
	CHECK(!qdos_keypad_poll(-1, &ev)); // no keypad attached is not an error
	CHECK(!qdos_keypad_poll(0, NULL));
	qdos_keypad_close(-1); // must not crash
}

/* ---------------------------------------------------------------------- */
/* Virtual keypad via uinput                                              */
/* ---------------------------------------------------------------------- */

static const int INJECTED_KEYS[] = {
	KEY_1, KEY_SPACE, KEY_2, KEY_KPPLUS, KEY_ENTER, KEY_ESC, KEY_BACKSPACE,
};
#define INJECTED_COUNT ((int)(sizeof(INJECTED_KEYS) / sizeof(INJECTED_KEYS[0])))

/** Create a virtual keypad. Returns the uinput fd, or -1 if unavailable. */
static int uinput_create(void) {
	int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
	if (fd < 0)
		return -1;

	if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0)
		goto fail;
	for (int i = 0; i < INJECTED_COUNT; i++)
		if (ioctl(fd, UI_SET_KEYBIT, INJECTED_KEYS[i]) < 0)
			goto fail;

	struct uinput_setup setup;
	memset(&setup, 0, sizeof(setup));
	setup.id.bustype = BUS_VIRTUAL;
	setup.id.vendor = 0x4744;  // "GD"
	setup.id.product = 0x0501;
	snprintf(setup.name, sizeof(setup.name), "qdos-test-keypad");

	if (ioctl(fd, UI_DEV_SETUP, &setup) < 0)
		goto fail;
	if (ioctl(fd, UI_DEV_CREATE) < 0)
		goto fail;

	return fd;
fail:
	close(fd);
	return -1;
}

/**
 * Find the event node the kernel created for our uinput device.
 *
 * UI_GET_SYSNAME names the sysfs input directory, which contains the eventN
 * directory. Going through sysfs avoids guessing at or scanning other people's
 * input devices.
 */
static bool uinput_event_path(int uifd, char* path, size_t cap) {
	char sysname[64];
	if (ioctl(uifd, UI_GET_SYSNAME(sizeof(sysname)), sysname) < 0)
		return false;

	char dirpath[128];
	snprintf(dirpath, sizeof(dirpath), "/sys/devices/virtual/input/%s", sysname);

	DIR* dir = opendir(dirpath);
	if (!dir)
		return false;

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

	if (write(uifd, ev, sizeof(ev)) != (ssize_t)sizeof(ev))
		fprintf(stderr, "  warning: injection write failed: %s\n", strerror(errno));
}

/** Wait briefly for an event, since delivery is not instant. */
static bool poll_with_timeout(int fd, qdos_key_event* out, int attempts) {
	for (int i = 0; i < attempts; i++) {
		if (qdos_keypad_poll(fd, out))
			return true;
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
	test_mapping();
	test_poll_guards();
	test_real_input_device();
	return check_report("keypad");
}
