/**
 * @file test_mathwords.c
 * @brief The maths words, and the domains lib/math would otherwise abort on
 */

#include "check.h"

#include "../src/shell/mathwords.h"

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

int main(void) {
	test_domains_are_errors();
	test_inside_the_domain();
	test_repeated_ln_does_not_abort();
	test_refusal_keeps_the_value();
	return check_report("mathwords");
}
