/**
 * @file device_linux.c
 * @brief Device backend — Linux framebuffer and evdev
 */

// POSIX interfaces (fileno, fsync, nanosleep) on top of a strict c11 build
#define _POSIX_C_SOURCE 200809L

#include "device_linux.h"

#include "framebuffer.h"
#include "keypad.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <dirent.h>
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
	int tty_fd;		 ///< The VT whose text output is suspended while we draw
	bool first_paint;
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

	const char* layout = env_or("QDOS_KEYMAP", "us");
	if (!qdos_keypad_set_layout(layout)) {
		fprintf(stderr, "qdos: unknown QDOS_KEYMAP '%s', using us\n", layout);
	}

	const char* input_path = env_or("QDOS_INPUT", "/dev/input/event0");
	st->input_fd = qdos_keypad_open(input_path);
	if (st->input_fd < 0)
		fprintf(stderr, "qdos: no keypad on %s: %s\n", input_path, strerror(errno));

	// Stop the console drawing over the calculator.
	st->tty_fd = open(env_or("QDOS_TTY", "/dev/tty1"), O_RDWR);
	if (st->tty_fd >= 0) {
		ioctl(st->tty_fd, KDSETMODE, KD_GRAPHICS);
	}

	st->first_paint = true;
	st->running = true;
	return 0;
}

static void device_shutdown(qdos_hal* hal) {
	device_state* st = (device_state*)hal->impl;
	if (!st)
		return;

	// Hand the console back, or a later login would type onto a dead screen
	if (st->tty_fd >= 0) {
		ioctl(st->tty_fd, KDSETMODE, KD_TEXT);
		close(st->tty_fd);
		st->tty_fd = -1;
	}

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

	// Clear once: the boot logo and console text are still out there.
	if (st->first_paint) {
		memset(st->fb_mem, 0, st->fb_size);
		st->first_paint = false;
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
	// A real idle would block on the input fd. Battery life depends on it.
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
	// fsync before reporting success: power can vanish mid-write.
	fflush(f);
	fsync(fileno(f));
	const bool ok = (fclose(f) == 0) && (written == len);
	return ok ? QDOS_STORE_OK : QDOS_STORE_IO_ERROR;
}

static qdos_store_result device_store_list(qdos_hal* hal, qdos_store_visit visit, void* user) {
	device_state* st = (device_state*)hal->impl;

	DIR* dir = opendir(st->store_dir);
	if (!dir)
		return QDOS_STORE_NOT_FOUND;

	const struct dirent* ent;
	while ((ent = readdir(dir)) != NULL) {
		if (ent->d_name[0] == '.')
			continue;
		if (!visit(ent->d_name, user))
			break;
	}

	closedir(dir);
	return QDOS_STORE_OK;
}

static device_state g_device;

void qdos_device_hal(qdos_hal* hal) {
	memset(&g_device, 0, sizeof(g_device));
	g_device.fb_fd = -1;
	g_device.input_fd = -1;
	g_device.tty_fd = -1;

	hal->init = device_init;
	hal->shutdown = device_shutdown;
	hal->present = device_present;
	hal->poll_key = device_poll_key;
	hal->running = device_running;
	hal->idle = device_idle;
	hal->store_read = device_store_read;
	hal->store_write = device_store_write;
	hal->store_list = device_store_list;
	hal->impl = &g_device;
}
