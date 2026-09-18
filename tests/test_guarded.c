/**
 * @file test_guarded.c
 * @brief Guarded evaluation, and the stdout it borrows to capture output
 */

// POSIX interfaces on top of a strict c11 build
#define _POSIX_C_SOURCE 200809L

#include "check.h"

#include "../src/shell/guarded.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

static qd_interp* fresh(void) {
	return qd_interp_create(4096);
}

/** How many descriptors this process holds, so a leak has somewhere to show */
static int open_fds(void) {
	DIR* dir = opendir("/proc/self/fd");
	if (!dir) {
		return -1;
	}

	int count = 0;
	const struct dirent* ent;
	while ((ent = readdir(dir)) != NULL) {
		if (ent->d_name[0] != '.') {
			count++;
		}
	}
	closedir(dir);
	return count;
}

/** Identify whatever stdout currently points at */
static bool stdout_identity(dev_t* dev, ino_t* ino) {
	struct stat sb;
	if (fstat(STDOUT_FILENO, &sb) != 0) {
		return false;
	}
	*dev = sb.st_dev;
	*ino = sb.st_ino;
	return true;
}

/** What a program prints comes back, rather than going to the real stdout. */
static void test_output_is_captured(void) {
	qd_interp* interp = fresh();

	CHECK(qdos_guarded_eval(interp, "\"HELLO\" print"));
	CHECK(strstr(qdos_guarded_output(), "HELLO") != NULL);

	// A later evaluation that prints nothing reports nothing, rather than
	// repeating what the one before it printed
	CHECK(qdos_guarded_eval(interp, "1 2 +"));
	CHECK(qdos_guarded_output()[0] == '\0');

	qd_interp_destroy(interp);
}

/** A refused evaluation still hands stdout back and still explains itself. */
static void test_a_refusal_is_not_fatal(void) {
	qd_interp* interp = fresh();

	CHECK(!qdos_guarded_eval(interp, "nosuchword"));
	CHECK(qdos_guarded_error(interp)[0] != '\0');

	CHECK(qdos_guarded_eval(interp, "1 2 +")); // and it still evaluates
	CHECK(qd_interp_depth(interp) == 1);

	qd_interp_destroy(interp);
}

/**
 * Evaluation borrows stdout and must give it back. Nothing on the device reads
 * it, but leaving it on a pipe nobody drains means QDOS's own diagnostics go
 * into a buffer that eventually fills and blocks.
 */
static void test_stdout_comes_back(void) {
	dev_t dev_before = 0, dev_after = 0;
	ino_t ino_before = 0, ino_after = 0;
	CHECK(stdout_identity(&dev_before, &ino_before));

	qd_interp* interp = fresh();
	CHECK(qdos_guarded_eval(interp, "\"BORROWED\" print"));
	qd_interp_destroy(interp);

	CHECK(stdout_identity(&dev_after, &ino_after));
	CHECK(dev_before == dev_after);
	CHECK(ino_before == ino_after);
}

/** And it gives back every descriptor it took. */
static void test_no_descriptors_are_leaked(void) {
	qd_interp* interp = fresh();

	// Once first, so nothing that only happens on the first call is counted
	CHECK(qdos_guarded_eval(interp, "\"WARM\" print"));

	const int before = open_fds();
	CHECK(before > 0);

	for (int i = 0; i < 20; i++) {
		CHECK(qdos_guarded_eval(interp, "\"AGAIN\" print"));
	}

	CHECK(open_fds() == before);
	qd_interp_destroy(interp);
}

/* ---------------------------------------------------------------------- */
/* Nesting                                                               */
/*                                                                        */
/* `forget` restores a shipped program by evaluating it, from inside the  */
/* native word, which is itself inside an evaluation. This is that shape, */
/* without the shell around it.                                          */
/* ---------------------------------------------------------------------- */

static int nested_evaluations = 0;

static int native_nests(qd_context* ctx, void* userdata) {
	(void)ctx;
	qd_interp* interp = userdata;
	nested_evaluations++;
	qdos_guarded_eval(interp, "\"INNER\" print");
	return 0;
}

static void test_a_nested_evaluation_is_contained(void) {
	dev_t dev_before = 0, dev_after = 0;
	ino_t ino_before = 0, ino_after = 0;
	CHECK(stdout_identity(&dev_before, &ino_before));

	qd_interp* interp = fresh();
	qd_interp_register(interp, "nests", "( -- )", native_nests, interp);

	nested_evaluations = 0;
	CHECK(qdos_guarded_eval(interp, "nests"));
	CHECK(nested_evaluations == 1); // the word really did run

	// Whatever was printed, the outer evaluation is the one that reports it
	CHECK(strstr(qdos_guarded_output(), "INNER") != NULL);

	// The nesting must not strand stdout on a pipe
	CHECK(stdout_identity(&dev_after, &ino_after));
	CHECK(dev_before == dev_after);
	CHECK(ino_before == ino_after);

	qd_interp_destroy(interp);
}

/** Nesting repeatedly must not leak either, since `forget` can be pressed all day. */
static void test_nesting_leaks_nothing(void) {
	qd_interp* interp = fresh();
	qd_interp_register(interp, "nests", "( -- )", native_nests, interp);

	CHECK(qdos_guarded_eval(interp, "nests")); // warm up

	const int before = open_fds();
	CHECK(before > 0);

	for (int i = 0; i < 20; i++) {
		CHECK(qdos_guarded_eval(interp, "nests"));
	}

	CHECK(open_fds() == before);
	qd_interp_destroy(interp);
}

