/**
 * @file keypad.c
 * @brief Reading the calculator keypad through evdev
 */

// POSIX interfaces on top of a strict c11 build
#define _POSIX_C_SOURCE 200809L

#include "keypad.h"

#include <fcntl.h>
#include <linux/input.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/**
 */
typedef struct {
	uint16_t code;
	char plain;
	char shifted;
	char altgr; // 0 where the layout has no third level
} layout_key;

static const layout_key LAYOUT_US[] = {
		{KEY_A, 'a', 'A', 0}, {KEY_B, 'b', 'B', 0}, {KEY_C, 'c', 'C', 0}, {KEY_D, 'd', 'D', 0},
		{KEY_E, 'e', 'E', 0}, {KEY_F, 'f', 'F', 0}, {KEY_G, 'g', 'G', 0}, {KEY_H, 'h', 'H', 0},
		{KEY_I, 'i', 'I', 0}, {KEY_J, 'j', 'J', 0}, {KEY_K, 'k', 'K', 0}, {KEY_L, 'l', 'L', 0},
		{KEY_M, 'm', 'M', 0}, {KEY_N, 'n', 'N', 0}, {KEY_O, 'o', 'O', 0}, {KEY_P, 'p', 'P', 0},
		{KEY_Q, 'q', 'Q', 0}, {KEY_R, 'r', 'R', 0}, {KEY_S, 's', 'S', 0}, {KEY_T, 't', 'T', 0},
		{KEY_U, 'u', 'U', 0}, {KEY_V, 'v', 'V', 0}, {KEY_W, 'w', 'W', 0}, {KEY_X, 'x', 'X', 0},
		{KEY_Y, 'y', 'Y', 0}, {KEY_Z, 'z', 'Z', 0},

		{KEY_1, '1', '!', 0}, {KEY_2, '2', '@', 0}, {KEY_3, '3', '#', 0}, {KEY_4, '4', '$', 0},
		{KEY_5, '5', '%', 0}, {KEY_6, '6', '^', 0}, {KEY_7, '7', '&', 0}, {KEY_8, '8', '*', 0},
		{KEY_9, '9', '(', 0}, {KEY_0, '0', ')', 0},

		{KEY_MINUS, '-', '_', 0},
		{KEY_EQUAL, '=', '+', 0},
		{KEY_LEFTBRACE, '[', '{', 0},
		{KEY_RIGHTBRACE, ']', '}', 0},
		{KEY_SEMICOLON, ';', ':', 0},
		{KEY_APOSTROPHE, '\'', '"', 0},
		{KEY_GRAVE, '`', '~', 0},
		{KEY_BACKSLASH, '\\', '|', 0},
		{KEY_COMMA, ',', '<', 0},
		{KEY_DOT, '.', '>', 0},
		{KEY_SLASH, '/', '?', 0},
		{KEY_SPACE, ' ', ' ', 0},
};

/**
 */
static const layout_key LAYOUT_SE[] = {
		{KEY_A, 'a', 'A', 0}, {KEY_B, 'b', 'B', 0}, {KEY_C, 'c', 'C', 0}, {KEY_D, 'd', 'D', 0},
		{KEY_E, 'e', 'E', 0}, {KEY_F, 'f', 'F', 0}, {KEY_G, 'g', 'G', 0}, {KEY_H, 'h', 'H', 0},
		{KEY_I, 'i', 'I', 0}, {KEY_J, 'j', 'J', 0}, {KEY_K, 'k', 'K', 0}, {KEY_L, 'l', 'L', 0},
		{KEY_M, 'm', 'M', 0}, {KEY_N, 'n', 'N', 0}, {KEY_O, 'o', 'O', 0}, {KEY_P, 'p', 'P', 0},
		{KEY_Q, 'q', 'Q', 0}, {KEY_R, 'r', 'R', 0}, {KEY_S, 's', 'S', 0}, {KEY_T, 't', 'T', 0},
		{KEY_U, 'u', 'U', 0}, {KEY_V, 'v', 'V', 0}, {KEY_W, 'w', 'W', 0}, {KEY_X, 'x', 'X', 0},
		{KEY_Y, 'y', 'Y', 0}, {KEY_Z, 'z', 'Z', 0},

		{KEY_1, '1', '!', 0},
		{KEY_2, '2', '"', '@'},
		{KEY_3, '3', '#', 0},
		{KEY_4, '4', 0, '$'},
		{KEY_5, '5', '%', 0},
		{KEY_6, '6', '&', 0},
		{KEY_7, '7', '/', '{'},
		{KEY_8, '8', '(', '['},
		{KEY_9, '9', ')', ']'},
		{KEY_0, '0', '=', '}'},

		{KEY_MINUS, '+', '?', '\\'},
		{KEY_BACKSLASH, '\'', '*', 0},
		{KEY_COMMA, ',', ';', 0},
		{KEY_DOT, '.', ':', 0},
		{KEY_SLASH, '-', '_', 0},
		{KEY_102ND, '<', '>', '|'},
		{KEY_SPACE, ' ', ' ', 0},
};

#define LAYOUT_US_COUNT (sizeof(LAYOUT_US) / sizeof(LAYOUT_US[0]))
#define LAYOUT_SE_COUNT (sizeof(LAYOUT_SE) / sizeof(LAYOUT_SE[0]))

static qdos_layout g_layout = QDOS_LAYOUT_US;

