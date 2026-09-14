/**
 * @file keypad.h
 * @brief Reading the calculator keypad through evdev
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
 * @param code evdev code, e.g. KEY_1 or KEY_KPPLUS from <linux/input-event-codes.h>
 * @param shift Whether a shift key is held
 * @param altgr Whether AltGr is held, which reaches the third level on layouts
 *              that have one
 * @param[out] out Receives the logical key when true is returned
 * @return true if the code is one the calculator uses
 */
bool qdos_keypad_map(uint16_t code, bool shift, bool altgr, qdos_key_event* out);

/** @brief Keyboard layouts the device backend understands */
typedef enum {
	QDOS_LAYOUT_US = 0,
	QDOS_LAYOUT_SE ///< Swedish, where braces are on AltGr
} qdos_layout;

/**
 * @brief Choose the layout used to turn key presses into characters
 * @param name "us" or "se"; anything else leaves the layout unchanged
 * @return true if the name was recognised
 */
bool qdos_keypad_set_layout(const char* name);

/** @brief The layout currently in use */
qdos_layout qdos_keypad_layout(void);

/**
 * @brief Open an evdev node for reading
 * @param path Device node, e.g. /dev/input/event0
 * @return File descriptor, or -1 on failure with errno set
 * @note Opened non-blocking; qdos_keypad_poll() never waits
 */
int qdos_keypad_open(const char* path);

/**
 * @brief Read the next usable key press, if one is waiting
 * @param fd Descriptor from qdos_keypad_open(), or -1 for "no keypad"
 * @param[out] out Receives the event
 * @return true if an event was produced
 */
bool qdos_keypad_poll(int fd, qdos_key_event* out);

/** @brief Close a descriptor from qdos_keypad_open(). -1 is ignored. */
void qdos_keypad_close(int fd);

#ifdef __cplusplus
}
#endif

#endif // QDOS_KEYPAD_H
