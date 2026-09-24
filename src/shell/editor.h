/**
 * @file editor.h
 * @brief Multi-line text buffer for editing a program
 */

#ifndef QDOS_EDITOR_H
#define QDOS_EDITOR_H

#include "storage.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	char text[QDOS_PROGRAM_MAX];
	size_t len;
	size_t cursor;
	char name[QDOS_PROGRAM_NAME_MAX];
	bool dirty; ///< Edited since it was opened

	/* One step back, taken before each run of typing or of deleting */
	char undo_text[QDOS_PROGRAM_MAX];
	size_t undo_len;
	size_t undo_cursor;
	bool can_undo;
	int last_op;
} qdos_editor;

/** @brief Start editing. A NULL source opens a template for a new program. */
void qdos_editor_open(qdos_editor* ed, const char* name, const char* source);

/** @brief A `}` typed on a blank line takes one level of indent back */
void qdos_editor_insert(qdos_editor* ed, char ch);
void qdos_editor_backspace(qdos_editor* ed);

/** @brief A new line at the indent of this one, a level deeper after `{` */
void qdos_editor_newline(qdos_editor* ed);

/** @brief Swap back to before the last run of edits; again, and it is redone */
bool qdos_editor_undo(qdos_editor* ed);

/** @brief The cursor to the first non-blank of a line, counted from 0 */
void qdos_editor_goto(qdos_editor* ed, size_t line);

/** @brief Move by dx characters then dy lines, keeping the column where it fits */
void qdos_editor_move(qdos_editor* ed, int dx, int dy);

size_t qdos_editor_lines(const qdos_editor* ed);

/**
 * @brief One line, without its newline
 * @param[out] len Length of the returned run
 * @return Pointer into the buffer, or NULL past the end
 */
const char* qdos_editor_line(const qdos_editor* ed, size_t index, size_t* len);

/** @brief Where the cursor sits, as a line and a column */
void qdos_editor_where(const qdos_editor* ed, size_t* line, size_t* col);

#ifdef __cplusplus
}
#endif

#endif // QDOS_EDITOR_H
