/**
 * @file framebuffer.h
 * @brief Convert the QDOS panel image into a Linux framebuffer
 *
 * Split out of the device backend so it can be tested without a framebuffer
 * device, on any architecture. This is the part of the device path most likely
 * to be wrong — pixel packing, stride and offsets are easy to get subtly
 * mismatched and produce a display that is skewed, tinted or blank.
 */

#ifndef QDOS_FRAMEBUFFER_H
#define QDOS_FRAMEBUFFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Geometry of a Linux framebuffer, as reported by FBIOGET_*SCREENINFO
 */
typedef struct {
	uint32_t width;	 ///< Visible width in pixels (fb_var_screeninfo.xres)
	uint32_t height; ///< Visible height in pixels (fb_var_screeninfo.yres)
	uint32_t pitch;	 ///< Bytes per scanline (fb_fix_screeninfo.line_length)
	uint32_t bpp;	 ///< Bits per pixel; 16 (RGB565) and 32 (XRGB8888) are supported
} qdos_fb_info;

/**
 * @brief Is this framebuffer one we know how to write?
 *
 * @return true for 16 or 32 bits per pixel with a pitch wide enough for the
 *         visible width
 */
bool qdos_fb_supported(const qdos_fb_info* info);

/**
 * @brief Write the panel image into a framebuffer
 *
 * The panel image is centred if the framebuffer is larger and clipped if it is
 * smaller, so an oversized HDMI output during bring-up shows the panel in the
 * middle of the screen rather than stretched or cropped at a corner. Pixels
 * outside the copied region are left untouched.
 *
 * @param info Framebuffer geometry; must satisfy qdos_fb_supported()
 * @param dst  Start of the mapped framebuffer, at least info->pitch * info->height bytes
 * @param src  QDOS_SCREEN_W * QDOS_SCREEN_H grayscale pixels, row-major
 */
void qdos_fb_blit(const qdos_fb_info* info, uint8_t* dst, const uint8_t* src);

/**
 * @brief Pack an 8-bit gray level into RGB565
 *
 * Exposed for testing; the truncation is what makes this worth pinning.
 */
uint16_t qdos_fb_gray_to_rgb565(uint8_t gray);

#ifdef __cplusplus
}
#endif

#endif // QDOS_FRAMEBUFFER_H
