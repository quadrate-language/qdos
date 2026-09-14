/**
 * @file framebuffer.c
 * @brief Convert the QDOS panel image into a Linux framebuffer
 */

#include "framebuffer.h"

#include <qdos/hal.h>

#include <string.h>

uint16_t qdos_fb_gray_to_rgb565(uint8_t gray) {
	// 5-6-5, high bits of the same level; the low bits are dropped.
	const uint16_t r = (uint16_t)((gray & 0xF8u) >> 3);
	const uint16_t g = (uint16_t)((gray & 0xFCu) >> 2);
	const uint16_t b = (uint16_t)(gray >> 3);
	return (uint16_t)((r << 11) | (g << 5) | b);
}

bool qdos_fb_supported(const qdos_fb_info* info) {
	if (info == NULL || info->width == 0 || info->height == 0) {
		return false;
	}
	if (info->bpp != 16 && info->bpp != 32) {
		return false;
	}
	// A short pitch would run writes into the next scanline.
	return info->pitch >= info->width * (info->bpp / 8u);
}

void qdos_fb_blit(const qdos_fb_info* info, uint8_t* dst, const uint8_t* src) {
	if (info == NULL || dst == NULL || src == NULL || !qdos_fb_supported(info)) {
		return;
	}

	const uint32_t copy_w = (QDOS_SCREEN_W < info->width) ? (uint32_t)QDOS_SCREEN_W : info->width;
	const uint32_t copy_h = (QDOS_SCREEN_H < info->height) ? (uint32_t)QDOS_SCREEN_H : info->height;
	const uint32_t off_x = (info->width - copy_w) / 2u;
	const uint32_t off_y = (info->height - copy_h) / 2u;

	for (uint32_t y = 0; y < copy_h; y++) {
		const uint8_t* row = &src[(size_t)y * QDOS_SCREEN_W];
		uint8_t* line = dst + (size_t)(y + off_y) * info->pitch;

		if (info->bpp == 32) {
			// Byte-wise: an unaligned 32-bit store faults on some ARM.
			uint8_t* px = line + (size_t)off_x * 4u;
			for (uint32_t x = 0; x < copy_w; x++) {
				const uint8_t gray = row[x];
				px[x * 4u + 0u] = gray; // blue
				px[x * 4u + 1u] = gray; // green
				px[x * 4u + 2u] = gray; // red
				px[x * 4u + 3u] = 0xFF; // unused or alpha
			}
		} else {
			uint8_t* px = line + (size_t)off_x * 2u;
			for (uint32_t x = 0; x < copy_w; x++) {
				const uint16_t packed = qdos_fb_gray_to_rgb565(row[x]);
				px[x * 2u + 0u] = (uint8_t)(packed & 0xFFu); // little-endian low byte
				px[x * 2u + 1u] = (uint8_t)(packed >> 8);
			}
		}
	}
}
