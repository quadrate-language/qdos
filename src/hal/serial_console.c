/**
 * @file serial_console.c
 * @brief The calculator's serial console: a terminal as a keyboard, and uploads
 *
 * Bytes as a VT100-style terminal sends them: printable characters go through
 * the same mapping as the simulator's keyboard, and the escape sequences for
 * the arrows and function keys become the keys the simulator gives them -- F1
 * to F5 the soft row, F10 power, Escape on its own ESC.
 *
 * The port is the terminal, so an upload is framed to be told apart from
 * typing: a Ctrl-B, then one line naming the command, then for PUT the file's
 * bytes. Every answer is one line starting "QDUP ", which the host picks out
 * from among the log.
 *
 *   PUT <size> <crc32, hex> <path>   then <size> bytes
 *   DEL <path>                       a file, or an app folder and what is in it
 *   LS                               "QDUP F <size> <path>" per file, then OK
 *   HELLO                            OK, to show something is answering
 *
 * A path is a program or an app's file, as the stores hold them: "hello.qd" or
 * "doom/main.qd". A file is written beside itself and renamed into place once
 * its checksum holds, so the shell never reads half of one.
 */

#define _POSIX_C_SOURCE 200809L

#include "serial_console.h"

#include "hal/sim/keypad_ui.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ESC 0x1B
/** Ctrl-B: what follows is a command for the inbox */
#define STX 0x02
/** How long after an Escape the rest of a sequence may take to arrive */
#define SEQUENCE_MS 30

#define CMD_MAX 160
#define PATH_CAP 256
#define LINE_MS 1000 ///< Between the bytes of the command line
#define DATA_MS 3000 ///< Between the chunks of a file
#define SIZE_MAX_BYTES (8u * 1024u * 1024u)

uint32_t qdos_serial_crc32(uint32_t crc, const uint8_t* buf, size_t len) {
	crc = ~crc;
	for (size_t i = 0; i < len; i++) {
		crc ^= buf[i];
		for (int bit = 0; bit < 8; bit++) {
			crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
		}
	}
	return ~crc;
}

/* ---- keys ---------------------------------------------------------------- */

/** One byte, or -1 if none came within @p timeout_ms, or -2 if the port is gone */
static int read_byte(qdos_serial* s, int timeout_ms) {
	uint8_t b;
	const int got = s->read(s->user, &b, 1, timeout_ms);
	if (got < 0) {
		return -2;
	}
	return got == 1 ? b : -1;
}

static void send_key(qdos_serial* s, qdos_key key) {
	const qdos_key_event ev = {.key = key, .ch = 0};
	s->key(s->user, &ev);
}

/** What follows ESC [ or ESC O, up to its final byte */
static void sequence(qdos_serial* s, char intro) {
	char body[8];
	size_t n = 0;
	for (;;) {
		const int b = read_byte(s, SEQUENCE_MS);
		if (b < 0) {
			return;
		}
		if (n + 1 < sizeof(body)) {
			body[n++] = (char)b;
		}
		if (b >= 0x40 && b <= 0x7E) {
			break; // the final byte
		}
	}
	body[n] = '\0';

	static const struct {
		char intro;
		const char* body;
		qdos_key key;
	} MAP[] = {
			{'[', "A", QDOS_KEY_UP},
			{'[', "B", QDOS_KEY_DOWN},
			{'[', "C", QDOS_KEY_RIGHT},
			{'[', "D", QDOS_KEY_LEFT},
			{'O', "A", QDOS_KEY_UP},
			{'O', "B", QDOS_KEY_DOWN},
			{'O', "C", QDOS_KEY_RIGHT},
			{'O', "D", QDOS_KEY_LEFT},
			{'O', "P", QDOS_KEY_SOFT1},
			{'O', "Q", QDOS_KEY_SOFT2},
			{'O', "R", QDOS_KEY_SOFT3},
			{'O', "S", QDOS_KEY_SOFT4},
			{'[', "11~", QDOS_KEY_SOFT1},
			{'[', "12~", QDOS_KEY_SOFT2},
			{'[', "13~", QDOS_KEY_SOFT3},
			{'[', "14~", QDOS_KEY_SOFT4},
			{'[', "15~", QDOS_KEY_SOFT5},
			{'[', "21~", QDOS_KEY_POWER},
			{'[', "3~", QDOS_KEY_BACKSPACE},
	};

	for (size_t i = 0; i < sizeof(MAP) / sizeof(MAP[0]); i++) {
		if (MAP[i].intro == intro && strcmp(MAP[i].body, body) == 0) {
			send_key(s, MAP[i].key);
			return;
		}
	}
}

