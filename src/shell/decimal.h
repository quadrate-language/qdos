/**
 * @file decimal.h
 * @brief The calculator in decimal: f128 numbers, and words written for f64 kept to them
 *
 * With decimal on, a number typed with a point is an f128, so 0.1 0.2 + is 0.3, and a
 * whole number stays an i64, which widens to one where the two meet. The words written
 * for f64 -- statistics, complex numbers, the hyperbolic functions -- still compute in
 * binary, so a call to one goes through qdos_decimal_call(), which hands it f64 and
 * gives back what it leaves as decimals.
 */

#ifndef QDOS_DECIMAL_H
#define QDOS_DECIMAL_H

#include <quadrate/interp/interp.h>
#include <quadrate/rt/context.h>
#include <quadrate/rt/f128.h>

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void qdos_decimal_set(bool on);
bool qdos_decimal(void);

/**
 * @brief A double as the decimal it was meant to be: its shortest spelling that reads
 *        back to it, so 0.1 is 0.1 and not 0.1000000000000000055511151231257827
 */
qd_f128_t qdos_decimal_of_double(double v);

/** @brief The number @p depth below the top as an f128: an f128, an i64 or an f64 */
bool qdos_decimal_peek(qd_context* ctx, size_t depth, qd_f128_t* out);

/**
 * @brief Run a word written for f64 on decimal arguments
 *
 * The top @p inputs values are handed over as f64 where they are f128, an array of f128
 * as an array of f64, and every f64 the word leaves comes back as an f128. With decimal
 * off this is a plain call.
 */
int qdos_decimal_call(qd_context* ctx, size_t inputs, qd_interp_native_fn fn, void* user);

/** @brief How many values a native signature takes: those before its `--` */
size_t qdos_signature_inputs(const char* signature);

/** @brief A word written for f64, registered to go through qdos_decimal_call() */
typedef struct {
	const char* name;
	const char* signature;
	qd_interp_native_fn fn;
} qdos_f64_word;

/** @brief Register @p words, each kept to decimals when decimal is on */
void qdos_register_f64_words(qd_interp* interp, const qdos_f64_word* words, size_t count);

#ifdef __cplusplus
}
#endif

#endif // QDOS_DECIMAL_H
