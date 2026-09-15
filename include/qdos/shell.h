/**
 * @file shell.h
 * @brief The Quadrate shell
 */

#ifndef QDOS_SHELL_H
#define QDOS_SHELL_H

#include <qdos/hal.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct qdos_shell qdos_shell;

qdos_shell* qdos_shell_create(qdos_hal* hal);

/** @brief NULL is ignored */
void qdos_shell_destroy(qdos_shell* sh);

/** @brief Runs until the backend says stop */
void qdos_shell_run(qdos_shell* sh);

#ifdef __cplusplus
}
#endif

#endif // QDOS_SHELL_H
