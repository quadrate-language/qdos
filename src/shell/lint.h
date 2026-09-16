/**
 * @file lint.h
 * @brief What parses now but would only fail once it runs
 */

#ifndef QDOS_LINT_H
#define QDOS_LINT_H

#include <quadrate/interp/interp.h>

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Look for what a declaration leaves until it is called
 *
 * Declaring a word only parses it. The interpreter looks the names in a body
 * up when the body runs, so a typo sits there until someone calls it -- which
 * is why a program can pass a check and still say a word is not defined.
 *
 * This walks the same nodes the interpreter walks and stops at the first one
 * it would refuse, judged against the vocabulary @p interp holds right now.
 * Declare the program first and the word can call itself.
 *
 * @param out Receives a message written for the message line
 * @return true when something was found
 */
bool qdos_lint_program(qd_interp* interp, const char* source, char* out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif // QDOS_LINT_H
