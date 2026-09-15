/**
 * @file test_mathwords.c
 * @brief The maths words, and the domains lib/math would otherwise abort on
 */

#include "check.h"

#include "../src/shell/mathwords.h"
#include "../src/ui/console.h"

#include <math.h>
#include <stdlib.h>

static qd_interp* fresh(void) {
	qd_interp* interp = qd_interp_create(4096);
	qdos_register_math(interp);
	return interp;
}

/** Evaluate and return the top of the stack, or NAN if it did not run. */
static double value_of(const char* source, bool* ok) {
	qd_interp* interp = fresh();
	*ok = qd_interp_eval(interp, source);

	double out = NAN;
	qd_interp_value value;
	if (*ok && qd_interp_peek(interp, 0, &value))
		out = (value.type == QD_INTERP_VALUE_FLOAT) ? value.f : (double)value.i;

	qd_interp_destroy(interp);
	return out;
}

static bool refuses(const char* source) {
	bool ok = false;
	value_of(source, &ok);
	return !ok;
}

static void close_to(const char* source, double want) {
	bool ok = false;
	const double got = value_of(source, &ok);
	CHECK(ok);
	CHECK(fabs(got - want) < 1e-9);
}

/**
 * Out of domain is an error, not an abort. lib/math calls a domain error fatal,
 * which would take the calculator down mid-sum.
 */
static void test_domains_are_errors(void) {
	static const char* const OUTSIDE[] = {
			"-1 sqrt", "0 ln", "-1 ln", "0 log10", "-2 log10", "0 log2", "-2 log2",
			"2 asin", "-2 asin", "2 acos", "-2 acos", "0 acosh", "1 atanh", "-1 atanh",
			"0 inv", "-1 fac", "1000000 fac", "5 0 fmod",
	};
	for (size_t i = 0; i < sizeof(OUTSIDE) / sizeof(*OUTSIDE); i++)
		CHECK(refuses(OUTSIDE[i]));
}

/** Inside the domain nothing changed: lib/math still computes, integers included. */
static void test_inside_the_domain(void) {
	close_to("9 sqrt", 3.0);
	close_to("100 log10", 2.0);
	close_to("8 log2", 3.0);
	close_to("1 ln", 0.0);
	close_to("0 asin", 0.0);
	close_to("1 acosh", 0.0);
	close_to("0 atanh", 0.0);
	close_to("4 inv", 0.25);
	close_to("5 fac", 120.0);
	close_to("7 2 fmod", 1.0);
	close_to("2 10 pow", 1024.0);
	close_to("3 4 hypot", 5.0);
}

/**
 * The words with no domain to fall out of. They are thin wrappers over
 * lib/math, but a wrapper is where a name gets attached to the wrong function,
 * and the cap on the key is the only other place that is written down.
 */
static void test_plain_words(void) {
	close_to("1 sinh", 1.1752011936438014);
	close_to("0 cosh", 1.0);
	close_to("0 tanh", 0.0);
	close_to("0 asinh", 0.0);
	close_to("7 sq", 49.0);
	close_to("3 cb", 27.0);
	close_to("27 cbrt", 3.0);
	close_to("0 exp", 1.0);
	close_to("10 exp2", 1024.0);

	// Rounding, which differs in all four directions
	close_to("1.2 ceil", 2.0);
	close_to("1.8 floor", 1.0);
	close_to("1.8 round", 2.0);
	close_to("1.2 round", 1.0);
	close_to("1.8 trunc", 1.0);
	close_to("1.8 neg trunc", -1.0); // towards zero, not down
	close_to("1.8 neg floor", -2.0); // down, not towards zero

	close_to("5 neg abs", 5.0);
	close_to("5 abs", 5.0);

	close_to("3 7 min", 3.0);
	close_to("3 7 max", 7.0);
	close_to("7 3 min", 3.0); // and the order of the two does not matter
	close_to("7 3 max", 7.0);
}

