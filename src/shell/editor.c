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

void qdos_editor_backspace(qdos_editor* ed) {
	if (ed->cursor == 0) {
		return;
	}

	memmove(ed->text + ed->cursor - 1, ed->text + ed->cursor, ed->len - ed->cursor);
	ed->cursor--;
	ed->len--;
	ed->text[ed->len] = '\0';
	ed->dirty = true;
}

void qdos_editor_move(qdos_editor* ed, int dx, int dy) {
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
