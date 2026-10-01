/**
 * @file hal_esp32.c
 * @brief Device backend -- the ESP32-S3
 *
 * The Sharp panel on SPI, the keypad as a GPIO matrix, a terminal on the USB
 * port beside it, and the store as files on two FAT filesystems in flash: one
 * read-only, flashed with the firmware, and one wear-levelled for everything
 * the machine writes.
 *
 * No modules (there is no dynamic linker), no USB drive (yet), no battery
 * gauge, and a clock that reads as unset until something sets it.
 */

#include "hal_esp32.h"

#include "input.h"
#include "keymatrix.h"
#include "serial_keys.h"
#include "sharp_lcd.h"

#include "hal/filestore.h"
#include "hal/sim/keypad_ui.h"
#include "hal/wallclock.h"

#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define SYSTEM_MOUNT "/system"
#define DATA_MOUNT "/data"

static const char* TAG = "hal";

typedef struct {
	qdos_filestore fs;
	QueueHandle_t input; ///< qdos_input, from the pad and the console
	qdos_pad_face face;
	const char* pending; ///< The rest of a key that types text
	wl_handle_t wl;
} esp_state;

static esp_state g_esp;

static void mount_store(esp_state* st) {
	const esp_vfs_fat_mount_config_t ro = {
			.max_files = 4,
	};
	esp_err_t err = esp_vfs_fat_spiflash_mount_ro(SYSTEM_MOUNT, "system", &ro);
	if (err != ESP_OK) {
		ESP_LOGW(TAG, "no system programs: %s", esp_err_to_name(err));
	}

	// A data partition that will not mount is a new machine, or a wrecked one;
	// either way an empty store is how to carry on
	const esp_vfs_fat_mount_config_t rw = {
			.max_files = 4,
			.format_if_mount_failed = true,
			.allocation_unit_size = 4096,
	};
	st->wl = WL_INVALID_HANDLE;
	err = esp_vfs_fat_spiflash_mount_rw_wl(DATA_MOUNT, "data", &rw, &st->wl);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "no store: %s", esp_err_to_name(err));
	}

	st->fs.system_dir = SYSTEM_MOUNT;
	st->fs.inbox_dir = DATA_MOUNT "/inbox";
	st->fs.store_dir = DATA_MOUNT "/user";
	mkdir(st->fs.inbox_dir, 0755);
	mkdir(st->fs.store_dir, 0755);
}

static int esp_init(qdos_hal* hal) {
	esp_state* st = (esp_state*)hal->impl;

	mount_store(st);

	st->input = xQueueCreate(32, sizeof(qdos_input));
	if (st->input == NULL) {
		return 1;
	}

#if CONFIG_QDOS_LCD
	if (qdos_lcd_init() != 0) {
		ESP_LOGE(TAG, "no panel");
		return 1;
	}
#endif

	// Either one is enough to use the machine, so neither is fatal
	if (qdos_matrix_init(st->input) != 0) {
		ESP_LOGW(TAG, "no keypad");
	}
	if (qdos_serial_keys_init(st->input) != 0) {
		ESP_LOGW(TAG, "no serial keyboard");
	}
	return 0;
}

static void esp_shutdown(qdos_hal* hal) {
	(void)hal;
}

static void esp_present(qdos_hal* hal, const uint8_t* fb) {
	(void)hal;
#if CONFIG_QDOS_LCD
	qdos_lcd_present(fb);
#else
	(void)fb;
#endif
}

static bool esp_poll_key(qdos_hal* hal, qdos_key_event* out) {
	esp_state* st = (esp_state*)hal->impl;

	// A text key delivers one character per poll, like typing it
	if (st->pending != NULL && *st->pending != '\0') {
		out->key = QDOS_KEY_CHAR;
		out->ch = *st->pending++;
		return true;
	}

	qdos_input in;
	while (xQueueReceive(st->input, &in, 0) == pdTRUE) {
		if (!in.pad) {
			*out = in.key;
			return true;
		}

		// A modifier only changes the face, and comes back as nothing: the
		// shell notices the face has changed and shows it
		const qdos_pad_action* a = qdos_pad_press(&st->face, qdos_pad_button_at(in.col, in.row));
		if (a == NULL) {
			continue;
		}
		if (a->key != QDOS_KEY_NONE) {
			out->key = a->key;
			out->ch = 0;
			return true;
		}
		if (a->text != NULL && *a->text != '\0') {
			st->pending = a->text;
			out->key = QDOS_KEY_CHAR;
			out->ch = *st->pending++;
			return true;
		}
	}
	return false;
}

