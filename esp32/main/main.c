/**
 * @file main.c
 * @brief QDOS entry point on the ESP32-S3
 */

#include "hal_esp32.h"

#include <qdos/shell.h>

#include "ui/splash.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include <string.h>

static const char* TAG = "qdos";

static void shell_task(void* arg) {
	(void)arg;

	static qdos_hal hal;
	memset(&hal, 0, sizeof(hal));
	qdos_esp32_hal(&hal);

	if (hal.init(&hal) != 0) {
		ESP_LOGE(TAG, "backend failed to start");
		vTaskDelete(NULL);
		return;
	}
	qdos_splash_draw(&hal);

	qdos_shell* shell = qdos_shell_create(&hal);
	if (shell == NULL) {
		ESP_LOGE(TAG, "out of memory");
		vTaskDelete(NULL);
		return;
	}

	ESP_LOGI(TAG, "up: %u KB internal, %u KB PSRAM free",
			(unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
			(unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

	// Returns when the machine is switched off, the session already saved
	qdos_shell_run(shell);
	qdos_shell_destroy(shell);

	qdos_esp32_power_off();
}

void app_main(void) {
	// Internal RAM, not PSRAM: the store is flash, and a task whose stack is
	// in PSRAM is not allowed to touch flash. Core 1, leaving core 0 to the
	// keypad, the console and the timers.
	const uint32_t stack = CONFIG_QDOS_SHELL_STACK_KB * 1024;
	if (xTaskCreatePinnedToCore(shell_task, "shell", stack, NULL, 5, NULL, 1) != pdPASS) {
		ESP_LOGE(TAG, "no room for a %u KB shell stack", (unsigned)(stack / 1024));
	}
}
