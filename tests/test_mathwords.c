/**
 * @file test_mathwords.c
 * @brief The maths words, and the domains lib/math would otherwise abort on
 */

#include "check.h"

#include "../src/shell/mathwords.h"
#include "../src/ui/console.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

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
	if (*ok && qd_interp_peek(interp, 0, &value)) {
		out = (value.type == QD_INTERP_VALUE_FLOAT) ? value.f : (double)value.i;
	}

	qd_interp_destroy(interp);
	return out;
}

static bool refuses(const char* source) {
	bool ok = false;
	value_of(source, &ok);
	return !ok;
}

/** Whether the top of the stack came back a whole number rather than a float */
static bool is_whole(const char* source) {
	qd_interp* interp = fresh();
	qd_interp_value value;
	const bool whole =
			qd_interp_eval(interp, source) && qd_interp_peek(interp, 0, &value) && value.type == QD_INTERP_VALUE_INT;

	qd_interp_destroy(interp);
	return whole;
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
			"-1 sqrt",
			"0 ln",
			"-1 ln",
			"0 log10",
			"-2 log10",
			"0 log2",
			"-2 log2",
			"2 asin",
			"-2 asin",
			"2 acos",
			"-2 acos",
			"0 acosh",
			"1 atanh",
			"-1 atanh",
			"0 inv",
			"-1 fac",
			"1000000 fac",
			"5 0 fmod",
	};
	for (size_t i = 0; i < sizeof(OUTSIDE) / sizeof(*OUTSIDE); i++) {
		CHECK(refuses(OUTSIDE[i]));
	}
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

/**
 * These six are written in Quadrate in lib/math rather than C, so there is no
 * usr_math_ symbol to call and the shell does them itself. A whole number in
 * is a whole number out: `3 sq` is 9 on a calculator, not 9.0, and the bitwise
 * words still take it.
 */
static void test_the_words_the_shell_does_itself(void) {
	CHECK(is_whole("7 sq"));
	CHECK(is_whole("3 cb"));
	CHECK(is_whole("5 neg abs"));
	CHECK(is_whole("3 7 min"));
	CHECK(is_whole("7 3 max"));

	CHECK(!is_whole("2.5 sq"));
	CHECK(!is_whole("2.5 neg abs"));
	CHECK(!is_whole("4 inv")); // a reciprocal is a fraction whatever went in

	close_to("2.5 sq", 6.25);
	close_to("1.5 cb", 3.375);
	close_to("2.5 neg abs", 2.5);
	close_to("4 inv", 0.25);

	// The one it picks is the one it keeps, float or not
	close_to("3 7.5 min", 3.0);
	CHECK(is_whole("3 7.5 min"));
	close_to("3 7.5 max", 7.5);
	CHECK(!is_whole("3 7.5 max"));

	CHECK(refuses("0 inv"));
	CHECK(refuses("\"x\" sq"));
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
		if (!qd_interp_eval(interp, "ln")) {
			refused = true;
		}
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
			"-1 sqrt",
			"0 ln",
			"0 log10",
			"0 log2",
			"2 asin",
			"2 acos",
			"0 acosh",
			"1 atanh",
			"0 inv",
			"-1 fac",
			"5 0 fmod",
			"1 0 divide",
	};

	for (size_t i = 0; i < sizeof(REFUSED) / sizeof(*REFUSED); i++) {
		qd_interp* interp = fresh();
		CHECK(!qd_interp_eval(interp, REFUSED[i]));

		const char* text = qd_interp_error(interp);
		if (strlen(text) > (size_t)QDOS_COLS) {
			fprintf(stderr, "  too long (%zu): %s\n", strlen(text), text);
		}
		CHECK(strlen(text) <= (size_t)QDOS_COLS);
		qd_interp_destroy(interp);
	}
}

