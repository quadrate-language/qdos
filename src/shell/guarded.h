/**
 * @file guarded.h
 * @brief Evaluation that survives a fatal runtime error
 */

#ifndef QDOS_GUARDED_H
#define QDOS_GUARDED_H

#include <quadrate/interp/interp.h>

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Evaluate with recovery armed
 *
 * A type error in lib/rt -- `1.5 2.5 and`, a string where a number was wanted --
 * is fatal and would end the process. Recovery makes the runtime longjmp back
 * here instead. The stack may be left part-way through the failed word, which
 * is the documented price of surviving at all.
 */
bool qdos_guarded_eval(qd_interp* interp, const char* source);

/** @brief What went wrong, in QDOS's words when the runtime left none */
const char* qdos_guarded_error(qd_interp* interp);

/**
 * @brief Whatever the last evaluation printed, or an empty string
 *
 * `print` and `nl` are built into the runtime and write to stdout, which on the
 * device goes nowhere. Evaluation runs with stdout on a pipe so the shell can
 * put it on the panel instead.
 */
const char* qdos_guarded_output(void);

#ifdef __cplusplus
}
#endif

#endif // QDOS_GUARDED_H
