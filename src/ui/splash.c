/**
 * @file splash.c
 * @brief What the panel shows while the machine is still starting
 */

#include "splash.h"

#include "console.h"

void qdos_splash_draw(qdos_hal* hal) {
	if (hal == NULL || hal->present == NULL) {
		return;
	}

	static qdos_console con;
	qdos_console_init(&con);

	const int middle = QDOS_ROWS / 2;

	qdos_console_puts_centered(&con, middle - 4, "QDOS", 3);
	qdos_console_rule(&con, middle + 1);
	qdos_console_puts_centered(&con, middle + 3, "starting", 1);

	hal->present(hal, con.fb);
}
