/**
 * @file stats.h
 * @brief Summary statistics, regressions and distributions over plain arrays
 *
 * Nothing here knows about lists, the store or the interpreter; the words and
 * the STAT page both come to it with doubles.
 */

#ifndef QDOS_STATS_H
#define QDOS_STATS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief One-variable statistics, as a TI's 1-Var Stats lists them */
typedef struct {
	size_t n;
	double mean;
	double sum;
	double sum_sq;
	double sx;	  ///< Sample standard deviation; 0 for a single value
	double sigma; ///< Population standard deviation
	double min, q1, median, q3, max;
} qdos_stats1;

/** @brief Two-variable statistics */
typedef struct {
	size_t n;
	double mean_x, mean_y;
	double sum_x, sum_y;
	double sum_x2, sum_y2, sum_xy;
	double sx, sy; ///< Sample standard deviations
	double r;	   ///< Correlation; 0 where either variable is constant
} qdos_stats2;

/** @brief False when @p n is nought */
bool qdos_stats_one(const double* x, size_t n, qdos_stats1* out);

/** @brief False when @p n is nought */
bool qdos_stats_two(const double* x, const double* y, size_t n, qdos_stats2* out);

typedef enum {
	QDOS_REG_LINEAR = 0,  ///< y = a x + b
	QDOS_REG_QUADRATIC,	  ///< y = a x^2 + b x + c
	QDOS_REG_EXPONENTIAL, ///< y = a b^x, y > 0
	QDOS_REG_POWER,		  ///< y = a x^b, x > 0 and y > 0
	QDOS_REG_LOG,		  ///< y = a + b ln x, x > 0
	QDOS_REG__COUNT
} qdos_regression;

/** @brief How many coefficients a model has: 2, or 3 for the quadratic */
int qdos_regression_terms(qdos_regression model);

/**
 * @brief The least-squares fit of @p model to the points
 * @param coef a, b and, for the quadratic, c
 * @param r2 How much of the variation in y the fit explains
 * @return false with too few points, or with values the model cannot take
 */
bool qdos_regress(qdos_regression model, const double* x, const double* y, size_t n, double coef[3], double* r2);

/** @brief The model at @p x, with the coefficients qdos_regress found */
double qdos_regression_at(qdos_regression model, const double coef[3], double x);

double qdos_ncr(double n, double r);
double qdos_npr(double n, double r);

double qdos_normal_pdf(double x, double mu, double sigma);

/** @brief The probability of a value between @p lo and @p hi */
double qdos_normal_cdf(double lo, double hi, double mu, double sigma);

/** @brief The x below which a proportion @p p of values lie; NaN outside (0, 1) */
double qdos_inv_norm(double p, double mu, double sigma);

/** @brief The chance of exactly @p k successes in @p n trials */
double qdos_binom_pdf(double n, double p, double k);

/** @brief The chance of @p k successes or fewer */
double qdos_binom_cdf(double n, double p, double k);

void qdos_rand_seed(uint64_t seed);

/** @brief Uniform on [0, 1) */
double qdos_rand(void);

#ifdef __cplusplus
}
#endif

#endif // QDOS_STATS_H