static qdos_keypad_mod esp_modifier(qdos_hal* hal) {
	switch (((esp_state*)hal->impl)->face.layer) {
	case QDOS_PAD_ALPHA:
		return QDOS_MOD_ALPHA;
	case QDOS_PAD_SYMBOL:
		return QDOS_MOD_SYMBOL;
	default:
		return QDOS_MOD_NONE;
	}
}

static void esp_modifier_reset(qdos_hal* hal) {
	esp_state* st = (esp_state*)hal->impl;
	st->face.layer = QDOS_PAD_PLAIN;
	st->face.locked = QDOS_PAD_PLAIN;
}

static bool esp_running(qdos_hal* hal) {
	(void)hal;
	return true; // only power ends it, and the shell sees power itself
}

static uint32_t esp_ticks_ms(qdos_hal* hal) {
	(void)hal;
	return (uint32_t)(esp_timer_get_time() / 1000);
}

static void esp_wait(qdos_hal* hal, int timeout_ms) {
	esp_state* st = (esp_state*)hal->impl;
	if (st->pending != NULL && *st->pending != '\0') {
		return;
	}

	qdos_input peek;
	xQueuePeek(st->input, &peek, timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms));
}

static qdos_store_result esp_store_read(
		qdos_hal* hal, qdos_store_scope scope, const char* name, void* buf, size_t cap, size_t* len) {
	return qdos_filestore_read(&((esp_state*)hal->impl)->fs, scope, name, buf, cap, len);
}

static qdos_store_result esp_store_write(qdos_hal* hal, const char* name, const void* buf, size_t len) {
	return qdos_filestore_write(&((esp_state*)hal->impl)->fs, name, buf, len);
}

static qdos_store_result esp_store_remove(qdos_hal* hal, const char* name) {
	return qdos_filestore_remove(&((esp_state*)hal->impl)->fs, name);
}

static qdos_store_result esp_store_list(
		qdos_hal* hal, qdos_store_scope scope, const char* folder, qdos_store_visit visit, void* user) {
	return qdos_filestore_list(&((esp_state*)hal->impl)->fs, scope, folder, visit, user);
}

static bool esp_time_of_day(qdos_hal* hal, int* seconds) {
	(void)hal;
	return qdos_wallclock(seconds);
}

#if CONFIG_QDOS_SERIAL_STACK
static void esp_show_stack(qdos_hal* hal, const char* const* rows, size_t count, size_t depth) {
	(void)hal;
	static char last[QDOS_SHOWN_ROWS * 64];
	char now[sizeof(last)];
	size_t n = (size_t)snprintf(now, sizeof(now), "depth %u:", (unsigned)depth);
	for (size_t i = 0; i < count && n < sizeof(now); i++) {
		n += (size_t)snprintf(now + n, sizeof(now) - n, " [%s]", rows[i]);
	}
	if (strcmp(now, last) == 0) {
		return; // a repaint, not a change
	}
	memcpy(last, now, sizeof(last));

	const unsigned stack = CONFIG_QDOS_SHELL_STACK_KB * 1024;
	const unsigned left = (unsigned)uxTaskGetStackHighWaterMark(NULL);
	ESP_LOGI("stack", "%s  (shell stack %u/%u KB used)", now, (stack - left) / 1024, stack / 1024);
}
#endif

void qdos_esp32_power_off(void) {
	ESP_LOGI(TAG, "off");
#if CONFIG_QDOS_LCD
	qdos_lcd_stop();
#endif
	qdos_matrix_arm_wake();
	esp_deep_sleep_start();
}

void qdos_esp32_hal(qdos_hal* hal) {
	memset(&g_esp, 0, sizeof(g_esp));

	hal->init = esp_init;
	hal->shutdown = esp_shutdown;
	hal->present = esp_present;
	hal->poll_key = esp_poll_key;
	hal->modifier = esp_modifier;
	hal->modifier_reset = esp_modifier_reset;
	hal->running = esp_running;
	hal->ticks_ms = esp_ticks_ms;
	hal->wait = esp_wait;
	hal->store_read = esp_store_read;
	hal->store_write = esp_store_write;
	hal->store_remove = esp_store_remove;
	hal->store_list = esp_store_list;
	// No store_path: nothing can be opened by path without a dynamic linker
	hal->time_of_day = esp_time_of_day;
#if CONFIG_QDOS_SERIAL_STACK
	hal->show_stack = esp_show_stack;
#endif
	hal->impl = &g_esp;
}
