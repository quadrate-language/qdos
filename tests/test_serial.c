/**
 * @file test_serial.c
 * @brief The serial console: what the device and the simulator do with a terminal's bytes
 *
 * Fed from memory rather than a port, so the protocol qdosctl speaks is
 * checked here without either.
 */

#define _POSIX_C_SOURCE 200809L

#include "check.h"

#include "hal/serial_console.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef INBOX_DIR
#error "INBOX_DIR must be defined by the build"
#endif

/* ---- an in-memory port ---------------------------------------------------- */

static const uint8_t* g_in;
static size_t g_in_len;
static size_t g_in_pos;

static char g_out[4096];
static size_t g_out_len;

static qdos_key_event g_keys[32];
static size_t g_key_count;
static int g_touched;

/** Runs dry as a port that has gone, so the loop below ends */
static int mem_read(void* user, uint8_t* buf, size_t cap, int timeout_ms) {
	(void)user;
	(void)timeout_ms;
	if (g_in_pos >= g_in_len) {
		return -1;
	}
	const size_t left = g_in_len - g_in_pos;
	const size_t n = left < cap ? left : cap;
	memcpy(buf, g_in + g_in_pos, n);
	g_in_pos += n;
	return (int)n;
}

static void mem_write(void* user, const char* text, size_t len) {
	(void)user;
	if (g_out_len + len < sizeof(g_out)) {
		memcpy(g_out + g_out_len, text, len);
		g_out_len += len;
		g_out[g_out_len] = '\0';
	}
}

static void mem_key(void* user, const qdos_key_event* ev) {
	(void)user;
	if (g_key_count < sizeof(g_keys) / sizeof(g_keys[0])) {
		g_keys[g_key_count++] = *ev;
	}
}

static void mem_touched(void* user) {
	(void)user;
	g_touched++;
}

/** Feed @p len bytes through the console until they run out */
static void feed(const void* bytes, size_t len) {
	g_in = (const uint8_t*)bytes;
	g_in_len = len;
	g_in_pos = 0;
	g_out_len = 0;
	g_out[0] = '\0';
	g_key_count = 0;
	g_touched = 0;

	qdos_serial s = {
			.read = mem_read,
			.write = mem_write,
			.key = mem_key,
			.touched = mem_touched,
			.inbox = INBOX_DIR,
	};
	while (qdos_serial_step(&s)) {
	}
}

static void feed_str(const char* text) {
	feed(text, strlen(text));
}

/** A PUT of @p body as @p path, with its checksum, or a wrong one */
static void put(const char* path, const char* body, bool good_crc) {
	static char frame[1024];
	const uint32_t crc = qdos_serial_crc32(0, (const uint8_t*)body, strlen(body));
	const int n = snprintf(frame, sizeof(frame), "\x02PUT %zu %08x %s\n%s", strlen(body),
			(unsigned)(good_crc ? crc : crc ^ 1u), path, body);
	feed(frame, (size_t)n);
}

static bool exists(const char* rel) {
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", INBOX_DIR, rel);
	struct stat st;
	return stat(path, &st) == 0;
}

static void wipe(const char* rel) {
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", INBOX_DIR, rel);
	unlink(path);
	rmdir(path);
}

/* ---- keys ----------------------------------------------------------------- */

/** CR, LF and CR LF are each one Enter, as terminals send all three. */
static void test_every_enter_is_one(void) {
	feed_str("\r\n\n\r");
	CHECK(g_key_count == 3);
	for (size_t i = 0; i < g_key_count; i++) {
		CHECK(g_keys[i].key == QDOS_KEY_ENTER);
	}
}

/** The escape sequences qdosctl's `key` sends are the keys it names. */
static void test_sequences_are_keys(void) {
	feed_str("\x1b[A\x1bOP\x1b[15~\x1b[21~\x1b[3~\t\x7f");
	const qdos_key want[] = {QDOS_KEY_UP, QDOS_KEY_SOFT1, QDOS_KEY_SOFT5, QDOS_KEY_POWER, QDOS_KEY_BACKSPACE,
			QDOS_KEY_TAB, QDOS_KEY_BACKSPACE};
	CHECK(g_key_count == sizeof(want) / sizeof(want[0]));
	for (size_t i = 0; i < g_key_count && i < sizeof(want) / sizeof(want[0]); i++) {
		CHECK(g_keys[i].key == want[i]);
	}
}

