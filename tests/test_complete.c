/**
 * @file test_complete.c
 * @brief Completing a partly typed word
 */

#include "check.h"

#include "../src/shell/complete.h"

#include <string.h>

static size_t prefix_of(const char* line) {
	return qdos_complete_prefix_len(line, strlen(line));
}

static void test_prefix_boundaries(void) {
	CHECK(prefix_of("du") == 2);
	CHECK(prefix_of("1 2 swa") == 3);
	CHECK(prefix_of("{ du") == 2);
	CHECK(prefix_of("my_word") == 7);

	// Nothing to complete
	CHECK(prefix_of("") == 0);
	CHECK(prefix_of("1 2 ") == 0);
	CHECK(prefix_of("3 +") == 0);
	CHECK(prefix_of("dup ") == 0);

	// A number is not a word
	CHECK(prefix_of("42") == 0);
	CHECK(prefix_of("1 2 30") == 0);

	// ... but a word may contain digits
	CHECK(prefix_of("a1") == 2);
}

static void test_completes_a_builtin(void) {
	qd_interp* interp = qd_interp_create(256);

	qdos_completion done;
	qdos_complete(interp, "swa", &done);
	CHECK(done.matches == 1);
	CHECK(strcmp(done.common, "swap") == 0);

	qd_interp_destroy(interp);
}

static void test_completes_a_declaration(void) {
	qd_interp* interp = qd_interp_create(256);
	qd_interp_eval(interp, "fn wobble( -- r:i64) { 7 }");

	qdos_completion done;
	qdos_complete(interp, "wob", &done);
	CHECK(done.matches == 1);
	CHECK(strcmp(done.common, "wobble") == 0);

	qd_interp_destroy(interp);
}

static void test_common_prefix_of_several(void) {
	qd_interp* interp = qd_interp_create(256);
	qd_interp_eval(interp, "fn wobble( -- r:i64) { 1 }");
	qd_interp_eval(interp, "fn wobbly( -- r:i64) { 2 }");

	qdos_completion done;
	qdos_complete(interp, "wob", &done);
	CHECK(done.matches == 2);
	// Shared by both, so safe to type even though it is ambiguous
	CHECK(strcmp(done.common, "wobbl") == 0);
	CHECK(strstr(done.listing, "wobble") != NULL);
	CHECK(strstr(done.listing, "wobbly") != NULL);

	qd_interp_destroy(interp);
}

static void test_no_match(void) {
	qd_interp* interp = qd_interp_create(256);

	qdos_completion done;
	qdos_complete(interp, "zzzz", &done);
	CHECK(done.matches == 0);
	CHECK(done.common[0] == '\0');

	qd_interp_destroy(interp);
}

static void test_listing_is_bounded(void) {
	qd_interp* interp = qd_interp_create(256);

	// "d" matches several builtins; whatever the count, the listing must fit
	qdos_completion done;
	qdos_complete(interp, "d", &done);
	CHECK(done.matches > 0);
	CHECK(strlen(done.listing) < QDOS_COMPLETE_LISTING);

	qd_interp_destroy(interp);
}

int main(void) {
	test_prefix_boundaries();
	test_completes_a_builtin();
	test_completes_a_declaration();
	test_common_prefix_of_several();
	test_no_match();
	test_listing_is_bounded();
	return check_report("complete");
}
