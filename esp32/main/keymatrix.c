/**
 * @file keymatrix.c
 * @brief The keypad, read as a matrix of GPIOs
 *
 * Rows are driven open-drain, columns read through pull-ups, so a key down
 * pulls its column low while its row is. The rows and the positions in them
 * are the pad's own (src/hal/sim/keypad_ui.c): wire row r, key c to row pin r
 * and column pin c and the legends are right. A five-key row leaves its sixth
 * column unwired.
 *
 * Without a diode per key, three keys down at the corners of a rectangle make
 * a fourth appear. Nobody presses three calculator keys at once, but the
 * diodes are cheap.
 */

#include "keymatrix.h"

#include "input.h"

#include "hal/sim/keypad_ui.h"

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_sleep.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/** A key has to read the same this many scans running to count */
#define DEBOUNCE_SCANS 2
#define SCAN_MS 5
/** Scans with nothing down before the matrix goes back to waiting */
#define IDLE_SCANS 4
/** For the row just driven to reach its columns */
#define SETTLE_US 5

static const char* TAG = "keys";

static int g_rows[QDOS_PAD_ROWS];
static int g_cols[QDOS_PAD_COLS];
static int g_row_count;
static int g_col_count;
static QueueHandle_t g_out;
static TaskHandle_t g_task;

/** "8,9,10" into pins; returns how many */
static int parse_pins(const char* list, int* pins, int cap) {
	int n = 0;
	const char* p = list;
	while (*p != '\0' && n < cap) {
		char* end = NULL;
		const long pin = strtol(p, &end, 10);
		if (end == p) {
			break;
		}
		pins[n++] = (int)pin;
		p = (*end == ',') ? end + 1 : end;
	}
	return n;
}

static void drive_all_rows(int level) {
	for (int r = 0; r < g_row_count; r++) {
		gpio_set_level(g_rows[r], level);
	}
}

static void columns_interrupt(bool on) {
	for (int c = 0; c < g_col_count; c++) {
		if (on) {
			gpio_intr_enable(g_cols[c]);
		} else {
			gpio_intr_disable(g_cols[c]);
		}
	}
}

static void IRAM_ATTR on_column(void* arg) {
	(void)arg;
	// Level-triggered, so it would fire again at once: off until the scan ends
	for (int c = 0; c < g_col_count; c++) {
		gpio_intr_disable(g_cols[c]);
	}
	BaseType_t woken = pdFALSE;
	vTaskNotifyGiveFromISR(g_task, &woken);
	portYIELD_FROM_ISR(woken);
}

/** Whether the key in column @p c of the row being driven is down */
static bool raw_down(int c) {
	return gpio_get_level(g_cols[c]) == 0;
}

/** One pass over every row; reports whether anything at all was down */
static bool scan_once(bool down[QDOS_PAD_ROWS][QDOS_PAD_COLS], uint8_t streak[QDOS_PAD_ROWS][QDOS_PAD_COLS]) {
	bool any = false;
	for (int r = 0; r < g_row_count; r++) {
		gpio_set_level(g_rows[r], 0);
		esp_rom_delay_us(SETTLE_US);

		for (int c = 0; c < g_col_count; c++) {
			const bool now = raw_down(c);
			any = any || now || down[r][c];

			if (now == down[r][c]) {
				streak[r][c] = 0;
				continue;
			}
			if (++streak[r][c] < DEBOUNCE_SCANS) {
				continue;
			}

			streak[r][c] = 0;
			down[r][c] = now;
			if (now) {
				const qdos_input in = {.pad = true, .row = (uint8_t)r, .col = (uint8_t)c};
				xQueueSend(g_out, &in, 0);
			}
		}

		gpio_set_level(g_rows[r], 1);
	}
	return any;
}

static void scan_task(void* arg) {
	(void)arg;
	for (;;) {
		ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

		bool down[QDOS_PAD_ROWS][QDOS_PAD_COLS];
		uint8_t streak[QDOS_PAD_ROWS][QDOS_PAD_COLS];
		memset(down, 0, sizeof(down));
		memset(streak, 0, sizeof(streak));

		drive_all_rows(1);
		int idle = 0;
		while (idle < IDLE_SCANS) {
			idle = scan_once(down, streak) ? 0 : idle + 1;
			vTaskDelay(pdMS_TO_TICKS(SCAN_MS));
		}

		// Back to waiting: every row low, so any key pulls a column down
		drive_all_rows(0);
		columns_interrupt(true);
	}
}

