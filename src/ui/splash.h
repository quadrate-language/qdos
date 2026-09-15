/**
 * @file splash.h
 * @brief What the panel shows while the machine is still starting
 */

#ifndef QDOS_SPLASH_H
#define QDOS_SPLASH_H

#include <qdos/hal.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Paint the startup screen. Only the backend display is used. */
void qdos_splash_draw(qdos_hal* hal);

#ifdef __cplusplus
}
#endif

#endif // QDOS_SPLASH_H
