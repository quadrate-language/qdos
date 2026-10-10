/**
 * @file upload.c
 * @brief Files onto the inbox, over the serial console
 *
 * The USB port is the terminal, so an upload is framed to be told apart from
 * typing: a Ctrl-B, then one line naming the command, then for PUT the file's
 * bytes. Every answer is one line starting "QDUP ", which the host picks out
 * from among the log. tools/qdosctl is the other end.
 *
 *   PUT <size> <crc32, hex> <path>   then <size> bytes
 *   DEL <path>                       a file, or an app folder and what is in it
 *   LS                               "QDUP F <size> <path>" per file, then OK
 *
 * A path is a program or an app's file, as the stores hold them: "hello.qd" or
 * "doom/main.qd". A file is written beside itself and renamed into place once
 * its checksum holds, so the shell never reads half of one.
 */

#include "upload.h"

#include "hal_esp32.h"

#include "esp_rom_crc.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CMD_MAX 160
#define PATH_MAX_LEN 96
#define LINE_MS 1000 ///< Between the bytes of the command line
#define DATA_MS 3000 ///< Between the chunks of a file
#define SIZE_MAX_BYTES (8u * 1024u * 1024u)

static qdos_upload_read g_read;

static void reply(const char* fmt, ...) {
	char line[CMD_MAX];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	printf("QDUP %s\n", line);
	fflush(stdout);
}

