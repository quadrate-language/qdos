/**
 * @file complex.h
 * @brief Complex numbers, held as runtime objects the calculator recognises
 *
 * Quadrate has no complex type, so one is a reference-counted struct on the
 * stack: dup and drop manage it as they do any object, and the keypad's words
 * know what it is. Quadrate's own + - * do not. See README.md.
 */

#ifndef QDOS_COMPLEX_H
#define QDOS_COMPLEX_H

#include <quadrate/interp/interp.h>
#include <quadrate/rt/context.h>
#include <quadrate/rt/stack.h>

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Whether a real function may answer in complex, and how complex is shown */
typedef enum {
	QDOS_CPX_REAL = 0, ///< sqrt of a negative is an error, as on a TI in REAL
	QDOS_CPX_RECT,	   ///< a+bi
	QDOS_CPX_POLAR,	   ///< re^(θi)
	QDOS_CPX__COUNT
} qdos_cpx_mode;

void qdos_cpx_set_mode(qdos_cpx_mode mode);
qdos_cpx_mode qdos_cpx_get_mode(void);

/** @brief The parts of a stack element, if it is a complex number */
bool qdos_cpx_of(const qd_stack_element_t* element, double* re, double* im);

/** @brief Push re+im i; with no imaginary part, a plain float */
int qdos_cpx_push(qd_context* ctx, double re, double im);

/** @brief As the mode shows it, in at most @p room characters where it can be */
void qdos_cpx_format(double re, double im, char* out, size_t cap, size_t room);

/**
 * @brief Do a maths word the complex way, if its arguments call for it
 *
 * When any argument is complex, or the mode lets a real one leave the reals
 * (the square root of -4), @p result is the word's and true comes back.
 */
bool qdos_cpx_apply(qd_context* ctx, const char* word, size_t arity, int* result);

/** @brief complex, csplit, polar, real, imag, conj, angle and i */
void qdos_register_complex(qd_interp* interp);

#ifdef __cplusplus
}
#endif

#endif // QDOS_COMPLEX_H
