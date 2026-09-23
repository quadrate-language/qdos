/**
 * @file wallclock.c
 * @brief The time of day, for backends that run on Linux
 */

// localtime_r on top of a strict c11 build
#define _POSIX_C_SOURCE 200809L

#include "wallclock.h"

#include <time.h>

/*
 * Earlier than this and nothing has set the clock. A Pi with no RTC and no
 * network boots at 1970; it cannot tell that from a real time, only from one
 * that is impossible.
 */
#define CLOCK_SET_YEAR 2025

bool qdos_wallclock(int* seconds) {
	const time_t now = time(NULL);
	struct tm local;
	if (now == (time_t)-1 || localtime_r(&now, &local) == NULL || local.tm_year + 1900 < CLOCK_SET_YEAR) {
		return false;
	}

	*seconds = local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec;
	return true;
}