/** An integer too big to stay one goes on as a float rather than wrapping. */
static void test_overflow_becomes_a_float(void) {
	bool ok;
	CHECK(fabs(value_of("9223372036854775807 1 plus", &ok) - 9223372036854775808.0) < 1e4);
	CHECK(!is_whole("9223372036854775807 1 plus"));
	CHECK(value_of("-9223372036854775808 1 minus", &ok) < -9.2e18);
	CHECK(fabs(value_of("4294967296 4294967296 times", &ok) - 18446744073709551616.0) < 1e5);
	CHECK(value_of("3037000500 sq", &ok) > 9.2e18);
	CHECK(value_of("2097152 cb", &ok) > 9.2e18);
	CHECK(value_of("-9223372036854775808 abs", &ok) > 9.2e18);
	CHECK(value_of("-9223372036854775808 -1 divide", &ok) > 9.2e18 && ok); // used to trap
	CHECK(fabs(value_of("21 fac", &ok) / 51090942171709440000.0 - 1.0) < 1e-12);
	CHECK(value_of("170 fac", &ok) > 7.2e306);

	// And whole where it fits
	CHECK(is_whole("3 4 plus") && is_whole("3 4 minus") && is_whole("3 4 times"));
	CHECK(is_whole("20 fac") && value_of("20 fac", &ok) == 2432902008176640000.0);
	CHECK(is_whole("5.0 fac") && value_of("5.0 fac", &ok) == 120.0 && ok);
	CHECK(refuses("5.5 fac") && refuses("\"x\" fac"));
	CHECK(is_whole("-5 abs") && is_whole("-3 cb"));
}

/** In degrees a whole quarter turn is exact, and tan has no value at the odd ones. */
static void test_degree_trig_is_exact(void) {
	bool ok;
	qdos_math_set_degrees(true);
	CHECK(value_of("180 sin", &ok) == 0.0);
	CHECK(value_of("90 cos", &ok) == 0.0);
	CHECK(value_of("-90 sin", &ok) == -1.0);
	CHECK(value_of("720 cos", &ok) == 1.0);
	CHECK(value_of("180 tan", &ok) == 0.0);
	CHECK(refuses("90 tan") && refuses("-270 tan"));
	CHECK(fabs(value_of("30 sin", &ok) - 0.5) < 1e-15);

	// More than a turn is the same angle, to the last digit
	CHECK(value_of("390 sin", &ok) == value_of("30 sin", &ok));
	CHECK(value_of("1110 tan", &ok) == value_of("30 tan", &ok));
	CHECK(value_of("-330 cos", &ok) == value_of("30 cos", &ok));
	qdos_math_set_degrees(false);
	CHECK(!refuses("90 tan")); // radians: 90 is nowhere near a pole
}

/** From the keypad a result with no finite value is refused and the arguments kept. */
static void test_finite_only_refuses_and_restores(void) {
	qd_interp* interp = fresh();
	qdos_math_set_finite_only(true);
	CHECK(!qd_interp_eval(interp, "1e308 10 times"));
	CHECK(strstr(qd_interp_error(interp), "OVERFLOW") != NULL);
	CHECK(qd_interp_depth(interp) == 2);
	qd_interp_value v;
	CHECK(qd_interp_peek(interp, 0, &v) && v.type == QD_INTERP_VALUE_INT && v.i == 10);
	CHECK(qd_interp_peek(interp, 1, &v) && v.f == 1e308);

	CHECK(qd_interp_eval(interp, "clear -8 0.5 pow") == false);
	CHECK(strstr(qd_interp_error(interp), "UNDEFINED") != NULL);
	CHECK(qd_interp_eval(interp, "clear 1000 exp") == false);
	CHECK(qd_interp_eval(interp, "clear 2 3 pow"));
	qdos_math_set_finite_only(false);

	// Anywhere else infinity is a value like any other
	CHECK(qd_interp_eval(interp, "clear 1e308 10 times"));
	CHECK(qd_interp_peek(interp, 0, &v) && isinf(v.f));
	qd_interp_destroy(interp);
}

int main(void) {
	test_overflow_becomes_a_float();
	test_degree_trig_is_exact();
	test_finite_only_refuses_and_restores();
	test_domains_are_errors();
	test_plain_words();
	test_the_words_the_shell_does_itself();
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