int qdos_matrix_init(QueueHandle_t out) {
	g_out = out;
	g_row_count = parse_pins(CONFIG_QDOS_KEY_ROW_PINS, g_rows, QDOS_PAD_ROWS);
	g_col_count = parse_pins(CONFIG_QDOS_KEY_COL_PINS, g_cols, QDOS_PAD_COLS);
	if (g_row_count == 0 || g_col_count == 0) {
		ESP_LOGW(TAG, "no keypad pins configured");
		return 1;
	}

	// Coming back from deep sleep, the rows are still held where sleep left them
	gpio_deep_sleep_hold_dis();
	for (int r = 0; r < g_row_count; r++) {
		gpio_hold_dis(g_rows[r]);
	}
	for (int c = 0; c < g_col_count; c++) {
		if (rtc_gpio_is_valid_gpio(g_cols[c])) {
			rtc_gpio_deinit(g_cols[c]);
		}
	}

	uint64_t row_mask = 0;
	for (int r = 0; r < g_row_count; r++) {
		row_mask |= 1ULL << g_rows[r];
	}
	const gpio_config_t rows = {
			.pin_bit_mask = row_mask,
			.mode = GPIO_MODE_OUTPUT_OD,
	};
	gpio_config(&rows);
	drive_all_rows(0);

	uint64_t col_mask = 0;
	for (int c = 0; c < g_col_count; c++) {
		col_mask |= 1ULL << g_cols[c];
	}
	const gpio_config_t cols = {
			.pin_bit_mask = col_mask,
			.mode = GPIO_MODE_INPUT,
			.pull_up_en = GPIO_PULLUP_ENABLE,
			.intr_type = GPIO_INTR_LOW_LEVEL,
	};
	gpio_config(&cols);

	if (xTaskCreatePinnedToCore(scan_task, "keys", 3072, NULL, 10, &g_task, 0) != pdPASS) {
		return 1;
	}

	esp_err_t err = gpio_install_isr_service(0);
	if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
		ESP_LOGE(TAG, "isr service: %s", esp_err_to_name(err));
		return 1;
	}
	for (int c = 0; c < g_col_count; c++) {
		gpio_isr_handler_add(g_cols[c], on_column, NULL);
	}

	ESP_LOGI(TAG, "%d rows, %d columns", g_row_count, g_col_count);
	return 0;
}

void qdos_matrix_arm_wake(void) {
	if (g_row_count == 0 || g_col_count == 0) {
		return;
	}

	// Let go of the pad first, for up to two seconds
	columns_interrupt(false);
	drive_all_rows(0);
	for (int waited = 0; waited < 2000; waited += SCAN_MS) {
		bool any = false;
		for (int c = 0; c < g_col_count; c++) {
			any = any || gpio_get_level(g_cols[c]) == 0;
		}
		if (!any) {
			break;
		}
		vTaskDelay(pdMS_TO_TICKS(SCAN_MS));
	}

	// Rows held low through sleep, columns pulled up by the RTC domain, and a
	// column going low is the wake
	for (int r = 0; r < g_row_count; r++) {
		gpio_hold_en(g_rows[r]);
	}
	gpio_deep_sleep_hold_en();

	uint64_t wake = 0;
	for (int c = 0; c < g_col_count; c++) {
		if (!rtc_gpio_is_valid_gpio(g_cols[c])) {
			ESP_LOGW(TAG, "GPIO %d cannot wake the chip", g_cols[c]);
			continue;
		}
		rtc_gpio_init(g_cols[c]);
		rtc_gpio_set_direction(g_cols[c], RTC_GPIO_MODE_INPUT_ONLY);
		rtc_gpio_pulldown_dis(g_cols[c]);
		rtc_gpio_pullup_en(g_cols[c]);
		wake |= 1ULL << g_cols[c];
	}
	if (wake != 0) {
		esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
		esp_sleep_enable_ext1_wakeup_io(wake, ESP_EXT1_WAKEUP_ANY_LOW);
	}
}
