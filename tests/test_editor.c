/**
 * @file test_editor.c
 * @brief The multi-line program editor buffer
 */

#include "check.h"

#include "../src/shell/editor.h"

#include <string.h>

static void expect_line(const qdos_editor* ed, size_t index, const char* want) {
	size_t len = 0;
	const char* got = qdos_editor_line(ed, index, &len);
	CHECK(got != NULL);
	CHECK(len == strlen(want));
	CHECK(strncmp(got, want, len) == 0);
}

static void test_open_existing(void) {
	qdos_editor ed;
	qdos_editor_open(&ed, "hyp", "fn hyp() {\n\t5\n}");

	CHECK(strcmp(ed.name, "hyp") == 0);
	CHECK(qdos_editor_lines(&ed) == 3);
	expect_line(&ed, 0, "fn hyp() {");
	expect_line(&ed, 1, "\t5");
	expect_line(&ed, 2, "}");
	CHECK(qdos_editor_line(&ed, 3, &(size_t){0}) == NULL);
}

static void test_open_new_has_a_template(void) {
	qdos_editor ed;
	qdos_editor_open(&ed, "fresh", NULL);

	CHECK(strstr(ed.text, "fn fresh") != NULL);
	CHECK(qdos_editor_lines(&ed) == 3);

	// The cursor starts on the body line, not at the end
	size_t line, col;
	qdos_editor_where(&ed, &line, &col);
	CHECK(line == 1);
}

static void test_insert_and_backspace(void) {
	qdos_editor ed;
	qdos_editor_open(&ed, "x", "ac");

	qdos_editor_move(&ed, 1, 0); // after 'a'
	qdos_editor_insert(&ed, 'b');
	CHECK(strcmp(ed.text, "abc") == 0);

	qdos_editor_backspace(&ed);
	CHECK(strcmp(ed.text, "ac") == 0);

	// Backspace at the very start does nothing
	while (ed.cursor > 0)
		qdos_editor_move(&ed, -1, 0);
	qdos_editor_backspace(&ed);
	CHECK(strcmp(ed.text, "ac") == 0);
}

static void test_newline_splits(void) {
	qdos_editor ed;
	qdos_editor_open(&ed, "x", "ab");

	qdos_editor_move(&ed, 1, 0);
	qdos_editor_insert(&ed, '\n');
	CHECK(qdos_editor_lines(&ed) == 2);
	expect_line(&ed, 0, "a");
	expect_line(&ed, 1, "b");
}

static void test_vertical_movement_keeps_column(void) {
	qdos_editor ed;
	qdos_editor_open(&ed, "x", "abcdef\nghijkl\nmn");

	for (int i = 0; i < 4; i++)
		qdos_editor_move(&ed, 1, 0); // column 4 on line 0

	qdos_editor_move(&ed, 0, 1);
	size_t line, col;
	qdos_editor_where(&ed, &line, &col);
	CHECK(line == 1 && col == 4);

	// A shorter line clamps to its end, and does not overshoot
	qdos_editor_move(&ed, 0, 1);
	qdos_editor_where(&ed, &line, &col);
	CHECK(line == 2 && col == 2);

	qdos_editor_move(&ed, 0, -1);
	qdos_editor_where(&ed, &line, &col);
	CHECK(line == 1);
}

static void test_movement_stops_at_the_ends(void) {
	qdos_editor ed;
	qdos_editor_open(&ed, "x", "one\ntwo");

	for (int i = 0; i < 20; i++)
		qdos_editor_move(&ed, 0, -1);
	size_t line, col;
	qdos_editor_where(&ed, &line, &col);
	CHECK(line == 0);

	for (int i = 0; i < 20; i++)
		qdos_editor_move(&ed, 0, 1);
	qdos_editor_where(&ed, &line, &col);
	CHECK(line == 1);

	for (int i = 0; i < 40; i++)
		qdos_editor_move(&ed, 1, 0);
	CHECK(ed.cursor == ed.len);
}

static void test_full_buffer_is_refused(void) {
	qdos_editor ed;
	qdos_editor_open(&ed, "x", "");

	for (size_t i = 0; i < QDOS_PROGRAM_MAX + 100; i++)
		qdos_editor_insert(&ed, 'z');

	CHECK(ed.len < QDOS_PROGRAM_MAX);
	CHECK(ed.text[ed.len] == '\0');
}

/** Opening is not editing, so a buffer nobody touched has nothing to lose. */
static void test_dirty_follows_the_edits(void) {
	qdos_editor ed;
	qdos_editor_open(&ed, "x", "fn x( -- ) { }");
	CHECK(!ed.dirty);

	qdos_editor_move(&ed, 1, 0);
	CHECK(!ed.dirty); // looking about is not editing either

	qdos_editor_insert(&ed, 'z');
	CHECK(ed.dirty);

	// A template is the editor's own writing, not the user's
	qdos_editor_open(&ed, "fresh", NULL);
	CHECK(!ed.dirty);

	qdos_editor_backspace(&ed);
	CHECK(ed.dirty);
}

int main(void) {
	test_open_existing();
	test_open_new_has_a_template();
	test_insert_and_backspace();
	test_newline_splits();
	test_vertical_movement_keeps_column();
	test_movement_stops_at_the_ends();
	test_full_buffer_is_refused();
	test_dirty_follows_the_edits();
	return check_report("editor");
}
