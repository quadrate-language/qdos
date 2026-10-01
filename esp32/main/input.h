/**
 * @file input.h
 * @brief What the keypad and the serial console hand the HAL
 */

#ifndef QDOS_ESP_INPUT_H
#define QDOS_ESP_INPUT_H

#include <qdos/keys.h>

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief One press, from either source
 *
 * The matrix reports where a key is, not what it means: what it means depends
 * on the face the pad is showing, which belongs to the HAL. The console has no
 * faces and reports the key outright.
 */
typedef struct {
	bool pad;			///< A matrix position rather than a key
	uint8_t row;		///< Pad row, top first
	uint8_t col;		///< Position in the row, left first
	qdos_key_event key; ///< When !pad
} qdos_input;

#endif // QDOS_ESP_INPUT_H
