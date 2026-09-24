/**
 * @file stats.c
 * @brief Summary statistics, regressions and distributions over plain arrays
 */

#include "stats.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static int by_value(const void* a, const void* b) {
	const double x = *(const double*)a, y = *(const double*)b;
	return (x > y) - (x < y);
}

static double median_of(const double* sorted, size_t n) {
	return (n % 2) ? sorted[n / 2] : 0.5 * (sorted[n / 2 - 1] + sorted[n / 2]);
}

bool qdos_stats_one(const double* x, size_t n, qdos_stats1* out) {
	memset(out, 0, sizeof(*out));
	if (n == 0) {
		return false;
	}
	double* sorted = malloc(n * sizeof(*sorted));
	if (sorted == NULL) {
		return false;
	}
	memcpy(sorted, x, n * sizeof(*sorted));
	qsort(sorted, n, sizeof(*sorted), by_value);

	out->n = n;
	for (size_t i = 0; i < n; i++) {
		out->sum += x[i];
		out->sum_sq += x[i] * x[i];
	}
	out->mean = out->sum / (double)n;

	// About the mean rather than from the sums, which cancel badly
	double ss = 0.0;
	for (size_t i = 0; i < n; i++) {
		ss += (x[i] - out->mean) * (x[i] - out->mean);
	}
	out->sigma = sqrt(ss / (double)n);
	out->sx = (n > 1) ? sqrt(ss / (double)(n - 1)) : 0.0;

	// Quartiles as a TI takes them: the medians of the halves either side of
	// the median, which is in neither when there is an odd number
	out->min = sorted[0];
	out->max = sorted[n - 1];
	out->median = median_of(sorted, n);
	if (n == 1) {
		out->q1 = out->q3 = sorted[0];
	} else {
		out->q1 = median_of(sorted, n / 2);
		out->q3 = median_of(sorted + (n + 1) / 2, n / 2);
	}
	free(sorted);
	return true;
}

bool qdos_stats_two(const double* x, const double* y, size_t n, qdos_stats2* out) {
	memset(out, 0, sizeof(*out));
	if (n == 0) {
		return false;
	}
	out->n = n;
	for (size_t i = 0; i < n; i++) {
		out->sum_x += x[i];
		out->sum_y += y[i];
		out->sum_x2 += x[i] * x[i];
		out->sum_y2 += y[i] * y[i];
		out->sum_xy += x[i] * y[i];
	}
	out->mean_x = out->sum_x / (double)n;
	out->mean_y = out->sum_y / (double)n;

	double sxx = 0.0, syy = 0.0, sxy = 0.0;
	for (size_t i = 0; i < n; i++) {
		const double dx = x[i] - out->mean_x, dy = y[i] - out->mean_y;
		sxx += dx * dx;
		syy += dy * dy;
		sxy += dx * dy;
	}
	out->sx = (n > 1) ? sqrt(sxx / (double)(n - 1)) : 0.0;
	out->sy = (n > 1) ? sqrt(syy / (double)(n - 1)) : 0.0;
	out->r = (sxx > 0.0 && syy > 0.0) ? sxy / sqrt(sxx * syy) : 0.0;
	return true;
}

int qdos_regression_terms(qdos_regression model) {
	return (model == QDOS_REG_QUADRATIC) ? 3 : 2;
}

/* y = slope x + intercept, and r^2; false when x does not vary */
static bool fit_line(const double* x, const double* y, size_t n, double* slope, double* intercept, double* r2) {
	if (n < 2) {
		return false;
	}
	double mx = 0.0, my = 0.0;
	for (size_t i = 0; i < n; i++) {
		mx += x[i];
		my += y[i];
	}
	mx /= (double)n;
	my /= (double)n;

	double sxx = 0.0, syy = 0.0, sxy = 0.0;
	for (size_t i = 0; i < n; i++) {
		sxx += (x[i] - mx) * (x[i] - mx);
		syy += (y[i] - my) * (y[i] - my);
		sxy += (x[i] - mx) * (y[i] - my);
	}
	if (sxx <= 0.0) {
		return false;
	}
	*slope = sxy / sxx;
	*intercept = my - *slope * mx;
	*r2 = (syy > 0.0) ? sxy * sxy / (sxx * syy) : 1.0;
	return true;
}

