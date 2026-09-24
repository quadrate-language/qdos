/**
 * @file test_stats.c
 * @brief Statistics, regressions and distributions, against worked answers
 */

#include "check.h"

#include "../src/shell/stats.h"

#include <math.h>

#define NEAR(a, b, tol) (fabs((a) - (b)) <= (tol))

static void test_one_variable(void) {
	const double x[] = {2, 4, 4, 4, 5, 5, 7, 9};
	qdos_stats1 s;
	CHECK(qdos_stats_one(x, 8, &s));
	CHECK(s.n == 8);
	CHECK(NEAR(s.mean, 5.0, 1e-12));
	CHECK(NEAR(s.sum, 40.0, 1e-12));
	CHECK(NEAR(s.sum_sq, 232.0, 1e-12));
	CHECK(NEAR(s.sigma, 2.0, 1e-12));
	CHECK(NEAR(s.sx, sqrt(32.0 / 7.0), 1e-12));
	CHECK(s.min == 2.0 && s.max == 9.0);
	CHECK(NEAR(s.median, 4.5, 1e-12));
	CHECK(NEAR(s.q1, 4.0, 1e-12));
	CHECK(NEAR(s.q3, 6.0, 1e-12));

	// Odd: the median belongs to neither half, and the order given does not matter
	const double odd[] = {9, 1, 5, 3, 7};
	CHECK(qdos_stats_one(odd, 5, &s));
	CHECK(s.median == 5.0 && s.q1 == 2.0 && s.q3 == 8.0);

	const double one[] = {3};
	CHECK(qdos_stats_one(one, 1, &s));
	CHECK(s.sx == 0.0 && s.q1 == 3.0 && s.q3 == 3.0);
	CHECK(!qdos_stats_one(one, 0, &s));
}

static void test_two_variable(void) {
	const double x[] = {1, 2, 3, 4, 5};
	const double y[] = {2, 4, 5, 4, 5};
	qdos_stats2 s;
	CHECK(qdos_stats_two(x, y, 5, &s));
	CHECK(NEAR(s.mean_x, 3.0, 1e-12) && NEAR(s.mean_y, 4.0, 1e-12));
	CHECK(NEAR(s.sum_xy, 66.0, 1e-12));
	CHECK(NEAR(s.r, 0.7745966692414834, 1e-12));
}

static void test_linear(void) {
	const double x[] = {1, 2, 3, 4, 5};
	const double y[] = {2, 4, 5, 4, 5};
	double c[3], r2;
	CHECK(qdos_regress(QDOS_REG_LINEAR, x, y, 5, c, &r2));
	CHECK(NEAR(c[0], 0.6, 1e-12));
	CHECK(NEAR(c[1], 2.2, 1e-12));
	CHECK(NEAR(r2, 0.6, 1e-12));
	CHECK(NEAR(qdos_regression_at(QDOS_REG_LINEAR, c, 10.0), 8.2, 1e-12));

	const double flat[] = {3, 3, 3};
	CHECK(!qdos_regress(QDOS_REG_LINEAR, flat, y, 3, c, &r2)); // x does not vary
	CHECK(!qdos_regress(QDOS_REG_LINEAR, x, y, 1, c, &r2));
}

static void test_quadratic(void) {
	const double x[] = {-2, -1, 0, 1, 2, 3};
	double y[6];
	for (int i = 0; i < 6; i++) {
		y[i] = 2.0 * x[i] * x[i] - 3.0 * x[i] + 1.0;
	}
	double c[3], r2;
	CHECK(qdos_regress(QDOS_REG_QUADRATIC, x, y, 6, c, &r2));
	CHECK(NEAR(c[0], 2.0, 1e-9) && NEAR(c[1], -3.0, 1e-9) && NEAR(c[2], 1.0, 1e-9));
	CHECK(NEAR(r2, 1.0, 1e-12));
	CHECK(qdos_regression_terms(QDOS_REG_QUADRATIC) == 3);
	CHECK(!qdos_regress(QDOS_REG_QUADRATIC, x, y, 2, c, &r2));
}

