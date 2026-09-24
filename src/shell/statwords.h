/**
 * @file statwords.h
 * @brief Statistics and probability, registered as shell words
 */

#ifndef QDOS_STATWORDS_H
#define QDOS_STATWORDS_H

#include <quadrate/interp/interp.h>
#include <quadrate/rt/context.h>

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief sum, mean, the regressions, ncr, the distributions, rand and the rest */
void qdos_register_stats(qd_interp* interp);

/**
 * @brief Take an array of numbers off the top of the stack
 * @param out Receives a malloc'd copy, which the caller frees; NULL when empty
 * @return false, having raised an error for @p word, when the top is not one
 */
bool qdos_pop_numbers(qd_context* ctx, const char* word, double** out, size_t* n);

/** @brief Push @p n doubles as a new []f64 */
int qdos_push_numbers(qd_context* ctx, const double* values, size_t n);

#ifdef __cplusplus
}
#endif

#endif // QDOS_STATWORDS_H
