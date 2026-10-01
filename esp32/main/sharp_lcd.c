/**
 * @file sharp_lcd.c
 * @brief The Sharp LS027B7DH01 memory LCD, over SPI
 *
 * The panel takes lines, not frames: a write is a command byte, then for each
 * line its address, its 50 bytes and a trailer, then one more trailer. Bits go
 * least significant first, a set bit is white, and lines count from one.
 *
 * VCOM has to be inverted about once a second, or the liquid crystal takes a
 * DC bias. The breakout pulls EXTMODE low, which leaves that to software: the
 * bit rides in every command, and a timer sends a command with no lines when
 * nothing else has.
 */

#include "sharp_lcd.h"

#include "sdkconfig.h"

// The whole driver, where there is a panel to drive; see CONFIG_QDOS_LCD
#if CONFIG_QDOS_LCD && !CONFIG_QDOS_LCD_QEMU

#include <qdos/hal.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdbool.h>
#include <string.h>

#define LINE_BYTES (QDOS_SCREEN_W / 8)
#define LINE_FRAME (1 + LINE_BYTES + 1) ///< Address, pixels, trailer
#define TX_MAX (1 + QDOS_SCREEN_H * LINE_FRAME + 1)

#define CMD_WRITE 0x01
#define CMD_VCOM 0x02
#define CMD_CLEAR 0x04

/** Grey below this is ink, as the simulator and the kernel's driver take it */
#define INK_BELOW 128

static const char* TAG = "lcd";

static spi_device_handle_t g_dev;
static SemaphoreHandle_t g_lock; ///< The timer and the shell both send
static esp_timer_handle_t g_vcom_timer;
static uint8_t* g_tx;			 ///< DMA-capable, so internal RAM
static uint8_t g_shown[QDOS_SCREEN_H][LINE_BYTES];
static bool g_vcom;

static void send(const uint8_t* buf, size_t len) {
	spi_transaction_t t = {
			.length = len * 8,
			.tx_buffer = buf,
	};
	const esp_err_t err = spi_device_transmit(g_dev, &t);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "transmit: %s", esp_err_to_name(err));
	}
}

static uint8_t vcom_bit(void) {
	return g_vcom ? CMD_VCOM : 0;
}

static void vcom_tick(void* arg) {
	(void)arg;
	xSemaphoreTake(g_lock, portMAX_DELAY);
	g_vcom = !g_vcom;
	g_tx[0] = vcom_bit();
	g_tx[1] = 0;
	send(g_tx, 2);
	xSemaphoreGive(g_lock);
}

int qdos_lcd_init(void) {
	if (CONFIG_QDOS_LCD_DISP >= 0) {
		gpio_reset_pin(CONFIG_QDOS_LCD_DISP);
		gpio_set_direction(CONFIG_QDOS_LCD_DISP, GPIO_MODE_OUTPUT);
		gpio_set_level(CONFIG_QDOS_LCD_DISP, 1);
	}

	const spi_bus_config_t bus = {
			.mosi_io_num = CONFIG_QDOS_LCD_MOSI,
			.miso_io_num = -1, // write-only: the panel has no output
			.sclk_io_num = CONFIG_QDOS_LCD_SCLK,
			.quadwp_io_num = -1,
			.quadhd_io_num = -1,
			.max_transfer_sz = TX_MAX,
	};
	esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "bus: %s", esp_err_to_name(err));
		return 1;
	}

	// Chip select is active high, unlike every other SPI device, and wants
	// 3 us of setup and 1 us of hold: 8 and 4 cycles at 2 MHz, with margin.
	// Those only apply to half-duplex, which a write-only device is anyway.
	const spi_device_interface_config_t dev = {
			.mode = 0,
			.clock_speed_hz = CONFIG_QDOS_LCD_HZ,
			.spics_io_num = CONFIG_QDOS_LCD_CS,
			.flags = SPI_DEVICE_TXBIT_LSBFIRST | SPI_DEVICE_POSITIVE_CS | SPI_DEVICE_HALFDUPLEX,
			.queue_size = 1,
			.cs_ena_pretrans = 8,
			.cs_ena_posttrans = 4,
	};
	err = spi_bus_add_device(SPI2_HOST, &dev, &g_dev);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "device: %s", esp_err_to_name(err));
		return 1;
	}

	g_tx = heap_caps_malloc(TX_MAX, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
	g_lock = xSemaphoreCreateMutex();
	if (g_tx == NULL || g_lock == NULL) {
		return 1;
	}

	// Start from a known glass, so the first frame can be sent by difference
	g_tx[0] = CMD_CLEAR;
	g_tx[1] = 0;
	send(g_tx, 2);
	memset(g_shown, 0xFF, sizeof(g_shown));

	const esp_timer_create_args_t timer = {
			.callback = vcom_tick,
			.name = "vcom",
	};
	if (esp_timer_create(&timer, &g_vcom_timer) != ESP_OK ||
			esp_timer_start_periodic(g_vcom_timer, 1000 * 1000) != ESP_OK) {
		return 1;
	}
	return 0;
}

void qdos_lcd_present(const uint8_t* gray) {
	xSemaphoreTake(g_lock, portMAX_DELAY);

	size_t n = 0;
	g_tx[n++] = CMD_WRITE | vcom_bit();

	for (int y = 0; y < QDOS_SCREEN_H; y++) {
		uint8_t line[LINE_BYTES];
		memset(line, 0xFF, sizeof(line));
		const uint8_t* row = gray + (size_t)y * QDOS_SCREEN_W;
		for (int x = 0; x < QDOS_SCREEN_W; x++) {
			if (row[x] < INK_BELOW) {
				line[x / 8] &= (uint8_t)~(1u << (x % 8));
			}
		}

		if (memcmp(line, g_shown[y], LINE_BYTES) == 0) {
			continue;
		}
		memcpy(g_shown[y], line, LINE_BYTES);

		g_tx[n++] = (uint8_t)(y + 1);
		memcpy(&g_tx[n], line, LINE_BYTES);
		n += LINE_BYTES;
		g_tx[n++] = 0;
	}

	if (n > 1) {
		g_tx[n++] = 0;
		send(g_tx, n);
	}
	xSemaphoreGive(g_lock);
}

void qdos_lcd_stop(void) {
	if (g_vcom_timer != NULL) {
		esp_timer_stop(g_vcom_timer);
	}
}

#endif // CONFIG_QDOS_LCD && !CONFIG_QDOS_LCD_QEMU