static void test_logarithmic_models(void) {
	const double x[] = {1, 2, 3, 4};
	double y[4];
	double c[3], r2;

	for (int i = 0; i < 4; i++) {
		y[i] = 3.0 * pow(2.0, x[i]);
	}
	CHECK(qdos_regress(QDOS_REG_EXPONENTIAL, x, y, 4, c, &r2));
	CHECK(NEAR(c[0], 3.0, 1e-9) && NEAR(c[1], 2.0, 1e-9) && NEAR(r2, 1.0, 1e-12));

	for (int i = 0; i < 4; i++) {
		y[i] = 5.0 * pow(x[i], 1.5);
	}
	CHECK(qdos_regress(QDOS_REG_POWER, x, y, 4, c, &r2));
	CHECK(NEAR(c[0], 5.0, 1e-9) && NEAR(c[1], 1.5, 1e-9));

	for (int i = 0; i < 4; i++) {
		y[i] = 1.0 + 2.0 * log(x[i]);
	}
	CHECK(qdos_regress(QDOS_REG_LOG, x, y, 4, c, &r2));
	CHECK(NEAR(c[0], 1.0, 1e-9) && NEAR(c[1], 2.0, 1e-9));

	// A value the logarithm cannot take
	y[2] = -1.0;
	CHECK(!qdos_regress(QDOS_REG_EXPONENTIAL, x, y, 4, c, &r2));
}

static void test_counting(void) {
	CHECK(qdos_ncr(5, 2) == 10.0);
	CHECK(qdos_ncr(52, 5) == 2598960.0);
	CHECK(qdos_ncr(3, 5) == 0.0);
	CHECK(isnan(qdos_ncr(5.5, 2)));
	CHECK(isnan(qdos_ncr(-1, 0)));
	CHECK(qdos_npr(5, 2) == 20.0);
	CHECK(qdos_npr(10, 0) == 1.0);
}

static void test_normal(void) {
	CHECK(NEAR(qdos_normal_pdf(0, 0, 1), 0.3989422804014327, 1e-15));
	CHECK(NEAR(qdos_normal_cdf(-1, 1, 0, 1), 0.6826894921370859, 1e-14));
	CHECK(NEAR(qdos_normal_cdf(-1e99, 0, 0, 1), 0.5, 1e-15));
	CHECK(NEAR(qdos_normal_cdf(90, 110, 100, 10), 0.6826894921370859, 1e-14));
	CHECK(NEAR(qdos_inv_norm(0.975, 0, 1), 1.959963984540054, 1e-12));
	CHECK(NEAR(qdos_inv_norm(0.5, 100, 15), 100.0, 1e-12));
	CHECK(NEAR(qdos_inv_norm(1e-10, 0, 1), -6.361340902404056, 1e-9));
	CHECK(isnan(qdos_inv_norm(1.0, 0, 1)));
	CHECK(isnan(qdos_normal_pdf(0, 0, 0)));
}

static void test_binomial(void) {
	CHECK(NEAR(qdos_binom_pdf(10, 0.5, 5), 0.24609375, 1e-14));
	CHECK(NEAR(qdos_binom_cdf(10, 0.5, 5), 0.623046875, 1e-14));
	CHECK(qdos_binom_pdf(10, 0.5, 11) == 0.0);
	CHECK(qdos_binom_cdf(10, 0.5, 10) == 1.0);
	CHECK(qdos_binom_cdf(10, 0.5, -1) == 0.0);
	CHECK(qdos_binom_pdf(4, 0.0, 0) == 1.0);
	CHECK(isnan(qdos_binom_pdf(10, 1.5, 5)));
}

static void test_rand(void) {
	qdos_rand_seed(42);
	double lo = 1.0, hi = 0.0, sum = 0.0;
	for (int i = 0; i < 10000; i++) {
		const double r = qdos_rand();
		lo = fmin(lo, r);
		hi = fmax(hi, r);
		sum += r;
	}
	CHECK(lo >= 0.0 && hi < 1.0);
	CHECK(NEAR(sum / 10000.0, 0.5, 0.02));

	// The same seed, the same numbers
	qdos_rand_seed(7);
	const double a = qdos_rand();
	qdos_rand_seed(7);
	CHECK(qdos_rand() == a);
}

int main(void) {
	test_one_variable();
	test_two_variable();
	test_linear();
	test_quadratic();
	test_logarithmic_models();
	test_counting();
	test_normal();
	test_binomial();
	test_rand();
	return check_report("stats");
}
