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
} qdos_editor;

/** @brief Start editing. A NULL source opens a template for a new program. */
void qdos_editor_open(qdos_editor* ed, const char* name, const char* source);

void qdos_editor_insert(qdos_editor* ed, char ch);
void qdos_editor_backspace(qdos_editor* ed);

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
