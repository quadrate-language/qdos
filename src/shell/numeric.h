/**
 * @file numeric.h
 * @brief Roots, extrema, slopes and areas of a function known only by calling it
 *
 * What a graphing calculator's CALC menu does. The function is interpreted
 * Quadrate, so every call costs, and each method here caps how many it makes.
 */

#ifndef QDOS_NUMERIC_H
#define QDOS_NUMERIC_H

#include "graph.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief An x in [a, b] where f is nought
 *
 * The interval is scanned for a change of sign, and the first one found is
 * narrowed down. A root that only touches the axis, as x^2 does at 0, has no
 * change of sign and is found as a minimum of |f| that reaches zero.
 */
bool qdos_num_root(qdos_graph_fn fn, void* user, double a, double b, double* x);

/** @brief Where f is lowest in [a, b], and its value there */
bool qdos_num_minimum(qdos_graph_fn fn, void* user, double a, double b, double* x, double* y);

/** @brief Where f is highest in [a, b], and its value there */
bool qdos_num_maximum(qdos_graph_fn fn, void* user, double a, double b, double* x, double* y);

/** @brief f'(x), by central differences; false where f has no value either side */
bool qdos_num_derivative(qdos_graph_fn fn, void* user, double x, double* d);

/** @brief The integral of f from a to b, negative when b < a */
bool qdos_num_integral(qdos_graph_fn fn, void* user, double a, double b, double* area);

#ifdef __cplusplus
}
#endif

#endif // QDOS_NUMERIC_H