/* ---- uploads ------------------------------------------------------------- */

static void reply(qdos_serial* s, const char* fmt, ...) {
	char line[CMD_MAX + 8];
	int n = snprintf(line, sizeof(line), "QDUP ");
	va_list ap;
	va_start(ap, fmt);
	n += vsnprintf(line + n, sizeof(line) - (size_t)n - 1, fmt, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 2) {
		n = (int)sizeof(line) - 2;
	}
	line[n++] = '\n';
	s->write(s->user, line, (size_t)n);
}

/** @brief One path segment: no dots alone, nothing that leaves the inbox */
static bool good_segment(const char* p, size_t len) {
	if (len == 0 || len > 64 || (p[0] == '.')) {
		return false;
	}
	for (size_t i = 0; i < len; i++) {
		const char c = p[i];
		const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
						c == '_' || c == '-';
		if (!ok) {
			return false;
		}
	}
	return true;
}

/** @brief "name" or "app/name", and nothing deeper, as the stores are laid out */
static bool good_path(const char* path) {
	const char* slash = strchr(path, '/');
	if (slash == NULL) {
		return good_segment(path, strlen(path));
	}
	return strchr(slash + 1, '/') == NULL && good_segment(path, (size_t)(slash - path)) &&
		   good_segment(slash + 1, strlen(slash + 1));
}

static bool full_path(const qdos_serial* s, const char* path, char* buf, size_t cap) {
	const int n = snprintf(buf, cap, "%s/%s", s->inbox, path);
	return n > 0 && (size_t)n < cap;
}

static void put(qdos_serial* s, char* args) {
	char* end = NULL;
	const unsigned long size = strtoul(args, &end, 10);
	if (end == args || *end != ' ' || size > SIZE_MAX_BYTES) {
		reply(s, "ERR SIZE");
		return;
	}
	args = end + 1;
	const unsigned long want = strtoul(args, &end, 16);
	if (end == args || *end != ' ') {
		reply(s, "ERR CRC");
		return;
	}
	const char* path = end + 1;

	char dest[PATH_CAP];
	char part[PATH_CAP + 8];
	if (!good_path(path) || !full_path(s, path, dest, sizeof(dest)) ||
			snprintf(part, sizeof(part), "%s.part", dest) >= (int)sizeof(part)) {
		reply(s, "ERR PATH");
		return;
	}

	// An app's folder is made on its first file
	const char* slash = strchr(path, '/');
	if (slash != NULL) {
		char dir[PATH_CAP];
		snprintf(dir, sizeof(dir), "%s/%.*s", s->inbox, (int)(slash - path), path);
		if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
			reply(s, "ERR MKDIR");
			return;
		}
	}

	FILE* f = fopen(part, "wb");
	if (f == NULL) {
		reply(s, "ERR OPEN");
		return;
	}

	// The host waits for this before sending, so nothing it sends is taken for
	// a key while the file is being opened
	reply(s, "READY");

	uint8_t chunk[512];
	uint32_t crc = 0;
	unsigned long left = size;
	bool ok = true;
	while (left > 0) {
		const size_t want_now = left < sizeof(chunk) ? (size_t)left : sizeof(chunk);
		int got = s->read(s->user, chunk, want_now, DATA_MS);
		if (got <= 0) {
			ok = false;
			break;
		}
		crc = qdos_serial_crc32(crc, chunk, (size_t)got);
		if (fwrite(chunk, 1, (size_t)got, f) != (size_t)got) {
			ok = false;
			// Read the rest anyway, or it would arrive as keys
			left -= (unsigned long)got;
			while (left > 0 &&
					(got = s->read(s->user, chunk, left < sizeof(chunk) ? (size_t)left : sizeof(chunk), DATA_MS)) > 0) {
				left -= (unsigned long)got;
			}
			break;
		}
		left -= (unsigned long)got;
	}
	ok = (fclose(f) == 0) && ok;

	if (!ok || left > 0) {
		unlink(part);
		reply(s, left > 0 ? "ERR SHORT" : "ERR WRITE");
		return;
	}
	if (crc != (uint32_t)want) {
		unlink(part);
		reply(s, "ERR CRC %08lx", (unsigned long)crc);
		return;
	}

	// FAT will not rename over a file
	unlink(dest);
	if (rename(part, dest) != 0) {
		unlink(part);
		reply(s, "ERR RENAME");
		return;
	}

	s->touched(s->user);
	reply(s, "OK");
}

