/**
 * @file test_framebuffer.c
 * @brief Framebuffer conversion tests
 *
 * The device backend cannot be run here, but the pixel packing can: these
 * blit into ordinary memory shaped like a framebuffer and read the bytes back.
 * Stride, offsets and channel order are exactly the things that produce a
 * skewed or tinted display on first bring-up, and they are all checkable
 * without hardware.
 */

// POSIX interfaces on top of a strict c11 build
#define _POSIX_C_SOURCE 200809L

#include "check.h"

#include "../src/hal/device/framebuffer.h"

#include <qdos/hal.h>

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define PANEL_PIXELS ((size_t)QDOS_SCREEN_W * QDOS_SCREEN_H)

/** A panel image with a known value at every pixel. */
static uint8_t* make_panel(uint8_t fill) {
	uint8_t* panel = malloc(PANEL_PIXELS);
	memset(panel, fill, PANEL_PIXELS);
	return panel;
}

static void test_supported(void) {
	qdos_fb_info info = {.width = 320, .height = 240, .pitch = 1280, .bpp = 32};
	CHECK(qdos_fb_supported(&info));

	info.bpp = 16;
	info.pitch = 640;
	CHECK(qdos_fb_supported(&info));

	// Depths we cannot write must be refused rather than producing garbage
	info.bpp = 24;
	CHECK(!qdos_fb_supported(&info));
	info.bpp = 8;
	CHECK(!qdos_fb_supported(&info));

	// A pitch shorter than the visible width would overrun into the next line
	info.bpp = 32;
	info.width = 320;
	info.pitch = 320;
	CHECK(!qdos_fb_supported(&info));

	CHECK(!qdos_fb_supported(NULL));
}

static void test_rgb565_packing(void) {
	// Black and white must land exactly on the endpoints, or the display is
	// washed out at one end
	CHECK(qdos_fb_gray_to_rgb565(0x00) == 0x0000);
	CHECK(qdos_fb_gray_to_rgb565(0xFF) == 0xFFFF);

	// Mid gray: r = 0x80>>3 = 16, g = 0x80>>2 = 32, b = 0x80>>3 = 16
	CHECK(qdos_fb_gray_to_rgb565(0x80) == ((16u << 11) | (32u << 5) | 16u));
}

static void test_blit_32bpp_exact_fit(void) {
	const uint32_t pitch = QDOS_SCREEN_W * 4;
	qdos_fb_info info = {.width = QDOS_SCREEN_W, .height = QDOS_SCREEN_H, .pitch = pitch, .bpp = 32};

	uint8_t* fb = calloc((size_t)pitch * QDOS_SCREEN_H, 1);
	uint8_t* panel = make_panel(0x42);

	qdos_fb_blit(&info, fb, panel);

	// Gray replicated across all three channels, alpha opaque
	CHECK(fb[0] == 0x42 && fb[1] == 0x42 && fb[2] == 0x42);
	CHECK(fb[3] == 0xFF);

	// Last pixel of the last row must be written too
	const size_t last = (size_t)(QDOS_SCREEN_H - 1) * pitch + (size_t)(QDOS_SCREEN_W - 1) * 4;
	CHECK(fb[last] == 0x42);

	free(fb);
	free(panel);
}

static void test_blit_16bpp_exact_fit(void) {
	const uint32_t pitch = QDOS_SCREEN_W * 2;
	qdos_fb_info info = {.width = QDOS_SCREEN_W, .height = QDOS_SCREEN_H, .pitch = pitch, .bpp = 16};

	uint8_t* fb = calloc((size_t)pitch * QDOS_SCREEN_H, 1);
	uint8_t* panel = make_panel(0xFF);

	qdos_fb_blit(&info, fb, panel);

	// White is 0xFFFF; stored little-endian
	CHECK(fb[0] == 0xFF && fb[1] == 0xFF);

	const size_t last = (size_t)(QDOS_SCREEN_H - 1) * pitch + (size_t)(QDOS_SCREEN_W - 1) * 2;
	CHECK(fb[last] == 0xFF && fb[last + 1] == 0xFF);

	free(fb);
	free(panel);
}

static void test_blit_respects_pitch(void) {
	// A pitch wider than the visible area is normal — the bytes past the
	// visible width must stay untouched, or the image skews
	const uint32_t pitch = QDOS_SCREEN_W * 4 + 64;
	qdos_fb_info info = {.width = QDOS_SCREEN_W, .height = QDOS_SCREEN_H, .pitch = pitch, .bpp = 32};

	uint8_t* fb = calloc((size_t)pitch * QDOS_SCREEN_H, 1);
	uint8_t* panel = make_panel(0x7F);

	qdos_fb_blit(&info, fb, panel);

	// Padding at the end of row 0 untouched
	for (uint32_t i = QDOS_SCREEN_W * 4; i < pitch; i++) {
		if (fb[i] != 0) {
			CHECK(0 && "blit wrote into the scanline padding");
			goto done;
		}
	}
	// Row 1 starts one pitch in, not one visible-width in
	CHECK(fb[pitch] == 0x7F);
done:
	free(fb);
	free(panel);
}

