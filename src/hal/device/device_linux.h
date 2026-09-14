/**
 * @file device_linux.h
 * @brief Device backend — Linux framebuffer and evdev
 */

#ifndef QDOS_DEVICE_LINUX_H
#define QDOS_DEVICE_LINUX_H

#include <qdos/hal.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Fill in @p hal with the Linux framebuffer/evdev backend */
void qdos_device_hal(qdos_hal* hal);

#ifdef __cplusplus
}
#endif

#endif // QDOS_DEVICE_LINUX_H
