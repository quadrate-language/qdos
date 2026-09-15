/**
 * @file mathwords.h
 * @brief Quadrate's math:: module, registered as shell words
 */

#ifndef QDOS_MATHWORDS_H
#define QDOS_MATHWORDS_H

#include <quadrate/interp/interp.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Every word the keypad and the catalog offer beyond the core vocabulary */
void qdos_register_math(qd_interp* interp);

#ifdef __cplusplus
}
#endif

#endif // QDOS_MATHWORDS_H