/* A x^2 + B x + C through the points, about x's mean so the squares stay small */
static bool fit_quadratic(const double* x, const double* y, size_t n, double coef[3], double* r2) {
	if (n < 3) {
		return false;
	}
	double mx = 0.0, my = 0.0;
	for (size_t i = 0; i < n; i++) {
		mx += x[i];
		my += y[i];
	}
	mx /= (double)n;
	my /= (double)n;

	// The normal equations in u = x - mean, as an augmented 3x4 matrix
	double m[3][4] = {{0}};
	for (size_t i = 0; i < n; i++) {
		const double u = x[i] - mx, p[3] = {u * u, u, 1.0};
		for (int r = 0; r < 3; r++) {
			for (int c = 0; c < 3; c++) {
				m[r][c] += p[r] * p[c];
			}
			m[r][3] += p[r] * y[i];
		}
	}
	for (int col = 0; col < 3; col++) {
		int pivot = col;
		for (int r = col + 1; r < 3; r++) {
			pivot = (fabs(m[r][col]) > fabs(m[pivot][col])) ? r : pivot;
		}
		if (fabs(m[pivot][col]) < 1e-300) {
			return false;
		}
		for (int c = 0; c < 4; c++) {
			const double t = m[col][c];
			m[col][c] = m[pivot][c];
			m[pivot][c] = t;
		}
		for (int r = 0; r < 3; r++) {
			if (r == col) {
				continue;
			}
			const double f = m[r][col] / m[col][col];
			for (int c = col; c < 4; c++) {
				m[r][c] -= f * m[col][c];
			}
		}
	}
	const double A = m[0][3] / m[0][0], B = m[1][3] / m[1][1], C = m[2][3] / m[2][2];
	coef[0] = A;
	coef[1] = B - 2.0 * A * mx;
	coef[2] = C - B * mx + A * mx * mx;

	double res = 0.0, tot = 0.0;
	for (size_t i = 0; i < n; i++) {
		const double e = y[i] - qdos_regression_at(QDOS_REG_QUADRATIC, coef, x[i]);
		res += e * e;
		tot += (y[i] - my) * (y[i] - my);
	}
	*r2 = (tot > 0.0) ? 1.0 - res / tot : 1.0;
	return true;
}

bool qdos_regress(qdos_regression model, const double* x, const double* y, size_t n, double coef[3], double* r2) {
	coef[0] = coef[1] = coef[2] = 0.0;
	if (model == QDOS_REG_QUADRATIC) {
		return fit_quadratic(x, y, n, coef, r2);
	}
	if (model == QDOS_REG_LINEAR) {
		return fit_line(x, y, n, &coef[0], &coef[1], r2);
	}

	// The others are a line through logarithms of the points
	double* u = malloc((n ? n : 1) * sizeof(*u));
	double* v = malloc((n ? n : 1) * sizeof(*v));
	bool ok = u != NULL && v != NULL;
	for (size_t i = 0; ok && i < n; i++) {
		const bool log_x = (model == QDOS_REG_POWER || model == QDOS_REG_LOG);
		const bool log_y = (model == QDOS_REG_POWER || model == QDOS_REG_EXPONENTIAL);
		ok = (!log_x || x[i] > 0.0) && (!log_y || y[i] > 0.0);
		u[i] = log_x ? log(x[i]) : x[i];
		v[i] = log_y ? log(y[i]) : y[i];
	}
	double slope = 0.0, intercept = 0.0;
	ok = ok && fit_line(u, v, n, &slope, &intercept, r2);
	free(u);
	free(v);
	if (!ok) {
		return false;
	}

	switch (model) {
	case QDOS_REG_EXPONENTIAL:
		coef[0] = exp(intercept);
		coef[1] = exp(slope);
		break;
	case QDOS_REG_POWER:
		coef[0] = exp(intercept);
		coef[1] = slope;
		break;
	default: // QDOS_REG_LOG
		coef[0] = intercept;
		coef[1] = slope;
		break;
	}
	return true;
}

double qdos_regression_at(qdos_regression model, const double coef[3], double x) {
	switch (model) {
	case QDOS_REG_LINEAR:
		return coef[0] * x + coef[1];
	case QDOS_REG_QUADRATIC:
		return (coef[0] * x + coef[1]) * x + coef[2];
	case QDOS_REG_EXPONENTIAL:
		return coef[0] * pow(coef[1], x);
	case QDOS_REG_POWER:
		return coef[0] * pow(x, coef[1]);
	case QDOS_REG_LOG:
		return coef[0] + coef[1] * log(x);
	default:
		return NAN;
	}
}

static bool whole(double v) {
	return isfinite(v) && v >= 0.0 && floor(v) == v;
}

