/**
 * @file qemu_lcd.c
 * @brief The panel under Espressif's QEMU: its virtual RGB display, in a window
 *
 * The same interface as the Sharp driver, so the backend cannot tell them
 * apart. The device is a block of registers: set the window's size once, then
 * for each update give a rectangle and the address of its pixels. That is all
 * of Espressif's esp_lcd_qemu_rgb, which is not worth a managed dependency.
 */

#include "sharp_lcd.h"

#include "sdkconfig.h"

#if CONFIG_QDOS_LCD_QEMU

#include <qdos/hal.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "soc/syscon_reg.h"

#include <stdbool.h>
#include <string.h>

/** Window pixels per panel pixel: the glass is 400 by 240, small on a desktop */
#define SCALE 2
#define WIN_W (QDOS_SCREEN_W * SCALE)
#define LINE_BYTES (QDOS_SCREEN_W / 8)

/** Grey below this is ink, as the simulator and the kernel's driver take it */
#define INK_BELOW 128

/** The simulator's colours, as 0x00RRGGBB */
#define INK 0x1A1C1Au
#define PAPER 0xC9CEC6u

/** "QEMU", in the word just below SYSCON_DATE_REG; real silicon has no such thing */
#define QEMU_ORIGIN 0x51454d55

typedef volatile struct {
	uint32_t version;
	uint32_t size; ///< width << 16 | height
	uint32_t from; ///< x << 16 | y
	uint32_t to;   ///< x << 16 | y, exclusive
	const void* content;
	uint32_t busy;
	uint32_t bpp;
} rgb_regs;

static rgb_regs* const g_regs = (rgb_regs*)0x21000000;

// The device's own memory, for the pixels of an update: the rectangle packed
// row after row. The shell's stack needs every byte of internal RAM there is.
static uint32_t (*const g_vram)[WIN_W] = (uint32_t(*)[WIN_W])0x20000000;

static const char* TAG = "lcd";

EXT_RAM_BSS_ATTR static uint8_t g_shown[QDOS_SCREEN_H][LINE_BYTES]; ///< A set bit is ink

/** Panel rows of the update QEMU has yet to take, inclusive; lo > hi for none */
static int g_lo = QDOS_SCREEN_H;
static int g_hi = -1;
static bool g_whole = true; ///< The window starts out black, not paper

int qdos_lcd_init(void) {
	if (REG_READ(SYSCON_DATE_REG - 4) != QEMU_ORIGIN) {
		ESP_LOGE(TAG, "not under QEMU");
		return 1;
	}
	g_regs->size = (uint32_t)WIN_W << 16 | (uint32_t)(QDOS_SCREEN_H * SCALE);
	g_regs->bpp = 32;
	return 0;
}

// QEMU takes an update when it refreshes the window, not when it is asked, and
// with no window never. So nothing waits for it: an update it has not taken
// yet grows to cover the new rows as well.
void qdos_lcd_present(const uint8_t* gray) {
	if (!g_regs->busy) {
		g_lo = QDOS_SCREEN_H;
		g_hi = -1;
	}
	if (g_whole) {
		g_lo = 0;
		g_hi = QDOS_SCREEN_H - 1;
		g_whole = false;
	}

	for (int y = 0; y < QDOS_SCREEN_H; y++) {
		uint8_t line[LINE_BYTES] = {0};
		const uint8_t* row = gray + (size_t)y * QDOS_SCREEN_W;
		for (int x = 0; x < QDOS_SCREEN_W; x++) {
			if (row[x] < INK_BELOW) {
				line[x / 8] |= (uint8_t)(1u << (x % 8));
			}
		}
		if (memcmp(line, g_shown[y], sizeof(line)) != 0) {
			memcpy(g_shown[y], line, sizeof(line));
			g_lo = y < g_lo ? y : g_lo;
			g_hi = y > g_hi ? y : g_hi;
		}
	}
	if (g_lo > g_hi) {
		return;
	}

	for (int y = g_lo; y <= g_hi; y++) {
		uint32_t* out = g_vram[(y - g_lo) * SCALE];
		for (int x = 0; x < QDOS_SCREEN_W; x++) {
			const uint32_t c = (g_shown[y][x / 8] >> (x % 8)) & 1 ? INK : PAPER;
			for (int s = 0; s < SCALE; s++) {
				out[x * SCALE + s] = c;
			}
		}
		for (int s = 1; s < SCALE; s++) {
			memcpy(g_vram[(y - g_lo) * SCALE + s], out, sizeof(g_vram[0]));
		}
	}

	g_regs->from = (uint32_t)(g_lo * SCALE);
	g_regs->to = (uint32_t)WIN_W << 16 | (uint32_t)((g_hi + 1) * SCALE);
	g_regs->content = g_vram;
	g_regs->busy = 1;
}

void qdos_lcd_stop(void) {
}

#endif // CONFIG_QDOS_LCD_QEMU
