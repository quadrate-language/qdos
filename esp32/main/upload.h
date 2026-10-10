/**
 * @file upload.h
 * @brief Files onto the inbox, over the serial console
 */

#ifndef QDOS_UPLOAD_H
#define QDOS_UPLOAD_H

#include <stddef.h>
#include <stdint.h>

/** @brief Up to @p cap bytes into @p buf; how many came, or 0 after @p timeout_ms */
typedef int (*qdos_upload_read)(uint8_t* buf, size_t cap, int timeout_ms);

/** @brief Serve one command, its Ctrl-B having been read; see upload.c */
void qdos_upload_run(qdos_upload_read read);

#endif // QDOS_UPLOAD_H
