/**
 * @file sim_sdl3.h
 * @brief SDL3 simulator backend
 */

#ifndef QDOS_SIM_SDL3_H
#define QDOS_SIM_SDL3_H

#include <qdos/hal.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Fill in @p hal with the SDL3 simulator backend */
void qdos_sim_hal(qdos_hal* hal);

#ifdef __cplusplus
}
#endif

#endif // QDOS_SIM_SDL3_H
