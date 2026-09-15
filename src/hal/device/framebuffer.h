/**
 * @file framebuffer.h
 * @brief Convert the QDOS panel image into a Linux framebuffer
 */

#ifndef QDOS_FRAMEBUFFER_H
#define QDOS_FRAMEBUFFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Geometry of a Linux framebuffer, as reported by FBIOGET_*SCREENINFO */
typedef struct {
	uint32_t width;	 ///< Visible width in pixels (fb_var_screeninfo.xres)
	uint32_t height; ///< Visible height in pixels (fb_var_screeninfo.yres)
	uint32_t pitch;	 ///< Bytes per scanline (fb_fix_screeninfo.line_length)
	uint32_t bpp;	 ///< Bits per pixel; 16 (RGB565) and 32 (XRGB8888) are supported
} qdos_fb_info;

/** @brief True for 16 or 32 bpp with a pitch wide enough for the width */
bool qdos_fb_supported(const qdos_fb_info* info);

/**
 * @brief Write the panel image into a mapped framebuffer
 * @param info Must satisfy qdos_fb_supported()
 */
void qdos_fb_blit(const qdos_fb_info* info, uint8_t* dst, const uint8_t* src);

/** @brief Pack an 8-bit gray level into RGB565 */
uint16_t qdos_fb_gray_to_rgb565(uint8_t gray);

#ifdef __cplusplus
}
#endif

#endif // QDOS_FRAMEBUFFER_H
