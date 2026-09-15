/**
 * @file mathwords.h
 * @brief Quadrate's math:: module, registered as shell words
 */

#ifndef QDOS_MATHWORDS_H
#define QDOS_MATHWORDS_H

#include <quadrate/interp/interp.h>
#include <quadrate/rt/context.h>

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Every word the keypad and the catalog offer beyond the core vocabulary */
void qdos_register_math(qd_interp* interp);

/**
 * @brief Read trigonometry arguments as degrees rather than radians
 *
 * lib/math works in radians only. A calculator is asked for sin 30 far more
 * often than sin 0.5236, so the shell converts on the way in and back out.
 */
void qdos_math_set_degrees(bool degrees);

bool qdos_math_degrees(void);

/** @brief Argument @p depth from the top as a double, ints included */
bool qdos_peek_number(qd_context* ctx, size_t depth, double* out);

/** @brief Raise "word: text" as an ordinary error */
int qdos_math_error(qd_context* ctx, const char* word, const char* text);

#ifdef __cplusplus
}
#endif

#endif // QDOS_MATHWORDS_H
