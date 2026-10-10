/**
 * @file builtins.c
 * @brief The native modules linked into the firmware, as there is no dlopen
 */

#include "sdkconfig.h"

#include "shell/native.h"

#if CONFIG_QDOS_DOOM
extern const qdos_native_module qdos_module_doom;
#endif

const qdos_native_builtin qdos_native_builtins[] = {
#if CONFIG_QDOS_DOOM
		{"libdoom.so", &qdos_module_doom},
#endif
		{NULL, NULL},
};
