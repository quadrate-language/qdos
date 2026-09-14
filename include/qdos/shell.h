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

/**
 * @brief Create a shell bound to a backend
 * @return New shell, or NULL on allocation failure
 */
qdos_shell* qdos_shell_create(qdos_hal* hal);

/** @brief Destroy a shell. NULL is ignored. */
void qdos_shell_destroy(qdos_shell* sh);

/** @brief Run until the backend reports it should stop */
void qdos_shell_run(qdos_shell* sh);

#ifdef __cplusplus
}
#endif

#endif // QDOS_SHELL_H
