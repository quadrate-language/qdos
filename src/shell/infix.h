/**
 * @file infix.h
 * @brief A Y= body written as a TI takes it, turned into the RPN Quadrate runs
 *
 * An HP 48 plots either an algebraic or an RPN program, and so does Y=. A body
 * that is already RPN is left as it is; one that is not is read as a formula,
 * `x^2 + 3x - sin(x)/2`, and written out as the words that work it out.
 */

#ifndef QDOS_INFIX_H
#define QDOS_INFIX_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Cannot be RPN: a word runs short of values, or a token is a formula's */
#define QDOS_RPN_NOT -1

/** @brief A word whose effect on the stack is not known here, so it may well be */
#define QDOS_RPN_UNKNOWN -2

/**
 * @brief How many values a body leaves as RPN, starting from an empty stack
 *
 * x, y, t and theta each push one, as a slot's parameters do. Counted from the
 * words whose effect is known; anything else, a program's own word or control
 * flow, is QDOS_RPN_UNKNOWN and the body is taken on trust.
 */
int qdos_rpn_results(const char* body);

/**
 * @brief Write a formula out as RPN
 *
 * Precedence as on a TI: `^` binds tightest and to the right, then unary
 * minus, so `-x^2` is -(x^2); then `*`, `/` and multiplication by juxtaposition
 * (`2x`, `3 sin(x)`, `(x+1)(x-1)`, but not `x 2`, which is RPN missing a word);
 * then `+` and `-`. A function takes its
 * argument in brackets, several of them split by commas, or without, `sin x`,
 * where it is one of the known functions of one argument. `sq`, `cb`, `inv`,
 * `fac` and `!` after a value apply to it, as the TI's x², x⁻¹ and ! keys do.
 * The keypad's words are its operators too: `plus`, `minus`, `times`,
 * `divide`, and `pow` between two values.
 *
 * Numbers are written as floats, so `1/2` is a half rather than the nought
 * Quadrate's integers make of it. A comma outside brackets separates values
 * left side by side, `cos(t), sin(t)` being a parametric curve's x and y.
 *
 * @param results Receives how many values the formula leaves
 * @param error Receives what is wrong, written for the message line
 * @return false when @p text is not a formula
 */
bool qdos_infix_to_rpn(const char* text, char* out, size_t cap, int* results, char* error, size_t error_cap);

#ifdef __cplusplus
}
#endif

#endif // QDOS_INFIX_H
