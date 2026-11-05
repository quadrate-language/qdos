/**
 * @file serial_keys.c
 * @brief The serial console on the USB port: keys and uploads
 *
 * Only the port is here. What its bytes mean -- keys, escape sequences and the
 * upload protocol qdosctl speaks -- is src/hal/serial_console.c, which the
 * simulator serves over a pseudo-terminal too.
 */

#include "serial_keys.h"

#include "hal_esp32.h"
#include "input.h"

#include "hal/serial_console.h"

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

#include <stdio.h>

static const char* TAG = "serial";
static QueueHandle_t g_out;

/** Up to @p cap bytes; 0 if none came within @p timeout_ms, which may be forever */
static int port_read(void* user, uint8_t* buf, size_t cap, int timeout_ms) {
	(void)user;
	const TickType_t ticks = timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
	const int got = usb_serial_jtag_read_bytes(buf, (uint32_t)cap, ticks);
#else
	const int got = uart_read_bytes(CONFIG_ESP_CONSOLE_UART_NUM, buf, (uint32_t)cap, ticks);
#endif
	return got > 0 ? got : 0;
}

/** Through stdout, which the console's driver owns, so it interleaves with the log */
static void port_write(void* user, const char* text, size_t len) {
	(void)user;
	fwrite(text, 1, len, stdout);
	fflush(stdout);
}

static void port_key(void* user, const qdos_key_event* ev) {
	(void)user;
	const qdos_input in = {.pad = false, .key = *ev};
	xQueueSend(g_out, &in, portMAX_DELAY);
}

static void port_touched(void* user) {
	(void)user;
	qdos_esp32_store_touched();
}

static void serial_task(void* arg) {
	(void)arg;
	qdos_serial console = {
			.read = port_read,
			.write = port_write,
			.key = port_key,
			.touched = port_touched,
			.inbox = QDOS_ESP32_INBOX,
	};
	for (;;) {
		qdos_serial_step(&console);
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

	// The upload's buffers are on this stack: a 512-byte chunk and its paths
	return xTaskCreatePinnedToCore(serial_task, "serial", 8192, NULL, 5, NULL, 0) == pdPASS ? 0 : 1;
}
