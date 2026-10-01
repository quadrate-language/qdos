/**
 * @file keymatrix.h
 * @brief The keypad, read as a matrix of GPIOs
 */

#ifndef QDOS_KEYMATRIX_H
#define QDOS_KEYMATRIX_H

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/**
 * @brief Start watching the pad; presses go to @p out as qdos_input
 *
 * Nothing runs while no key is down: every row is held low and the columns
 * wait on a level interrupt, and only a press starts the scan.
 *
 * @return 0 on success
 */
int qdos_matrix_init(QueueHandle_t out);

/**
 * @brief Make any key wake the chip from deep sleep
 *
 * Waits for the pad to be let go first -- the press that switched the machine
 * off would otherwise switch it straight back on.
 */
void qdos_matrix_arm_wake(void);

#endif // QDOS_KEYMATRIX_H
