/**
 * @file sharp_lcd.h
 * @brief The Sharp LS027B7DH01 memory LCD, over SPI
 */

#ifndef QDOS_SHARP_LCD_H
#define QDOS_SHARP_LCD_H

#include <stdint.h>

/** @return 0 on success */
int qdos_lcd_init(void);

/**
 * @brief Show a panel image: QDOS_SCREEN_W by QDOS_SCREEN_H grey bytes
 *
 * Only the lines that differ from what the glass already holds are sent, which
 * for a cursor blink or one changed stack row is one line in two hundred and
 * forty.
 */
void qdos_lcd_present(const uint8_t* gray);

/** @brief Stop the once-a-second VCOM toggle, before the chip sleeps */
void qdos_lcd_stop(void);

#endif // QDOS_SHARP_LCD_H
