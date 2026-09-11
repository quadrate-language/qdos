/**
 * @file keypad.c
 * @brief Reading the calculator keypad through evdev
 */

// POSIX interfaces on top of a strict c11 build
#define _POSIX_C_SOURCE 200809L

#include "keypad.h"

#include <fcntl.h>
#include <linux/input.h>
#include <unistd.h>

bool qdos_keypad_map(uint16_t code, qdos_key_event* out) {
	if (out == NULL) {
		return false;
	}
	out->ch = 0;

	switch (code) {
		// Both the main row and the numeric keypad, so a USB keyboard works for
		// bring-up before the real keypad exists
		case KEY_0: case KEY_NUMERIC_0: case KEY_KP0: out->key = QDOS_KEY_0; return true;
		case KEY_1: case KEY_NUMERIC_1: case KEY_KP1: out->key = QDOS_KEY_1; return true;
		case KEY_2: case KEY_NUMERIC_2: case KEY_KP2: out->key = QDOS_KEY_2; return true;
		case KEY_3: case KEY_NUMERIC_3: case KEY_KP3: out->key = QDOS_KEY_3; return true;
		case KEY_4: case KEY_NUMERIC_4: case KEY_KP4: out->key = QDOS_KEY_4; return true;
		case KEY_5: case KEY_NUMERIC_5: case KEY_KP5: out->key = QDOS_KEY_5; return true;
		case KEY_6: case KEY_NUMERIC_6: case KEY_KP6: out->key = QDOS_KEY_6; return true;
		case KEY_7: case KEY_NUMERIC_7: case KEY_KP7: out->key = QDOS_KEY_7; return true;
		case KEY_8: case KEY_NUMERIC_8: case KEY_KP8: out->key = QDOS_KEY_8; return true;
		case KEY_9: case KEY_NUMERIC_9: case KEY_KP9: out->key = QDOS_KEY_9; return true;

		case KEY_DOT: case KEY_KPDOT: out->key = QDOS_KEY_DOT; return true;
		case KEY_KPPLUS: out->key = QDOS_KEY_ADD; return true;
		case KEY_MINUS: case KEY_KPMINUS: out->key = QDOS_KEY_SUB; return true;
		case KEY_KPASTERISK: out->key = QDOS_KEY_MUL; return true;
		case KEY_SLASH: case KEY_KPSLASH: out->key = QDOS_KEY_DIV; return true;

		case KEY_ENTER: case KEY_KPENTER: out->key = QDOS_KEY_ENTER; return true;
		case KEY_BACKSPACE: out->key = QDOS_KEY_BACKSPACE; return true;
		case KEY_ESC: out->key = QDOS_KEY_CLEAR; return true;
		case KEY_POWER: out->key = QDOS_KEY_POWER; return true;

		case KEY_SPACE: out->key = QDOS_KEY_CHAR; out->ch = ' '; return true;

		default:
			return false;
	}
}

int qdos_keypad_open(const char* path) {
	if (path == NULL) {
		return -1;
	}
	return open(path, O_RDONLY | O_NONBLOCK);
}

bool qdos_keypad_poll(int fd, qdos_key_event* out) {
	if (fd < 0 || out == NULL) {
		return false;
	}

	struct input_event ev;
	while (read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
		// value 1 is a press, 2 an auto-repeat, 0 a release. Only presses act;
		// a calculator that repeated on a held key would be unusable.
		if (ev.type != EV_KEY || ev.value != 1) {
			continue;
		}
		if (qdos_keypad_map(ev.code, out)) {
			return true;
		}
	}
	return false;
}

void qdos_keypad_close(int fd) {
	if (fd >= 0) {
		close(fd);
	}
}