static void test_blit_centres_on_larger_framebuffer(void) {
	// Bring-up case: an HDMI output much bigger than the panel
	const uint32_t w = 640, h = 480, pitch = w * 4;
	qdos_fb_info info = {.width = w, .height = h, .pitch = pitch, .bpp = 32};

	uint8_t* fb = calloc((size_t)pitch * h, 1);
	uint8_t* panel = make_panel(0x33);

	qdos_fb_blit(&info, fb, panel);

	const uint32_t off_x = (w - QDOS_SCREEN_W) / 2;
	const uint32_t off_y = (h - QDOS_SCREEN_H) / 2;

	// Top-left corner untouched, panel image at the offset
	CHECK(fb[0] == 0);
	CHECK(fb[(size_t)off_y * pitch + (size_t)off_x * 4] == 0x33);

	// Just left of the panel is still background
	CHECK(fb[(size_t)off_y * pitch + (size_t)(off_x - 1) * 4] == 0);

	free(fb);
	free(panel);
}

static void test_blit_clips_on_smaller_framebuffer(void) {
	// A panel smaller than QDOS expects must not be overrun
	const uint32_t w = 128, h = 64, pitch = w * 2;
	qdos_fb_info info = {.width = w, .height = h, .pitch = pitch, .bpp = 16};

	const size_t size = (size_t)pitch * h;
	uint8_t* fb = calloc(size + 64, 1); // guard bytes past the end
	uint8_t* panel = make_panel(0xFF);

	qdos_fb_blit(&info, fb, panel);

	CHECK(fb[0] == 0xFF);
	for (size_t i = size; i < size + 64; i++)
		if (fb[i] != 0) {
			CHECK(0 && "blit wrote past the end of the framebuffer");
			goto done;
		}
	CHECK(1);
done:
	free(fb);
	free(panel);
}

static void test_blit_rejects_bad_input(void) {
	qdos_fb_info info = {.width = 320, .height = 240, .pitch = 1280, .bpp = 24}; // unsupported
	uint8_t* fb = calloc(1280 * 240, 1);
	uint8_t* panel = make_panel(0xFF);

	qdos_fb_blit(&info, fb, panel); // must do nothing rather than write garbage
	CHECK(fb[0] == 0);

	qdos_fb_blit(NULL, fb, panel);
	qdos_fb_blit(&info, NULL, panel);
	qdos_fb_blit(&info, fb, NULL);
	CHECK(1); // must not crash

	free(fb);
	free(panel);
}


/**
 * Probe a real framebuffer device.
 *
 * Covers the part the pure tests cannot: that the ioctls return what the code
 * expects, that the geometry passes our own validation, and that the size
 * implied by pitch and height is actually mappable. Nothing is written to the
 * device — the blit goes into a private buffer shaped from the real geometry,
 * so running this never disturbs whatever is on screen.
 *
 * Skips when there is no readable framebuffer, which is the normal case for an
 * unprivileged user.
 */
static void test_real_framebuffer_device(void) {
	const char* path = getenv("QDOS_FB");
	if (path == NULL)
		path = "/dev/fb0";

	const int fd = open(path, O_RDONLY);
	if (fd < 0) {
		printf("framebuffer: SKIP device probe (%s: %s)\n", path, strerror(errno));
		return;
	}

	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
	if (ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0 || ioctl(fd, FBIOGET_FSCREENINFO, &fix) < 0) {
		printf("framebuffer: SKIP device probe (%s: cannot query geometry)\n", path);
		close(fd);
		return;
	}

	const qdos_fb_info info = {
			.width = var.xres,
			.height = var.yres,
			.pitch = fix.line_length,
			.bpp = var.bits_per_pixel,
	};
	printf("framebuffer: probing %s — %ux%u, %u bpp, pitch %u\n", path, info.width, info.height, info.bpp,
			info.pitch);

	// The device reports a geometry; our validation must agree it is usable, or
	// explain itself by rejecting an unsupported depth
	if (info.bpp == 16 || info.bpp == 32) {
		CHECK(qdos_fb_supported(&info));
	} else {
		CHECK(!qdos_fb_supported(&info));
		printf("framebuffer: device is %u bpp, which QDOS does not drive\n", info.bpp);
	}

	// Mapping read-only proves the size implied by pitch * height is right
	const size_t size = (size_t)info.pitch * info.height;
	void* mapped = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
	CHECK(mapped != MAP_FAILED);
	if (mapped != MAP_FAILED)
		munmap(mapped, size);

	// Blit into a private buffer with the device's real geometry
	if (qdos_fb_supported(&info)) {
		uint8_t* shadow = calloc(size, 1);
		uint8_t* panel = make_panel(0x5A);
		qdos_fb_blit(&info, shadow, panel);

		const uint32_t off_x = (info.width > QDOS_SCREEN_W) ? (info.width - QDOS_SCREEN_W) / 2 : 0;
		const uint32_t off_y = (info.height > QDOS_SCREEN_H) ? (info.height - QDOS_SCREEN_H) / 2 : 0;
		const size_t at = (size_t)off_y * info.pitch + (size_t)off_x * (info.bpp / 8u);
		CHECK(shadow[at] != 0);

		free(shadow);
		free(panel);
	}

	close(fd);
}

int main(void) {
	test_supported();
	test_rgb565_packing();
	test_blit_32bpp_exact_fit();
	test_blit_16bpp_exact_fit();
	test_blit_respects_pitch();
	test_blit_centres_on_larger_framebuffer();
	test_blit_clips_on_smaller_framebuffer();
	test_blit_rejects_bad_input();
	test_real_framebuffer_device();
	return check_report("framebuffer");
}
