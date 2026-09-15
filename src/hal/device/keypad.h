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

/** @brief evdev code to logical key; false for codes we do not use */
bool qdos_keypad_map(uint16_t code, bool shift, bool altgr, qdos_key_event* out);

typedef enum {
	QDOS_LAYOUT_US = 0,
	QDOS_LAYOUT_SE ///< Swedish, where the braces Quadrate needs are on AltGr
} qdos_layout;

/** @brief "us" or "se"; anything else leaves the layout unchanged */
bool qdos_keypad_set_layout(const char* name);

qdos_layout qdos_keypad_layout(void);

int qdos_keypad_open(const char* path);

/** @brief fd may be -1 for no keypad */
bool qdos_keypad_poll(int fd, qdos_key_event* out);

/** @brief -1 is ignored */
void qdos_keypad_close(int fd);

#ifdef __cplusplus
}
#endif

#endif // QDOS_KEYPAD_H
