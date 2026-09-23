/**
 * @file wallclock.h
 * @brief The time of day, for backends that run on Linux
 */

#ifndef QDOS_WALLCLOCK_H
#define QDOS_WALLCLOCK_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Seconds since local midnight; false while the clock is unset */
bool qdos_wallclock(int* seconds);

#ifdef __cplusplus
}
#endif

#endif // QDOS_WALLCLOCK_H