static void del(qdos_serial* s, const char* path) {
	char full[PATH_CAP];
	if (!good_path(path) || !full_path(s, path, full, sizeof(full))) {
		reply(s, "ERR PATH");
		return;
	}

	struct stat st;
	if (stat(full, &st) != 0) {
		reply(s, "ERR MISSING");
		return;
	}

	if (S_ISDIR(st.st_mode)) {
		DIR* dir = opendir(full);
		if (dir != NULL) {
			struct dirent* ent;
			while ((ent = readdir(dir)) != NULL) {
				char inner[PATH_CAP + 72];
				if (ent->d_name[0] != '.' &&
						snprintf(inner, sizeof(inner), "%s/%s", full, ent->d_name) < (int)sizeof(inner)) {
					unlink(inner);
				}
			}
			closedir(dir);
		}
		if (rmdir(full) != 0) {
			reply(s, "ERR RMDIR");
			return;
		}
	} else if (unlink(full) != 0) {
		reply(s, "ERR UNLINK");
		return;
	}

	s->touched(s->user);
	reply(s, "OK");
}

/** @brief Every file under @p rel, which is the inbox itself or one app's folder */
static void list_dir(qdos_serial* s, const char* rel) {
	char full[PATH_CAP];
	snprintf(full, sizeof(full), "%s%s%s", s->inbox, rel[0] ? "/" : "", rel);
	DIR* dir = opendir(full);
	if (dir == NULL) {
		return;
	}

	struct dirent* ent;
	while ((ent = readdir(dir)) != NULL) {
		if (ent->d_name[0] == '.') {
			continue;
		}

		char path[PATH_CAP];
		char abs[PATH_CAP * 2];
		const int n = snprintf(path, sizeof(path), "%s%s%s", rel, rel[0] ? "/" : "", ent->d_name);
		if (n < 0 || (size_t)n >= sizeof(path) ||
				snprintf(abs, sizeof(abs), "%s/%s", s->inbox, path) >= (int)sizeof(abs)) {
			continue;
		}

		struct stat st;
		if (stat(abs, &st) != 0) {
			continue;
		}
		if (S_ISDIR(st.st_mode)) {
			if (rel[0] == '\0') {
				list_dir(s, path);
			}
		} else {
			reply(s, "F %ld %s", (long)st.st_size, path);
		}
	}
	closedir(dir);
}

/** @brief Serve one command, its Ctrl-B having been read */
static void command(qdos_serial* s) {
	char line[CMD_MAX];
	size_t n = 0;
	for (;;) {
		const int b = read_byte(s, LINE_MS);
		if (b < 0) {
			return; // a Ctrl-B typed by hand, and nothing after it
		}
		if (b == '\n' || b == '\r') {
			break;
		}
		if (n + 1 >= sizeof(line)) {
			reply(s, "ERR LONG");
			return;
		}
		line[n++] = (char)b;
	}
	line[n] = '\0';

	if (strncmp(line, "PUT ", 4) == 0) {
		put(s, line + 4);
	} else if (strncmp(line, "DEL ", 4) == 0) {
		del(s, line + 4);
	} else if (strcmp(line, "LS") == 0) {
		list_dir(s, "");
		reply(s, "OK");
	} else if (strcmp(line, "HELLO") == 0) {
		reply(s, "OK");
	} else {
		reply(s, "ERR COMMAND");
	}
}

/* ---- the loop ------------------------------------------------------------ */

bool qdos_serial_step(qdos_serial* s) {
	const int b = read_byte(s, -1);
	if (b == -2) {
		return false;
	}
	if (b < 0) {
		return true;
	}

	// A terminal's Enter is CR, CR LF or LF: one press whichever it is
	const bool was_cr = s->after_cr;
	s->after_cr = (b == '\r');
	if (b == '\n' && was_cr) {
		return true;
	}

	switch (b) {
	case '\r':
	case '\n':
		send_key(s, QDOS_KEY_ENTER);
		break;
	case 0x7F:
	case 0x08:
		send_key(s, QDOS_KEY_BACKSPACE);
		break;
	case '\t':
		send_key(s, QDOS_KEY_TAB);
		break;
	case STX:
		command(s);
		break;
	case ESC: {
		const int next = read_byte(s, SEQUENCE_MS);
		if (next == '[' || next == 'O') {
			sequence(s, (char)next);
		} else {
			send_key(s, QDOS_KEY_CLEAR);
		}
		break;
	}
	default:
		if (b >= 0x20 && b < 0x7F) {
			qdos_key_event ev = {.key = QDOS_KEY_NONE, .ch = 0};
			qdos_pad_typed((char)b, &ev);
			s->key(s->user, &ev);
		}
		break;
	}
	return true;
}