/* ---------------------------------------------------------------------- */
/* Fatal runtime errors                                                   */
/*                                                                        */
/* Which operations are fatal is lib/rt's business and moves with it.     */
/* qd_fatal_raise() is the one door they all go through, so a word that   */
/* knocks on it directly keeps testing this when that list changes.       */
/*                                                                        */
/* Note where the recovery actually happens: qd_interp_eval() arms its    */
/* own buffer over ours before any runtime code runs, so it catches these */
/* and hands back a plain false. What these pin down is the guarantee the */
/* shell relies on -- a fatal error ends the evaluation and nothing else. */
/* ---------------------------------------------------------------------- */

static int native_boom(qd_context* ctx, void* userdata) {
	(void)userdata;
	qd_fatal_raise(ctx, "boom", "a fatal error, on purpose");
	return 0; // not reached
}

static qd_interp* explosive(void) {
	qd_interp* interp = fresh();
	qd_interp_register(interp, "boom", "( -- )", native_boom, NULL);
	return interp;
}

/** The `forget` shape again, but the program it restores is a fatal one */
static int native_nests_a_boom(qd_context* ctx, void* userdata) {
	(void)ctx;
	qd_interp* interp = userdata;
	qdos_guarded_eval(interp, "boom");
	return 0; // not reached: the inner evaluation unwinds past here
}

/** A fatal runtime error ends the evaluation, not the calculator. */
static void test_a_fatal_error_is_survivable(void) {
	qd_interp* interp = explosive();

	CHECK(!qdos_guarded_eval(interp, "boom"));

	// It comes back as a refusal with something to show the user
	CHECK(qdos_guarded_error(interp)[0] != '\0');

	// Still here, and still a working interpreter
	CHECK(qdos_guarded_eval(interp, "1 2 +"));
	CHECK(qd_interp_depth(interp) == 1);

	qd_interp_destroy(interp);
}

/** The message says what went wrong, and is not left over from last time. */
static void test_the_message_follows_the_error(void) {
	qd_interp* interp = explosive();

	CHECK(!qdos_guarded_eval(interp, "boom"));
	const char* fatal = qdos_guarded_error(interp);
	CHECK(strstr(fatal, "on purpose") != NULL); // the raiser's own words

	CHECK(!qdos_guarded_eval(interp, "nosuchword"));
	CHECK(strstr(qdos_guarded_error(interp), "on purpose") == NULL);

	qd_interp_destroy(interp);
}

/** A fatal error must hand stdout back too, or the next print goes nowhere. */
static void test_a_fatal_error_gives_stdout_back(void) {
	dev_t dev_before = 0, dev_after = 0;
	ino_t ino_before = 0, ino_after = 0;
	CHECK(stdout_identity(&dev_before, &ino_before));

	qd_interp* interp = explosive();
	CHECK(!qdos_guarded_eval(interp, "\"BEFORE\" print boom"));

	CHECK(stdout_identity(&dev_after, &ino_after));
	CHECK(dev_before == dev_after);
	CHECK(ino_before == ino_after);

	// What it managed to print before it died is still worth showing
	CHECK(strstr(qdos_guarded_output(), "BEFORE") != NULL);

	// And the next evaluation captures its own output as usual
	CHECK(qdos_guarded_eval(interp, "\"AFTER\" print"));
	CHECK(strstr(qdos_guarded_output(), "AFTER") != NULL);
	CHECK(strstr(qdos_guarded_output(), "BEFORE") == NULL);

	qd_interp_destroy(interp);
}

/** Failing repeatedly must not leak the pipe the capture runs through. */
static void test_fatal_errors_leak_nothing(void) {
	qd_interp* interp = explosive();

	CHECK(!qdos_guarded_eval(interp, "boom")); // warm up

	const int before = open_fds();
	CHECK(before > 0);

	for (int i = 0; i < 20; i++) {
		CHECK(!qdos_guarded_eval(interp, "boom"));
	}

	CHECK(open_fds() == before);
	qd_interp_destroy(interp);
}

/**
 * A fatal error inside a nested evaluation stays inside it. `forget` ignores
 * whether the program it restores ran, so the outer evaluation carries on --
 * what matters is that it carries on with its stdout and its pipe intact.
 */
static void test_a_fatal_error_inside_a_nested_evaluation(void) {
	dev_t dev_before = 0, dev_after = 0;
	ino_t ino_before = 0, ino_after = 0;
	CHECK(stdout_identity(&dev_before, &ino_before));

	qd_interp* interp = explosive();
	qd_interp_register(interp, "nests_a_boom", "( -- )", native_nests_a_boom, interp);

	CHECK(qdos_guarded_eval(interp, "nests_a_boom")); // warm up
	const int before = open_fds();

	CHECK(qdos_guarded_eval(interp, "nests_a_boom"));

	CHECK(stdout_identity(&dev_after, &ino_after));
	CHECK(dev_before == dev_after);
	CHECK(ino_before == ino_after);
	CHECK(open_fds() == before);

	CHECK(qdos_guarded_eval(interp, "1 2 +")); // and it still evaluates
	qd_interp_destroy(interp);
}

int main(void) {
	test_output_is_captured();
	test_a_refusal_is_not_fatal();
	test_stdout_comes_back();
	test_no_descriptors_are_leaked();
	test_a_nested_evaluation_is_contained();
	test_nesting_leaks_nothing();
	test_a_fatal_error_is_survivable();
	test_the_message_follows_the_error();
	test_a_fatal_error_gives_stdout_back();
	test_fatal_errors_leak_nothing();
	test_a_fatal_error_inside_a_nested_evaluation();
	return check_report("guarded");
}