double qdos_ncr(double n, double r) {
	if (!whole(n) || !whole(r)) {
		return NAN;
	}
	if (r > n) {
		return 0.0;
	}
	r = fmin(r, n - r);
	double c = 1.0;
	for (double k = 0.0; k < r; k++) {
		c = c * (n - k) / (k + 1.0);
	}
	return (c < 9007199254740992.0) ? round(c) : c;
}

double qdos_npr(double n, double r) {
	if (!whole(n) || !whole(r)) {
		return NAN;
	}
	if (r > n) {
		return 0.0;
	}
	double p = 1.0;
	for (double k = 0.0; k < r; k++) {
		p *= n - k;
	}
	return p;
}

#define SQRT2 1.41421356237309504880
#define SQRT2PI 2.50662827463100050242

double qdos_normal_pdf(double x, double mu, double sigma) {
	if (!(sigma > 0.0)) {
		return NAN;
	}
	const double z = (x - mu) / sigma;
	return exp(-0.5 * z * z) / (sigma * SQRT2PI);
}

/* P(Z <= z), from erfc so the far tail keeps its digits */
static double phi(double z) {
	return 0.5 * erfc(-z / SQRT2);
}

double qdos_normal_cdf(double lo, double hi, double mu, double sigma) {
	if (!(sigma > 0.0)) {
		return NAN;
	}
	return phi((hi - mu) / sigma) - phi((lo - mu) / sigma);
}

/* Acklam's rational approximation, then one Halley step against erfc */
double qdos_inv_norm(double p, double mu, double sigma) {
	if (!(p > 0.0 && p < 1.0) || !(sigma > 0.0)) {
		return NAN;
	}
	static const double a[] = {-3.969683028665376e+01, 2.209460984245205e+02, -2.759285104469687e+02,
			1.383577518672690e+02, -3.066479806614716e+01, 2.506628277459239e+00};
	static const double b[] = {-5.447609879822406e+01, 1.615858368580409e+02, -1.556989798598866e+02,
			6.680131188771972e+01, -1.328068155288572e+01};
	static const double c[] = {-7.784894002430293e-03, -3.223964580411365e-01, -2.400758277161838e+00,
			-2.549732539343734e+00, 4.374664141464968e+00, 2.938163982698783e+00};
	static const double d[] = {
			7.784695709041462e-03, 3.224671290700398e-01, 2.445134137142996e+00, 3.754408661907416e+00};

	double z;
	if (p < 0.02425) {
		const double q = sqrt(-2.0 * log(p));
		z = (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
			((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0);
	} else if (p > 1.0 - 0.02425) {
		const double q = sqrt(-2.0 * log(1.0 - p));
		z = -(((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
			((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0);
	} else {
		const double q = p - 0.5, r = q * q;
		z = (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5]) * q /
			(((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1.0);
	}

	const double e = phi(z) - p;
	const double u = e * SQRT2PI * exp(0.5 * z * z);
	z -= u / (1.0 + 0.5 * z * u);
	return mu + sigma * z;
}

double qdos_binom_pdf(double n, double p, double k) {
	if (!whole(n) || !(p >= 0.0 && p <= 1.0) || !isfinite(k) || floor(k) != k) {
		return NAN;
	}
	if (k < 0.0 || k > n) {
		return 0.0;
	}
	if (p == 0.0) {
		return (k == 0.0) ? 1.0 : 0.0;
	}
	if (p == 1.0) {
		return (k == n) ? 1.0 : 0.0;
	}
	return exp(lgamma(n + 1.0) - lgamma(k + 1.0) - lgamma(n - k + 1.0) + k * log(p) + (n - k) * log1p(-p));
}

double qdos_binom_cdf(double n, double p, double k) {
	if (!whole(n) || !(p >= 0.0 && p <= 1.0) || !isfinite(k)) {
		return NAN;
	}
	k = floor(k);
	if (k < 0.0) {
		return 0.0;
	}
	if (k >= n) {
		return 1.0;
	}
	double sum = 0.0;
	for (double i = 0.0; i <= k; i++) {
		sum += qdos_binom_pdf(n, p, i);
	}
	return fmin(sum, 1.0);
}

static uint64_t g_rand = 0x9E3779B97F4A7C15ull;

void qdos_rand_seed(uint64_t seed) {
	g_rand = seed ? seed : 0x9E3779B97F4A7C15ull;
}

/* xorshift64*, the top 53 bits as the fraction */
double qdos_rand(void) {
	g_rand ^= g_rand >> 12;
	g_rand ^= g_rand << 25;
	g_rand ^= g_rand >> 27;
	return (double)((g_rand * 0x2545F4914F6CDD1Dull) >> 11) / 9007199254740992.0;
}
