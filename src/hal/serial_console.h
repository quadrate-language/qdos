/**
 * @file serial_console.h
 * @brief The calculator's serial console: a terminal as a keyboard, and uploads
 *
 * What comes in over the USB port, taken apart into keys and inbox commands.
 * tools/qdosctl is the other end. The device runs it over its USB serial unit
 * and the simulator over a pseudo-terminal, so qdosctl talks to either the
 * same way; only the four callbacks differ.
 */

#ifndef QDOS_SERIAL_CONSOLE_H
#define QDOS_SERIAL_CONSOLE_H

#include <qdos/hal.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	/**
	 * @brief Up to @p cap bytes into @p buf
	 *
	 * How many came, 0 once @p timeout_ms has passed with none, or -1 when the
	 * port has gone for good. A negative @p timeout_ms waits until something
	 * comes.
	 */
	int (*read)(void* user, uint8_t* buf, size_t cap, int timeout_ms);

	/** @brief Bytes back to the host */
	void (*write)(void* user, const char* text, size_t len);

	/** @brief A key the terminal sent, for the shell to have */
	void (*key)(void* user, const qdos_key_event* ev);

	/** @brief Something in the inbox changed, for the shell to read again */
	void (*touched)(void* user);

	const char* inbox; ///< The directory the inbox scope is read from
	void* user;		   ///< Handed to every callback

	bool after_cr; ///< The last byte was a CR, so an LF after it is the same Enter
} qdos_serial;

/**
 * @brief Read and act on one keypress, escape sequence or upload command
 * @return false once the port has gone
 */
bool qdos_serial_step(qdos_serial* s);

/** @brief CRC-32 as zlib computes it, continued from @p crc (0 to start) */
uint32_t qdos_serial_crc32(uint32_t crc, const uint8_t* buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif // QDOS_SERIAL_CONSOLE_H