/** atan2 keeps the quadrant that atan throws away, so the order is the point. */
static void test_atan2_argument_order(void) {
	close_to("1 1 atan2", atan2(1.0, 1.0));
	close_to("1 0 atan2", atan2(1.0, 0.0));
	close_to("0 1 atan2", atan2(0.0, 1.0));
	close_to("1 neg 1 neg atan2", atan2(-1.0, -1.0));
}

/** The reported crash: ln four times over drives the value negative. */
static void test_repeated_ln_does_not_abort(void) {
	qd_interp* interp = fresh();
	CHECK(qd_interp_eval(interp, "5"));

	bool refused = false;
	for (int i = 0; i < 20; i++) {
		if (!qd_interp_eval(interp, "ln"))
			refused = true;
	}
	CHECK(refused); // it stopped rather than carrying on into nonsense

	// The value it refused to take the log of is still there
	CHECK(qd_interp_depth(interp) == 1);
	qd_interp_destroy(interp);
}

/** A refused word leaves the stack as it was, so nothing is lost. */
static void test_refusal_keeps_the_value(void) {
	qd_interp* interp = fresh();
	CHECK(qd_interp_eval(interp, "7 neg"));
	CHECK(!qd_interp_eval(interp, "sqrt"));
	CHECK(qd_interp_depth(interp) == 1);

	qd_interp_value value;
	CHECK(qd_interp_peek(interp, 0, &value));
	qd_interp_destroy(interp);
}

/** Constants, so trigonometry does not need fifteen digits typed by hand. */
static void test_constants(void) {
	close_to("pi", 3.14159265358979323846);
	close_to("e", 2.71828182845904523536);
	close_to("pi 2 divide sin", 1.0);
}

/** Degrees, because a calculator is asked for sin 30 far more often. */
static void test_degree_mode(void) {
	qdos_math_set_degrees(true);
	close_to("30 sin", 0.5);
	close_to("180 cos", -1.0);
	close_to("45 tan", 1.0);
	close_to("1 asin", 90.0);
	close_to("1 acos", 0.0);
	close_to("1 atan", 45.0);
	qdos_math_set_degrees(false);

	close_to("0 sin", 0.0); // radians again
	close_to("0 tan", 0.0);
	close_to("1 atan", atan(1.0));
	CHECK(!qdos_math_degrees());
}

/** True division, which the calculator key uses in place of `/`. */
static void test_divide(void) {
	close_to("22 7 divide 100 *", 22.0 / 7.0 * 100.0);
	close_to("10 4 divide", 2.5);
	close_to("10 5 divide", 2.0);
	CHECK(refuses("1 0 divide"));
}

/**
 * A message the panel cannot show in full is not a message. Only QDOS's own
 * are checked: the runtime's arity errors are longer than 25 columns and are
 * left as written, being Quadrate's words rather than ours.
 */
static void test_messages_fit_the_screen(void) {
	static const char* const REFUSED[] = {
			"-1 sqrt", "0 ln", "0 log10", "0 log2", "2 asin", "2 acos",
			"0 acosh", "1 atanh", "0 inv", "-1 fac", "5 0 fmod", "1 0 divide",
	};

	for (size_t i = 0; i < sizeof(REFUSED) / sizeof(*REFUSED); i++) {
		qd_interp* interp = fresh();
		CHECK(!qd_interp_eval(interp, REFUSED[i]));

		const char* text = qd_interp_error(interp);
		if (strlen(text) > (size_t)QDOS_COLS)
			fprintf(stderr, "  too long (%zu): %s\n", strlen(text), text);
		CHECK(strlen(text) <= (size_t)QDOS_COLS);
		qd_interp_destroy(interp);
	}
}

int main(void) {
	test_domains_are_errors();
	test_plain_words();
	test_atan2_argument_order();
	test_constants();
	test_degree_mode();
	test_divide();
	test_messages_fit_the_screen();
	test_inside_the_domain();
	test_repeated_ln_does_not_abort();
	test_refusal_keeps_the_value();
	return check_report("mathwords");
}
