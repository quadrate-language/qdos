/**
 * @file test_numeric.c
 * @brief Roots, extrema, slopes and areas, against functions with known answers
 */

#include "check.h"

#include "../src/shell/numeric.h"

#include <math.h>

#define NEAR(a, b, tol) (fabs((a) - (b)) <= (tol))
#define PI 3.14159265358979323846

static int g_calls;

static bool f_square_minus_two(void* user, double x, double* y) {
	(void)user;
	g_calls++;
	*y = x * x - 2.0;
	return true;
}

static bool f_square(void* user, double x, double* y) {
	(void)user;
	*y = x * x;
	return true;
}

static bool f_sin(void* user, double x, double* y) {
	(void)user;
	*y = sin(x);
	return true;
}

static bool f_tan(void* user, double x, double* y) {
	(void)user;
	*y = tan(x);
	return true;
}

static bool f_ln(void* user, double x, double* y) {
	(void)user;
	if (x <= 0.0) {
		return false;
	}
	*y = log(x);
	return true;
}

static bool f_inv_sqrt(void* user, double x, double* y) {
	(void)user;
	if (x <= 0.0) {
		return false;
	}
	*y = 1.0 / sqrt(x);
	return true;
}

static bool f_cubic(void* user, double x, double* y) {
	(void)user;
	*y = x * x * x - 3.0 * x;
	return true;
}

static bool f_nowhere(void* user, double x, double* y) {
	(void)user;
	(void)x;
	(void)y;
	return false;
}

static bool f_positive(void* user, double x, double* y) {
	(void)user;
	*y = x * x + 1.0;
	return true;
}

static void test_root_of_a_crossing(void) {
	double x;
	g_calls = 0;
	CHECK(qdos_num_root(f_square_minus_two, NULL, 0.0, 5.0, &x));
	CHECK(NEAR(x, sqrt(2.0), 1e-12));
	CHECK(g_calls < 200); // the scan and a handful of steps, not a bisection to the last bit

	// The first one in the interval, bounds either way round
	CHECK(qdos_num_root(f_square_minus_two, NULL, 5.0, -5.0, &x));
	CHECK(NEAR(x, -sqrt(2.0), 1e-12));
}

static void test_root_that_touches(void) {
	double x;
	CHECK(qdos_num_root(f_square, NULL, -3.0, 5.0, &x));
	CHECK(NEAR(x, 0.0, 1e-5));
}

static void test_root_of_sin_is_a_multiple_of_pi(void) {
	double x;
	CHECK(qdos_num_root(f_sin, NULL, 2.0, 4.0, &x));
	CHECK(NEAR(x, PI, 1e-12));
}

static void test_a_pole_is_not_a_root(void) {
	double x;
	// tan changes sign at pi/2 without passing nought; the root is at pi
	CHECK(qdos_num_root(f_tan, NULL, 1.0, 3.5, &x));
	CHECK(NEAR(x, PI, 1e-9));
	CHECK(!qdos_num_root(f_tan, NULL, 1.0, 2.0, &x));
}

static void test_no_root(void) {
	double x;
	CHECK(!qdos_num_root(f_positive, NULL, -5.0, 5.0, &x));
	CHECK(!qdos_num_root(f_nowhere, NULL, -5.0, 5.0, &x));
}

static void test_extrema(void) {
	double x, y;
	CHECK(qdos_num_minimum(f_cubic, NULL, 0.0, 3.0, &x, &y));
	CHECK(NEAR(x, 1.0, 1e-6));
	CHECK(NEAR(y, -2.0, 1e-10));

	CHECK(qdos_num_maximum(f_cubic, NULL, -3.0, 0.0, &x, &y));
	CHECK(NEAR(x, -1.0, 1e-6));
	CHECK(NEAR(y, 2.0, 1e-10));

	// Lowest at the edge of the interval, where the curve is still falling
	CHECK(qdos_num_minimum(f_cubic, NULL, -0.5, 0.5, &x, &y));
	CHECK(NEAR(x, 0.5, 1e-6));

	CHECK(!qdos_num_maximum(f_nowhere, NULL, 0.0, 1.0, &x, &y));
}

static void test_derivative(void) {
	double d;
	CHECK(qdos_num_derivative(f_sin, NULL, 0.0, &d));
	CHECK(NEAR(d, 1.0, 1e-10));
	CHECK(qdos_num_derivative(f_cubic, NULL, 2.0, &d));
	CHECK(NEAR(d, 9.0, 1e-8));
	CHECK(qdos_num_derivative(f_square, NULL, 1000.0, &d));
	CHECK(NEAR(d, 2000.0, 1e-6));
	CHECK(!qdos_num_derivative(f_ln, NULL, 0.0, &d));
}

static void test_integral(void) {
	double v;
	CHECK(qdos_num_integral(f_sin, NULL, 0.0, PI, &v));
	CHECK(NEAR(v, 2.0, 1e-12));
	CHECK(qdos_num_integral(f_square, NULL, 0.0, 3.0, &v));
	CHECK(NEAR(v, 9.0, 1e-12));

	// Backwards is negative
	CHECK(qdos_num_integral(f_square, NULL, 3.0, 0.0, &v));
	CHECK(NEAR(v, -9.0, 1e-12));

	// Undefined at an end, but never evaluated there
	CHECK(qdos_num_integral(f_ln, NULL, 0.0, 1.0, &v));
	CHECK(NEAR(v, -1.0, 1e-8));
	CHECK(qdos_num_integral(f_inv_sqrt, NULL, 0.0, 1.0, &v));
	CHECK(NEAR(v, 2.0, 1e-4));

	// Undefined inside is no answer
	CHECK(!qdos_num_integral(f_ln, NULL, -1.0, 1.0, &v));
	CHECK(qdos_num_integral(f_sin, NULL, 1.0, 1.0, &v) && v == 0.0);
}

int main(void) {
	test_root_of_a_crossing();
	test_root_that_touches();
	test_root_of_sin_is_a_multiple_of_pi();
	test_a_pole_is_not_a_root();
	test_no_root();
	test_extrema();
	test_derivative();
	test_integral();
	return check_report("numeric");
}