bool qdos_keypad_set_layout(const char* name) {
	if (name == NULL) {
		return false;
	}
	if (strcmp(name, "us") == 0) {
		g_layout = QDOS_LAYOUT_US;
		return true;
	}
	if (strcmp(name, "se") == 0) {
		g_layout = QDOS_LAYOUT_SE;
		return true;
	}
	return false;
}

qdos_layout qdos_keypad_layout(void) {
	return g_layout;
}

bool qdos_keypad_map(uint16_t code, bool shift, bool altgr, qdos_key_event* out) {
	if (out == NULL) {
		return false;
	}
	out->ch = 0;

	// The calculator's own keys act rather than type.
	switch (code) {
		case KEY_NUMERIC_0: case KEY_KP0: out->key = QDOS_KEY_0; return true;
		case KEY_NUMERIC_1: case KEY_KP1: out->key = QDOS_KEY_1; return true;
		case KEY_NUMERIC_2: case KEY_KP2: out->key = QDOS_KEY_2; return true;
		case KEY_NUMERIC_3: case KEY_KP3: out->key = QDOS_KEY_3; return true;
		case KEY_NUMERIC_4: case KEY_KP4: out->key = QDOS_KEY_4; return true;
		case KEY_NUMERIC_5: case KEY_KP5: out->key = QDOS_KEY_5; return true;
		case KEY_NUMERIC_6: case KEY_KP6: out->key = QDOS_KEY_6; return true;
		case KEY_NUMERIC_7: case KEY_KP7: out->key = QDOS_KEY_7; return true;
		case KEY_NUMERIC_8: case KEY_KP8: out->key = QDOS_KEY_8; return true;
		case KEY_NUMERIC_9: case KEY_KP9: out->key = QDOS_KEY_9; return true;

		case KEY_KPDOT: out->key = QDOS_KEY_DOT; return true;
		case KEY_KPPLUS: out->key = QDOS_KEY_ADD; return true;
		case KEY_KPMINUS: out->key = QDOS_KEY_SUB; return true;
		case KEY_KPASTERISK: out->key = QDOS_KEY_MUL; return true;
		case KEY_KPSLASH: out->key = QDOS_KEY_DIV; return true;

		case KEY_ENTER: case KEY_KPENTER: out->key = QDOS_KEY_ENTER; return true;
		case KEY_BACKSPACE: out->key = QDOS_KEY_BACKSPACE; return true;
		case KEY_TAB: out->key = QDOS_KEY_TAB; return true;
		case KEY_ESC: out->key = QDOS_KEY_CLEAR; return true;
		case KEY_POWER: out->key = QDOS_KEY_POWER; return true;

		default:
			break;
	}

	const layout_key* table = (g_layout == QDOS_LAYOUT_SE) ? LAYOUT_SE : LAYOUT_US;
	const size_t count = (g_layout == QDOS_LAYOUT_SE) ? LAYOUT_SE_COUNT : LAYOUT_US_COUNT;

	for (size_t i = 0; i < count; i++) {
		if (table[i].code != code) {
			continue;
		}

		char ch = table[i].plain;
		if (altgr && table[i].altgr != 0) {
			ch = table[i].altgr;
		} else if (shift) {
			ch = table[i].shifted;
		}
		if (ch == 0) {
			return false; // a level this layout does not define
		}

		if (ch >= '0' && ch <= '9') {
			out->key = (qdos_key)(QDOS_KEY_0 + (ch - '0'));
			return true;
		}
		switch (ch) {
			case '.': out->key = QDOS_KEY_DOT; return true;
			case '+': out->key = QDOS_KEY_ADD; return true;
			case '-': out->key = QDOS_KEY_SUB; return true;
			case '*': out->key = QDOS_KEY_MUL; return true;
			case '/': out->key = QDOS_KEY_DIV; return true;
			default:
				out->key = QDOS_KEY_CHAR;
				out->ch = ch;
				return true;
		}
	}
	return false;
}

int qdos_keypad_open(const char* path) {
	if (path == NULL) {
		return -1;
	}

	const int fd = open(path, O_RDONLY | O_NONBLOCK);
	if (fd < 0) {
		return -1;
	}

	// Exclusive, or the console echoes every key onto the framebuffer.
	ioctl(fd, EVIOCGRAB, 1);

	return fd;
}

// File scope: one keypad, and this survives between polls.
static bool g_shift_held = false;
static bool g_altgr_held = false;

bool qdos_keypad_poll(int fd, qdos_key_event* out) {
	if (fd < 0 || out == NULL) {
		return false;
	}

	struct input_event ev;
	while (read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
		if (ev.type != EV_KEY) {
			continue;
		}

		// Modifiers are not key presses: track both edges
		if (ev.code == KEY_LEFTSHIFT || ev.code == KEY_RIGHTSHIFT) {
			g_shift_held = (ev.value != 0);
			continue;
		}
		if (ev.code == KEY_RIGHTALT) {
			g_altgr_held = (ev.value != 0);
			continue;
		}

		// 1 is a press, 2 autorepeat, 0 release; only presses act.
		if (ev.value != 1) {
			continue;
		}
		if (qdos_keypad_map(ev.code, g_shift_held, g_altgr_held, out)) {
			return true;
		}
	}
	return false;
}

void qdos_keypad_close(int fd) {
	if (fd >= 0) {
		ioctl(fd, EVIOCGRAB, 0);
		close(fd);
	}
}
