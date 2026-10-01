/**
 * @file serial_keys.h
 * @brief A terminal on the USB port, as a keyboard
 *
 * Until the keypad is built this is how the machine is typed at, as a USB
 * keyboard is on the Pi; afterwards it stays, for typing a long line.
 */

#ifndef QDOS_SERIAL_KEYS_H
#define QDOS_SERIAL_KEYS_H

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/** @return 0 on success; presses go to @p out as qdos_input */
int qdos_serial_keys_init(QueueHandle_t out);

#endif // QDOS_SERIAL_KEYS_H
