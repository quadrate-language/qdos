/**
 * @file device_linux.c
 * @brief Device backend — Linux framebuffer and evdev
 *
 * The deployment target: an mmapped framebuffer for the panel and an evdev
 * node for the keypad. Both are kernel interfaces rather than board-specific
 * ones, so this backend works over SSH on a Pi with an HDMI or SPI panel
 * before any custom hardware exists, and stays correct once it does.
 *
 * Untested against real hardware — there is none yet. The framebuffer and
 * input paths are written against the documented ioctl and event interfaces;
 * expect the first bring-up to shake out format assumptions.
 */

// POSIX interfaces (fileno, fsync, nanosleep) on top of a strict c11 build
#define _POSIX_C_SOURCE 200809L

#include "device_linux.h"

#include "framebuffer.h"
#include "keypad.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct {
	int fb_fd;
	int input_fd;
	uint8_t* fb_mem;   ///< mmapped framebuffer
	size_t fb_size;	   ///< Bytes mapped
	uint32_t fb_w;	   ///< Panel width in pixels
	uint32_t fb_h;	   ///< Panel height in pixels
	uint32_t fb_bpp;   ///< Bits per pixel
	uint32_t fb_pitch; ///< Bytes per scanline
	bool running;
	const char* store_dir;
} device_state;

static const char* env_or(const char* name, const char* fallback) {
	const char* value = getenv(name);
	return (value && *value) ? value : fallback;
}

static int device_init(qdos_hal* hal) {
	device_state* st = (device_state*)hal->impl;

	st->store_dir = env_or("QDOS_STORE", "/var/lib/qdos");

	const char* fb_path = env_or("QDOS_FB", "/dev/fb0");
	st->fb_fd = open(fb_path, O_RDWR);
	if (st->fb_fd < 0) {
		fprintf(stderr, "qdos: cannot open %s: %s\n", fb_path, strerror(errno));
		return 1;
	}

	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
	if (ioctl(st->fb_fd, FBIOGET_VSCREENINFO, &var) < 0 || ioctl(st->fb_fd, FBIOGET_FSCREENINFO, &fix) < 0) {
		fprintf(stderr, "qdos: cannot query %s: %s\n", fb_path, strerror(errno));
		return 1;
	}

	st->fb_w = var.xres;
	st->fb_h = var.yres;
	st->fb_bpp = var.bits_per_pixel;
	st->fb_pitch = fix.line_length;
	st->fb_size = (size_t)fix.line_length * var.yres;

	const qdos_fb_info info = {
			.width = st->fb_w,
			.height = st->fb_h,
			.pitch = st->fb_pitch,
			.bpp = st->fb_bpp,
	};
	if (!qdos_fb_supported(&info)) {
		fprintf(stderr, "qdos: unsupported framebuffer: %ux%u, %u bpp, pitch %u\n", st->fb_w, st->fb_h, st->fb_bpp,
				st->fb_pitch);
		return 1;
	}

	st->fb_mem = mmap(NULL, st->fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, st->fb_fd, 0);
	if (st->fb_mem == MAP_FAILED) {
		st->fb_mem = NULL;
		fprintf(stderr, "qdos: cannot map %s: %s\n", fb_path, strerror(errno));
		return 1;
	}

	// The keypad is optional at bring-up: a panel with no input is still worth
	// seeing, and saying so beats refusing to start
	const char* input_path = env_or("QDOS_INPUT", "/dev/input/event0");
	st->input_fd = qdos_keypad_open(input_path);
	if (st->input_fd < 0)
		fprintf(stderr, "qdos: no keypad on %s: %s\n", input_path, strerror(errno));

	st->running = true;
	return 0;
}

static void device_shutdown(qdos_hal* hal) {
	device_state* st = (device_state*)hal->impl;
	if (!st)
		return;

	if (st->fb_mem)
		munmap(st->fb_mem, st->fb_size);
	if (st->fb_fd >= 0)
		close(st->fb_fd);
	qdos_keypad_close(st->input_fd);

	st->fb_mem = NULL;
	st->fb_fd = -1;
	st->input_fd = -1;
}

static void device_present(qdos_hal* hal, const uint8_t* fb) {
	device_state* st = (device_state*)hal->impl;
	if (!st->fb_mem) {
		return;
	}

	const qdos_fb_info info = {
			.width = st->fb_w,
			.height = st->fb_h,
			.pitch = st->fb_pitch,
			.bpp = st->fb_bpp,
	};
	qdos_fb_blit(&info, st->fb_mem, fb);
}

static bool device_poll_key(qdos_hal* hal, qdos_key_event* out) {
	device_state* st = (device_state*)hal->impl;
	return qdos_keypad_poll(st->input_fd, out);
}

static bool device_running(qdos_hal* hal) {
	return ((device_state*)hal->impl)->running;
}

static void device_idle(qdos_hal* hal) {
	(void)hal;
	// A real idle belongs here: block on the input fd with a timeout rather
	// than sleeping, so the CPU is not woken 60 times a second to find nothing.
	// Battery life depends on getting this right.
	const struct timespec frame = {.tv_sec = 0, .tv_nsec = 16 * 1000 * 1000};
	nanosleep(&frame, NULL);
}

static bool store_path(device_state* st, const char* name, char* buf, size_t cap) {
	if (!name || !*name || strchr(name, '/') || strchr(name, '\\') || strcmp(name, "..") == 0)
		return false;

	const int written = snprintf(buf, cap, "%s/%s", st->store_dir, name);
	return written > 0 && (size_t)written < cap;
}

static qdos_store_result device_store_read(qdos_hal* hal, const char* name, void* buf, size_t cap, size_t* len) {
	device_state* st = (device_state*)hal->impl;

	char path[512];
	if (!store_path(st, name, path, sizeof(path)))
		return QDOS_STORE_IO_ERROR;

	FILE* f = fopen(path, "rb");
	if (!f)
		return QDOS_STORE_NOT_FOUND;

	const size_t got = fread(buf, 1, cap, f);
	const bool overflowed = (got == cap) && (fgetc(f) != EOF);
	fclose(f);

	if (overflowed)
		return QDOS_STORE_TOO_BIG;
	if (len)
		*len = got;
	return QDOS_STORE_OK;
}

static qdos_store_result device_store_write(qdos_hal* hal, const char* name, const void* buf, size_t len) {
	device_state* st = (device_state*)hal->impl;

	char path[512];
	if (!store_path(st, name, path, sizeof(path)))
		return QDOS_STORE_IO_ERROR;

	mkdir(st->store_dir, 0755);

	FILE* f = fopen(path, "wb");
	if (!f)
		return QDOS_STORE_IO_ERROR;

	const size_t written = fwrite(buf, 1, len, f);
	// Flash wears and power can vanish mid-write; fsync before declaring
	// success so a stored program is actually stored
	fflush(f);
	fsync(fileno(f));
	const bool ok = (fclose(f) == 0) && (written == len);
	return ok ? QDOS_STORE_OK : QDOS_STORE_IO_ERROR;
}

static device_state g_device;

void qdos_device_hal(qdos_hal* hal) {
	memset(&g_device, 0, sizeof(g_device));
	g_device.fb_fd = -1;
	g_device.input_fd = -1;

	hal->init = device_init;
	hal->shutdown = device_shutdown;
	hal->present = device_present;
	hal->poll_key = device_poll_key;
	hal->running = device_running;
	hal->idle = device_idle;
	hal->store_read = device_store_read;
	hal->store_write = device_store_write;
	hal->impl = &g_device;
}