/** @brief One path segment: no dots alone, nothing that leaves the inbox */
static bool good_segment(const char* s, size_t len) {
	if (len == 0 || len > 64 || (s[0] == '.')) {
		return false;
	}
	for (size_t i = 0; i < len; i++) {
		const char c = s[i];
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

static bool full_path(const char* path, char* buf, size_t cap) {
	const int n = snprintf(buf, cap, "%s/%s", QDOS_ESP32_INBOX, path);
	return n > 0 && (size_t)n < cap;
}

static void put(char* args) {
	char* end = NULL;
	const unsigned long size = strtoul(args, &end, 10);
	if (end == args || *end != ' ' || size > SIZE_MAX_BYTES) {
		reply("ERR SIZE");
		return;
	}
	args = end + 1;
	const unsigned long want = strtoul(args, &end, 16);
	if (end == args || *end != ' ') {
		reply("ERR CRC");
		return;
	}
	const char* path = end + 1;

	char dest[PATH_MAX_LEN + 32];
	char part[PATH_MAX_LEN + 40];
	if (!good_path(path) || !full_path(path, dest, sizeof(dest)) ||
			snprintf(part, sizeof(part), "%s.part", dest) >= (int)sizeof(part)) {
		reply("ERR PATH");
		return;
	}

	// An app's folder is made on its first file
	const char* slash = strchr(path, '/');
	if (slash != NULL) {
		char dir[PATH_MAX_LEN + 32];
		snprintf(dir, sizeof(dir), "%s/%.*s", QDOS_ESP32_INBOX, (int)(slash - path), path);
		if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
			reply("ERR MKDIR");
			return;
		}
	}

	FILE* f = fopen(part, "wb");
	if (f == NULL) {
		reply("ERR OPEN");
		return;
	}

	// The host waits for this before sending, so nothing it sends is taken for
	// a key while the file is being opened
	reply("READY");

	uint8_t chunk[512];
	uint32_t crc = 0;
	unsigned long left = size;
	bool ok = true;
	while (left > 0) {
		const size_t want_now = left < sizeof(chunk) ? (size_t)left : sizeof(chunk);
		int got = g_read(chunk, want_now, DATA_MS);
		if (got <= 0) {
			ok = false;
			break;
		}
		crc = esp_rom_crc32_le(crc, chunk, (uint32_t)got);
		if (fwrite(chunk, 1, (size_t)got, f) != (size_t)got) {
			ok = false;
			// Read the rest anyway, or it would arrive as keys
			left -= (unsigned long)got;
			while (left > 0 &&
					(got = g_read(chunk, left < sizeof(chunk) ? (size_t)left : sizeof(chunk), DATA_MS)) > 0) {
				left -= (unsigned long)got;
			}
			break;
		}
		left -= (unsigned long)got;
	}
	ok = (fclose(f) == 0) && ok;

	if (!ok || left > 0) {
		unlink(part);
		reply(left > 0 ? "ERR SHORT" : "ERR WRITE");
		return;
	}
	if (crc != (uint32_t)want) {
		unlink(part);
		reply("ERR CRC %08lx", (unsigned long)crc);
		return;
	}

	// FAT will not rename over a file
	unlink(dest);
	if (rename(part, dest) != 0) {
		unlink(part);
		reply("ERR RENAME");
		return;
	}

	qdos_esp32_store_touched();
	reply("OK");
}

static void del(const char* path) {
	char full[PATH_MAX_LEN + 32];
	if (!good_path(path) || !full_path(path, full, sizeof(full))) {
		reply("ERR PATH");
		return;
	}

	struct stat st;
	if (stat(full, &st) != 0) {
		reply("ERR MISSING");
		return;
	}

	if (S_ISDIR(st.st_mode)) {
		DIR* dir = opendir(full);
		if (dir != NULL) {
			struct dirent* ent;
			while ((ent = readdir(dir)) != NULL) {
				char inner[PATH_MAX_LEN + 100];
				if (snprintf(inner, sizeof(inner), "%s/%s", full, ent->d_name) < (int)sizeof(inner)) {
					unlink(inner);
				}
			}
			closedir(dir);
		}
		if (rmdir(full) != 0) {
			reply("ERR RMDIR");
			return;
		}
	} else if (unlink(full) != 0) {
		reply("ERR UNLINK");
		return;
	}

	qdos_esp32_store_touched();
	reply("OK");
}

static void list_dir(const char* rel) {
	char full[PATH_MAX_LEN + 32];
	snprintf(full, sizeof(full), "%s%s%s", QDOS_ESP32_INBOX, rel[0] ? "/" : "", rel);
	DIR* dir = opendir(full);
	if (dir == NULL) {
		return;
	}

	struct dirent* ent;
	while ((ent = readdir(dir)) != NULL) {
		char path[PATH_MAX_LEN + 70];
		char abs[PATH_MAX_LEN + 110];
		const int n = snprintf(path, sizeof(path), "%s%s%s", rel, rel[0] ? "/" : "", ent->d_name);
		if (n < 0 || (size_t)n >= sizeof(path) ||
				snprintf(abs, sizeof(abs), "%s/%s", QDOS_ESP32_INBOX, path) >= (int)sizeof(abs)) {
			continue;
		}

		struct stat st;
		if (stat(abs, &st) != 0) {
			continue;
		}
		if (S_ISDIR(st.st_mode)) {
			if (rel[0] == '\0') {
				list_dir(path);
			}
		} else {
			reply("F %ld %s", (long)st.st_size, path);
		}
	}
	closedir(dir);
}

void qdos_upload_run(qdos_upload_read read) {
	g_read = read;

	char line[CMD_MAX];
	size_t n = 0;
	for (;;) {
		uint8_t b;
		if (read(&b, 1, LINE_MS) != 1) {
			return; // a Ctrl-B typed by hand, and nothing after it
		}
		if (b == '\n' || b == '\r') {
			break;
		}
		if (n + 1 >= sizeof(line)) {
			reply("ERR LONG");
			return;
		}
		line[n++] = (char)b;
	}
	line[n] = '\0';

	if (strncmp(line, "PUT ", 4) == 0) {
		put(line + 4);
	} else if (strncmp(line, "DEL ", 4) == 0) {
		del(line + 4);
	} else if (strcmp(line, "LS") == 0) {
		list_dir("");
		reply("OK");
	} else if (strcmp(line, "HELLO") == 0) {
		reply("OK");
	} else {
		reply("ERR COMMAND");
	}
}
