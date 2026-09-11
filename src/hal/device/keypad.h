/**
 * @file keypad.h
 * @brief Reading the calculator keypad through evdev
 *
 * Split out of the device backend so the key path can be exercised on its own:
 * the mapping is pure and testable anywhere, and the device side can be driven
 * by a virtual input device rather than needing the real keypad to exist.
 */

#ifndef QDOS_KEYPAD_H
#define QDOS_KEYPAD_H

#include <qdos/hal.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Translate a Linux evdev key code into a logical key
 *
 * @param code evdev code, e.g. KEY_1 or KEY_KPPLUS from <linux/input-event-codes.h>
 * @param[out] out Receives the logical key when true is returned
 * @return true if the code is one the calculator uses
 */
bool qdos_keypad_map(uint16_t code, qdos_key_event* out);

/**
 * @brief Open an evdev node for reading
 *
 * @param path Device node, e.g. /dev/input/event0
 * @return File descriptor, or -1 on failure with errno set
 *
 * @note Opened non-blocking; qdos_keypad_poll() never waits
 */
int qdos_keypad_open(const char* path);

/**
 * @brief Read the next usable key press, if one is waiting
 *
 * Key releases and auto-repeats are skipped, as are codes the calculator does
 * not use, so a caller sees only presses it can act on.
 *
 * @param fd Descriptor from qdos_keypad_open(), or -1 for "no keypad"
 * @param[out] out Receives the event
 * @return true if an event was produced
 */
bool qdos_keypad_poll(int fd, qdos_key_event* out);

/**
 * @brief Close a descriptor from qdos_keypad_open(). -1 is ignored.
 */
void qdos_keypad_close(int fd);

#ifdef __cplusplus
}
#endif

#endif // QDOS_KEYPAD_H
