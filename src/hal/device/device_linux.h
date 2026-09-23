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

/**
 * @brief Charge left in the first battery under @p root, 0 to 100
 *
 * @p root is /sys/class/power_supply on the machine; a parameter so a test can
 * lay out its own. -1 when no supply there is a battery that reports one.
 */
int qdos_power_supply_capacity(const char* root);

/**
 * @brief Charge left in the first battery under @p root, 0 to 100
 *
 * @p root is /sys/class/power_supply on the machine; a parameter so a test can
 * lay out its own. -1 when no supply there is a battery that reports one.
 */
int qdos_power_supply_capacity(const char* root);

#ifdef __cplusplus
}
#endif

#endif // QDOS_DEVICE_LINUX_H
