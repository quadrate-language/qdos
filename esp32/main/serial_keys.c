/**
 * @file serial_keys.c
 * @brief A terminal on the USB port, as a keyboard
 *
 * Bytes as a VT100-style terminal sends them: printable characters go through
 * the same mapping as the simulator's keyboard, and the escape sequences for
 * the arrows and function keys become the keys the simulator gives them -- F1
 * to F5 the soft row, F10 power, Escape on its own ESC.
 */

#include "serial_keys.h"

#include "input.h"

#include "hal/sim/keypad_ui.h"

#include "esp_log.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#else
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#endif

#include <stdbool.h>
#include <string.h>

#define ESC 0x1B
/** How long after an Escape the rest of a sequence may take to arrive */
#define SEQUENCE_MS 30

static const char* TAG = "serial";
static QueueHandle_t g_out;

/** One byte, or -1 if none came within @p timeout_ms; negative waits forever */
static int read_byte(int timeout_ms) {
	const TickType_t ticks = timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
	uint8_t b;
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
	const int got = usb_serial_jtag_read_bytes(&b, 1, ticks);
#else
	const int got = uart_read_bytes(CONFIG_ESP_CONSOLE_UART_NUM, &b, 1, ticks);
#endif
	return got == 1 ? b : -1;
}

static void send_key(qdos_key key) {
	const qdos_input in = {.pad = false, .key = {.key = key, .ch = 0}};
	xQueueSend(g_out, &in, portMAX_DELAY);
}

/** What follows ESC [ or ESC O, up to its final byte */
static void sequence(char intro) {
	char body[8];
	size_t n = 0;
	for (;;) {
		const int b = read_byte(SEQUENCE_MS);
		if (b < 0) {
			return;
		}
		if (n + 1 < sizeof(body)) {
			body[n++] = (char)b;
		}
		if (b >= 0x40 && b <= 0x7E) {
			break; // the final byte
		}
	}
	body[n] = '\0';

	static const struct {
		char intro;
		const char* body;
		qdos_key key;
	} MAP[] = {
			{'[', "A", QDOS_KEY_UP},
			{'[', "B", QDOS_KEY_DOWN},
			{'[', "C", QDOS_KEY_RIGHT},
			{'[', "D", QDOS_KEY_LEFT},
			{'O', "A", QDOS_KEY_UP},
			{'O', "B", QDOS_KEY_DOWN},
			{'O', "C", QDOS_KEY_RIGHT},
			{'O', "D", QDOS_KEY_LEFT},
			{'O', "P", QDOS_KEY_SOFT1},
			{'O', "Q", QDOS_KEY_SOFT2},
			{'O', "R", QDOS_KEY_SOFT3},
			{'O', "S", QDOS_KEY_SOFT4},
			{'[', "11~", QDOS_KEY_SOFT1},
			{'[', "12~", QDOS_KEY_SOFT2},
			{'[', "13~", QDOS_KEY_SOFT3},
			{'[', "14~", QDOS_KEY_SOFT4},
			{'[', "15~", QDOS_KEY_SOFT5},
			{'[', "21~", QDOS_KEY_POWER},
			{'[', "3~", QDOS_KEY_BACKSPACE},
	};
	for (size_t i = 0; i < sizeof(MAP) / sizeof(MAP[0]); i++) {
		if (MAP[i].intro == intro && strcmp(MAP[i].body, body) == 0) {
			send_key(MAP[i].key);
			return;
		}
	}
}

static void serial_task(void* arg) {
	(void)arg;
	bool after_cr = false;

	for (;;) {
		const int b = read_byte(-1);
		if (b < 0) {
			continue;
		}

		// A terminal's Enter is CR, CR LF or LF: one press whichever it is
		const bool was_cr = after_cr;
		after_cr = (b == '\r');
		if (b == '\n' && was_cr) {
			continue;
		}

		switch (b) {
		case '\r':
		case '\n':
			send_key(QDOS_KEY_ENTER);
			break;
		case 0x7F:
		case 0x08:
			send_key(QDOS_KEY_BACKSPACE);
			break;
		case '\t':
			send_key(QDOS_KEY_TAB);
			break;
		case ESC: {
			const int next = read_byte(SEQUENCE_MS);
			if (next == '[' || next == 'O') {
				sequence((char)next);
			} else {
				send_key(QDOS_KEY_CLEAR);
			}
			break;
		}
		default:
			if (b >= 0x20 && b < 0x7F) {
				qdos_input in = {.pad = false};
				qdos_pad_typed((char)b, &in.key);
				xQueueSend(g_out, &in, portMAX_DELAY);
			}
			break;
		}
	}
}

int qdos_serial_keys_init(QueueHandle_t out) {
	g_out = out;

	// Reading takes the driver; the console's own output is then routed
	// through it too, or the two would fight over the port
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
	usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
	esp_err_t err = usb_serial_jtag_driver_install(&cfg);
	if (err == ESP_OK) {
		usb_serial_jtag_vfs_use_driver();
	}
#else
	esp_err_t err = uart_driver_install(CONFIG_ESP_CONSOLE_UART_NUM, 256, 0, 0, NULL, 0);
	if (err == ESP_OK) {
		uart_vfs_dev_use_driver(CONFIG_ESP_CONSOLE_UART_NUM);
	}
#endif
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "driver: %s", esp_err_to_name(err));
		return 1;
	}

	return xTaskCreatePinnedToCore(serial_task, "serial", 3072, NULL, 5, NULL, 0) == pdPASS ? 0 : 1;
}
