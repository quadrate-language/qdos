/**
 * @file builtins.c
 * @brief The native modules linked into the browser build, as there is no dlopen
 *
 * None yet: a module would be compiled to wasm and linked in, and listed here
 * under the name its .so has in the store, as esp32/main/builtins.c does.
 */

#include "shell/native.h"

const qdos_native_builtin qdos_native_builtins[] = {
		{NULL, NULL},
};