/** An Escape with nothing after it is ESC. */
static void test_a_lone_escape_is_esc(void) {
	feed_str("\x1b");
	CHECK(g_key_count == 1 && g_keys[0].key == QDOS_KEY_CLEAR);
}

/** Printable characters go through the keyboard's own mapping: a digit is its key. */
static void test_typing_is_characters(void) {
	feed_str("7x");
	CHECK(g_key_count == 2);
	CHECK(g_keys[0].key == QDOS_KEY_7);
	CHECK(g_keys[1].key == QDOS_KEY_CHAR && g_keys[1].ch == 'x');
}

/* ---- uploads -------------------------------------------------------------- */

static void test_hello_answers(void) {
	feed_str("\x02HELLO\n");
	CHECK_STR(g_out, "QDUP OK\n");
	CHECK(g_key_count == 0);
}

/** A file lands whole, and the shell is told. */
static void test_a_put_lands(void) {
	wipe("hello.qd");
	put("hello.qd", "fn hello( -- r:i64) { 1 }\n", true);
	CHECK_STR(g_out, "QDUP READY\nQDUP OK\n");
	CHECK(exists("hello.qd"));
	CHECK(!exists("hello.qd.part"));
	CHECK(g_touched == 1);
	CHECK(g_key_count == 0); // none of the file arrived as keys
	wipe("hello.qd");
}

/** A checksum that does not hold leaves nothing behind. */
static void test_a_bad_checksum_leaves_nothing(void) {
	wipe("bad.qd");
	put("bad.qd", "1 2 3\n", false);
	CHECK(strstr(g_out, "QDUP ERR CRC") != NULL);
	CHECK(!exists("bad.qd"));
	CHECK(!exists("bad.qd.part"));
	CHECK(g_touched == 0);
}

/** An app's folder is made on its first file, and LS lists what is inside. */
static void test_an_app_folder_and_its_listing(void) {
	wipe("game/main.qd");
	wipe("game");
	put("game/main.qd", "fn main( -- ) { }\n", true);
	CHECK(exists("game/main.qd"));

	feed_str("\x02LS\n");
	CHECK(strstr(g_out, "QDUP F 18 game/main.qd\n") != NULL);
	CHECK(strstr(g_out, "QDUP OK\n") != NULL);

	feed_str("\x02"
			 "DEL game\n");
	CHECK_STR(g_out, "QDUP OK\n");
	CHECK(!exists("game"));
	CHECK(g_touched == 1);
}

/** Nothing outside the inbox, and nothing deeper than an app's folder. */
static void test_paths_stay_in_the_inbox(void) {
	put("../escape.qd", "1\n", true);
	CHECK(strstr(g_out, "QDUP ERR PATH") != NULL);
	put("a/b/c.qd", "1\n", true);
	CHECK(strstr(g_out, "QDUP ERR PATH") != NULL);
	put(".hidden", "1\n", true);
	CHECK(strstr(g_out, "QDUP ERR PATH") != NULL);
	feed_str("\x02"
			 "DEL ..\n");
	CHECK(strstr(g_out, "QDUP ERR PATH") != NULL);
}

static void test_missing_and_unknown(void) {
	feed_str("\x02"
			 "DEL nothing.qd\n");
	CHECK_STR(g_out, "QDUP ERR MISSING\n");
	feed_str("\x02"
			 "FROB\n");
	CHECK_STR(g_out, "QDUP ERR COMMAND\n");
}

/** The checksum is zlib's, which is what qdosctl takes from gzip. */
static void test_the_checksum_is_zlibs(void) {
	CHECK(qdos_serial_crc32(0, (const uint8_t*)"123456789", 9) == 0xCBF43926u);
	const uint32_t half = qdos_serial_crc32(0, (const uint8_t*)"1234", 4);
	CHECK(qdos_serial_crc32(half, (const uint8_t*)"56789", 5) == 0xCBF43926u);
}

int main(void) {
	mkdir(INBOX_DIR, 0755);

	test_every_enter_is_one();
	test_sequences_are_keys();
	test_a_lone_escape_is_esc();
	test_typing_is_characters();
	test_hello_answers();
	test_a_put_lands();
	test_a_bad_checksum_leaves_nothing();
	test_an_app_folder_and_its_listing();
	test_paths_stay_in_the_inbox();
	test_missing_and_unknown();
	test_the_checksum_is_zlibs();
	return check_report("serial");
}
