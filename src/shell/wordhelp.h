/**
 * @file wordhelp.h
 * @brief One line on what a word does, for the catalog
 */

#ifndef QDOS_WORDHELP_H
#define QDOS_WORDHELP_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief What @p name takes and leaves, and what it is for
 *
 * Written as a stack effect and a few words, `x -- x!: factorial`, short
 * enough for one row of the small font.
 *
 * @return NULL for a word this has nothing on, a program's own among them
 */
const char* qdos_word_help(const char* name);

#ifdef __cplusplus
}
#endif

#endif // QDOS_WORDHELP_H
