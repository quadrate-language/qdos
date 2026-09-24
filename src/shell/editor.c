/**
 * @file editor.c
 * @brief Multi-line text buffer for editing a program
 */

#include "editor.h"

#include <stdio.h>
#include <string.h>

static size_t line_start(const qdos_editor* ed, size_t offset) {
	while (offset > 0 && ed->text[offset - 1] != '\n') {
		offset--;
	}
	return offset;
}

static size_t line_end(const qdos_editor* ed, size_t offset) {
	while (offset < ed->len && ed->text[offset] != '\n') {
		offset++;
	}
	return offset;
}

enum {
	OP_NONE = 0,
	OP_INSERT,
	OP_DELETE
};

static void checkpoint(qdos_editor* ed, int op) {
	if (op != ed->last_op) {
		memcpy(ed->undo_text, ed->text, ed->len + 1);
		ed->undo_len = ed->len;
		ed->undo_cursor = ed->cursor;
		ed->can_undo = true;
	}
	ed->last_op = op;
}

static void put(qdos_editor* ed, char ch) {
	if (ed->len + 1 >= QDOS_PROGRAM_MAX) {
		return;
	}

	memmove(ed->text + ed->cursor + 1, ed->text + ed->cursor, ed->len - ed->cursor);
	ed->text[ed->cursor] = ch;
	ed->cursor++;
	ed->len++;
	ed->text[ed->len] = '\0';
	ed->dirty = true;
}

static void delete_back(qdos_editor* ed) {
	memmove(ed->text + ed->cursor - 1, ed->text + ed->cursor, ed->len - ed->cursor);
	ed->cursor--;
	ed->len--;
	ed->text[ed->len] = '\0';
	ed->dirty = true;
}

static bool blank(char ch) {
	return ch == ' ' || ch == '\t';
}

void qdos_editor_open(qdos_editor* ed, const char* name, const char* source) {
	memset(ed, 0, sizeof(*ed));
	snprintf(ed->name, sizeof(ed->name), "%s", name);

	if (source == NULL) {
		snprintf(ed->text, sizeof(ed->text), "fn %s( -- ) {\n\t\n}", name);
		ed->len = strlen(ed->text);
		// Land on the blank body line, where the work starts
		ed->cursor = ed->len - 2;
		return;
	}

	snprintf(ed->text, sizeof(ed->text), "%s", source);
	ed->len = strlen(ed->text);
}

void qdos_editor_insert(qdos_editor* ed, char ch) {
	checkpoint(ed, OP_INSERT);

	if (ch == '}' && ed->cursor > 0 && ed->text[ed->cursor - 1] == '\t') {
		bool only_blank = true;
		for (size_t i = line_start(ed, ed->cursor); i < ed->cursor; i++) {
			only_blank = only_blank && blank(ed->text[i]);
		}
		if (only_blank) {
			delete_back(ed);
		}
	}
	put(ed, ch);
}

void qdos_editor_backspace(qdos_editor* ed) {
	if (ed->cursor == 0) {
		return;
	}
	checkpoint(ed, OP_DELETE);
	delete_back(ed);
}

void qdos_editor_newline(qdos_editor* ed) {
	const size_t start = line_start(ed, ed->cursor);
	size_t indent = 0;
	while (start + indent < ed->cursor && blank(ed->text[start + indent])) {
		indent++;
	}

	size_t back = ed->cursor;
	while (back > start && blank(ed->text[back - 1])) {
		back--;
	}
	const bool opens = back > start && ed->text[back - 1] == '{';

	// A line is a step of its own
	ed->last_op = OP_NONE;
	checkpoint(ed, OP_INSERT);
	put(ed, '\n');
	for (size_t i = 0; i < indent; i++) {
		put(ed, ed->text[start + i]);
	}
	if (opens) {
		put(ed, '\t');
	}
}

bool qdos_editor_undo(qdos_editor* ed) {
	if (!ed->can_undo) {
		return false;
	}

	char text[QDOS_PROGRAM_MAX];
	memcpy(text, ed->text, ed->len + 1);
	const size_t len = ed->len;
	const size_t cursor = ed->cursor;

	memcpy(ed->text, ed->undo_text, ed->undo_len + 1);
	ed->len = ed->undo_len;
	ed->cursor = ed->undo_cursor;

	memcpy(ed->undo_text, text, len + 1);
	ed->undo_len = len;
	ed->undo_cursor = cursor;

	ed->last_op = OP_NONE;
	ed->dirty = true;
	return true;
}

void qdos_editor_goto(qdos_editor* ed, size_t line) {
	size_t at = 0;
	for (size_t n = 0; n < line && at < ed->len; n++) {
		at = line_end(ed, at);
		if (at < ed->len) {
			at++;
		}
	}
	while (at < ed->len && blank(ed->text[at])) {
		at++;
	}
	ed->cursor = at;
	ed->last_op = OP_NONE;
}

void qdos_editor_move(qdos_editor* ed, int dx, int dy) {
	ed->last_op = OP_NONE;

	if (dx < 0 && ed->cursor > 0) {
		ed->cursor--;
	} else if (dx > 0 && ed->cursor < ed->len) {
		ed->cursor++;
	}

	if (dy == 0) {
		return;
	}

	const size_t start = line_start(ed, ed->cursor);
	const size_t col = ed->cursor - start;

	if (dy < 0) {
		if (start == 0) {
			return;
		}
		const size_t prev = line_start(ed, start - 1);
		const size_t prev_len = (start - 1) - prev;
		ed->cursor = prev + (col < prev_len ? col : prev_len);
	} else {
		const size_t end = line_end(ed, ed->cursor);
		if (end >= ed->len) {
			return;
		}
		const size_t next = end + 1;
		const size_t next_len = line_end(ed, next) - next;
		ed->cursor = next + (col < next_len ? col : next_len);
	}
}

size_t qdos_editor_lines(const qdos_editor* ed) {
	size_t n = 1;
	for (size_t i = 0; i < ed->len; i++) {
		if (ed->text[i] == '\n') {
			n++;
		}
	}
	return n;
}

const char* qdos_editor_line(const qdos_editor* ed, size_t index, size_t* len) {
	size_t at = 0;
	for (size_t n = 0; n < index; n++) {
		at = line_end(ed, at);
		if (at >= ed->len) {
			return NULL;
		}
		at++;
	}

	*len = line_end(ed, at) - at;
	return ed->text + at;
}

void qdos_editor_where(const qdos_editor* ed, size_t* line, size_t* col) {
	size_t n = 0;
	for (size_t i = 0; i < ed->cursor; i++) {
		if (ed->text[i] == '\n') {
			n++;
		}
	}

	*line = n;
	*col = ed->cursor - line_start(ed, ed->cursor);
}
