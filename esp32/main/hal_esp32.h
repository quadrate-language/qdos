/**
 * @file hal_esp32.h
 * @brief Device backend -- the ESP32-S3
 */

#ifndef QDOS_HAL_ESP32_H
#define QDOS_HAL_ESP32_H

#include <qdos/hal.h>

/** @brief Where uploads land: the inbox scope */
#define QDOS_ESP32_INBOX "/data/inbox"

/** @brief Something landed on the inbox; the shell reads it again */
void qdos_esp32_store_touched(void);

/** @brief Fill in @p hal with the ESP32-S3 backend */
void qdos_esp32_hal(qdos_hal* hal);

/** @brief Switch off: deep sleep until a key is pressed, which starts afresh */
void qdos_esp32_power_off(void);

#endif // QDOS_HAL_ESP32_H
