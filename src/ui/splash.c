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

	// "QDOS" is double height, so it spans two rows above the rule
	const int middle = QDOS_ROWS / 2;

	qdos_console_puts_centered(&con, middle - 3, "QDOS", 2);
	qdos_console_rule(&con, middle);
	qdos_console_puts_centered(&con, middle + 2, "starting", 1);

	hal->present(hal, con.fb);
}
