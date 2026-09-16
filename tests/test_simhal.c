/**
 * @file test_simhal.c
 * @brief The simulator backend's own lifecycle
 *
 * Mostly for shutdown: a card watcher that cannot be told to stop leaves the
 * process alive with its window gone, and SDL turns Ctrl-C into an event.
 */

#include "check.h"

#include "hal/sim/sim_sdl3.h"

#include <qdos/hal.h>

#include <string.h>

/** Starting and stopping returns, rather than hanging on the card watcher. */
static void test_shutdown_returns(void) {
	qdos_hal hal;
	memset(&hal, 0, sizeof(hal));
	qdos_sim_hal(&hal);

	CHECK(hal.init(&hal) == 0);

	// Hangs rather than failing if the watcher cannot be woken
	hal.shutdown(&hal);
	CHECK(1);
}

/** And again, because a backend that only starts once is not much of one. */
static void test_it_can_be_started_twice(void) {
	qdos_hal hal;

	for (int round = 0; round < 2; round++) {
		memset(&hal, 0, sizeof(hal));
		qdos_sim_hal(&hal);
		CHECK(hal.init(&hal) == 0);
		hal.shutdown(&hal);
	}
}

/** The simulator offers a card to share and a watch on it. */
static void test_it_offers_the_card(void) {
	qdos_hal hal;
	memset(&hal, 0, sizeof(hal));
	qdos_sim_hal(&hal);
	CHECK(hal.init(&hal) == 0);

	CHECK(hal.usb_export != NULL);
	CHECK(hal.store_changed != NULL);

	// Nothing has happened to it yet
	CHECK(!hal.store_changed(&hal));

	// The inbox reads as empty, as the unmounted partition does
	CHECK(hal.usb_export(&hal, true) == 0);
	char path[512];
	CHECK(!hal.store_path(&hal, QDOS_SCOPE_INBOX, "anything.qd", path, sizeof(path)));

	CHECK(hal.usb_export(&hal, false) == 0);
	CHECK(hal.store_path(&hal, QDOS_SCOPE_INBOX, "anything.qd", path, sizeof(path)));

	hal.shutdown(&hal);
}

int main(void) {
	test_shutdown_returns();
	test_it_can_be_started_twice();
	test_it_offers_the_card();
	return check_report("simhal");
}
