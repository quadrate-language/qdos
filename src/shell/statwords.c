/**
 * @file statwords.c
 * @brief Statistics and probability, registered as shell words
 */

#include "statwords.h"

#include "mathwords.h"
#include "stats.h"

#include <quadrate/rt/array.h>
#include <quadrate/rt/runtime.h>
#include <quadrate/rt/stack.h>

#include <math.h>
#include <stdlib.h>

bool qdos_pop_numbers(qd_context* ctx, const char* word, double** out, size_t* n) {
	*out = NULL;
	*n = 0;

	qd_stack_element_t top;
	if (qd_stack_pop(ctx->st, &top) != QD_STACK_OK) {
		qdos_math_error(ctx, word, "NEED A LIST");
		return false;
	}
	if (top.type != QD_STACK_TYPE_PTR || !qd_array_is_valid(top.value.p)) {
		qdos_math_error(ctx, word, "NEED A LIST");
		return false;
	}

	qd_array_t* array = (qd_array_t*)top.value.p;
	const size_t length = qd_array_length(array);
	if (array->elemType != QD_ARRAY_TYPE_INT && array->elemType != QD_ARRAY_TYPE_FLOAT) {
		qd_array_release(array);
		qdos_math_error(ctx, word, "NEED A LIST OF NUMBERS");
		return false;
	}

	double* values = malloc((length ? length : 1) * sizeof(*values));
	if (values == NULL) {
		qd_array_release(array);
		qdos_math_error(ctx, word, "OUT OF MEMORY");
		return false;
	}
	for (size_t i = 0; i < length; i++) {
		int64_t whole = 0;
		if (array->elemType == QD_ARRAY_TYPE_INT) {
			qd_array_get_int(array, i, &whole);
			values[i] = (double)whole;
		} else {
			qd_array_get_float(array, i, &values[i]);
		}
	}
	qd_array_release(array);

	*out = values;
	*n = length;
	return true;
}

int qdos_push_numbers(qd_context* ctx, const double* values, size_t n) {
	qd_array_t* array = qd_array_create(n, QD_ARRAY_TYPE_FLOAT);
	if (array == NULL) {
		return 1;
	}
	for (size_t i = 0; i < n; i++) {
		qd_array_push_float(array, values[i]);
	}
	return qd_push_p(ctx, array);
}

/* Arguments off the top, the last of them first, as numbers */
static bool pop_args(qd_context* ctx, const char* word, double* args, int count) {
	for (int i = count - 1; i >= 0; i--) {
		qd_stack_element_t e;
		if (qd_stack_pop(ctx->st, &e) != QD_STACK_OK) {
			qdos_math_error(ctx, word, "NEEDS A NUMBER");
			return false;
		}
		if (e.type == QD_STACK_TYPE_INT) {
			args[i] = (double)e.value.i;
		} else if (e.type == QD_STACK_TYPE_FLOAT) {
			args[i] = e.value.f;
		} else {
			qdos_math_error(ctx, word, "NEEDS A NUMBER");
			return false;
		}
	}
	return true;
}

/* A result that has no value is an error rather than a NaN on the stack */
static int push_result(qd_context* ctx, const char* word, double value) {
	if (isnan(value)) {
		return qdos_math_error(ctx, word, "OUT OF DOMAIN");
	}
	return qd_push_f(ctx, value);
}

/* Whole when it is, as fac is: 5 2 ncr is 10, not 10.0 */
static int push_count(qd_context* ctx, const char* word, double value) {
	if (isnan(value)) {
		return qdos_math_error(ctx, word, "NEEDS WHOLE NUMBERS");
	}
	if (value < 9007199254740992.0) {
		return qd_push_i(ctx, (int64_t)value);
	}
	return qd_push_f(ctx, value);
}

typedef enum {
	ONE_SUM,
	ONE_MEAN,
	ONE_MEDIAN,
	ONE_SX,
	ONE_SIGMA
} one_what;

static int one_var(qd_context* ctx, const char* word, one_what what) {
	double* x;
	size_t n;
	if (!qdos_pop_numbers(ctx, word, &x, &n)) {
		return 1;
	}
	qdos_stats1 s;
	const bool ok = qdos_stats_one(x, n, &s);
	free(x);
	if (!ok) {
		return qdos_math_error(ctx, word, "LIST IS EMPTY");
	}
	if (what == ONE_SX && n < 2) {
		return qdos_math_error(ctx, word, "NEEDS TWO VALUES");
	}
	const double r[] = {s.sum, s.mean, s.median, s.sx, s.sigma};
	return qd_push_f(ctx, r[what]);
}

static int w_sum(qd_context* ctx, void* user) {
	(void)user;
	return one_var(ctx, "sum", ONE_SUM);
}

static int w_mean(qd_context* ctx, void* user) {
	(void)user;
	return one_var(ctx, "mean", ONE_MEAN);
}

static int w_median(qd_context* ctx, void* user) {
	(void)user;
	return one_var(ctx, "median", ONE_MEDIAN);
}

static int w_stdev(qd_context* ctx, void* user) {
	(void)user;
	return one_var(ctx, "stdev", ONE_SX);
}

static int w_pstdev(qd_context* ctx, void* user) {
	(void)user;
	return one_var(ctx, "pstdev", ONE_SIGMA);
}

/* xs and ys, the same length; ys is on top */
static bool pop_pair(qd_context* ctx, const char* word, double** x, double** y, size_t* n) {
	size_t nx, ny;
	if (!qdos_pop_numbers(ctx, word, y, &ny)) {
		return false;
	}
	if (!qdos_pop_numbers(ctx, word, x, &nx)) {
		free(*y);
		return false;
	}
	if (nx != ny) {
		free(*x);
		free(*y);
		qdos_math_error(ctx, word, "LISTS DIFFER IN LENGTH");
		return false;
	}
	*n = nx;
	return true;
}

static int w_corr(qd_context* ctx, void* user) {
	(void)user;
	double *x, *y;
	size_t n;
	if (!pop_pair(ctx, "corr", &x, &y, &n)) {
		return 1;
	}
	qdos_stats2 s;
	const bool ok = n >= 2 && qdos_stats_two(x, y, n, &s);
	free(x);
	free(y);
	if (!ok) {
		return qdos_math_error(ctx, "corr", "NEEDS TWO POINTS");
	}
	return qd_push_f(ctx, s.r);
}

static int regress(qd_context* ctx, const char* word, qdos_regression model) {
	double *x, *y;
	size_t n;
	if (!pop_pair(ctx, word, &x, &y, &n)) {
		return 1;
	}
	double coef[3], r2;
	const bool ok = qdos_regress(model, x, y, n, coef, &r2);
	free(x);
	free(y);
	if (!ok) {
		return qdos_math_error(ctx, word, "CANNOT FIT THESE POINTS");
	}
	for (int i = 0; i < qdos_regression_terms(model); i++) {
		if (qd_push_f(ctx, coef[i]) != 0) {
			return 1;
		}
	}
	return 0;
}

static int w_linreg(qd_context* ctx, void* user) {
	(void)user;
	return regress(ctx, "linreg", QDOS_REG_LINEAR);
}

static int w_quadreg(qd_context* ctx, void* user) {
	(void)user;
	return regress(ctx, "quadreg", QDOS_REG_QUADRATIC);
}

static int w_expreg(qd_context* ctx, void* user) {
	(void)user;
	return regress(ctx, "expreg", QDOS_REG_EXPONENTIAL);
}

static int w_pwrreg(qd_context* ctx, void* user) {
	(void)user;
	return regress(ctx, "pwrreg", QDOS_REG_POWER);
}

static int w_lnreg(qd_context* ctx, void* user) {
	(void)user;
	return regress(ctx, "lnreg", QDOS_REG_LOG);
}

static int w_ncr(qd_context* ctx, void* user) {
	(void)user;
	double a[2];
	if (!pop_args(ctx, "ncr", a, 2)) {
		return 1;
	}
	return push_count(ctx, "ncr", qdos_ncr(a[0], a[1]));
}

static int w_npr(qd_context* ctx, void* user) {
	(void)user;
	double a[2];
	if (!pop_args(ctx, "npr", a, 2)) {
		return 1;
	}
	return push_count(ctx, "npr", qdos_npr(a[0], a[1]));
}

static int w_normalpdf(qd_context* ctx, void* user) {
	(void)user;
	double a[3];
	if (!pop_args(ctx, "normalpdf", a, 3)) {
		return 1;
	}
	return push_result(ctx, "normalpdf", qdos_normal_pdf(a[0], a[1], a[2]));
}

static int w_normalcdf(qd_context* ctx, void* user) {
	(void)user;
	double a[4];
	if (!pop_args(ctx, "normalcdf", a, 4)) {
		return 1;
	}
	return push_result(ctx, "normalcdf", qdos_normal_cdf(a[0], a[1], a[2], a[3]));
}

static int w_invnorm(qd_context* ctx, void* user) {
	(void)user;
	double a[3];
	if (!pop_args(ctx, "invnorm", a, 3)) {
		return 1;
	}
	return push_result(ctx, "invnorm", qdos_inv_norm(a[0], a[1], a[2]));
}

static int w_binompdf(qd_context* ctx, void* user) {
	(void)user;
	double a[3];
	if (!pop_args(ctx, "binompdf", a, 3)) {
		return 1;
	}
	return push_result(ctx, "binompdf", qdos_binom_pdf(a[0], a[1], a[2]));
}

static int w_binomcdf(qd_context* ctx, void* user) {
	(void)user;
	double a[3];
	if (!pop_args(ctx, "binomcdf", a, 3)) {
		return 1;
	}
	return push_result(ctx, "binomcdf", qdos_binom_cdf(a[0], a[1], a[2]));
}

static int w_rand(qd_context* ctx, void* user) {
	(void)user;
	return qd_push_f(ctx, qdos_rand());
}

/* The same seed, the same numbers after it: for a lesson, or a test */
static int w_randseed(qd_context* ctx, void* user) {
	(void)user;
	double a[1];
	if (!pop_args(ctx, "randseed", a, 1)) {
		return 1;
	}
	qdos_rand_seed((uint64_t)(int64_t)a[0]);
	return 0;
}

static int w_randint(qd_context* ctx, void* user) {
	(void)user;
	double a[2];
	if (!pop_args(ctx, "randint", a, 2)) {
		return 1;
	}
	const double lo = ceil(fmin(a[0], a[1])), hi = floor(fmax(a[0], a[1]));
	if (lo > hi || hi - lo >= 9007199254740992.0) {
		return qdos_math_error(ctx, "randint", "NO WHOLE NUMBER BETWEEN");
	}
	return qd_push_i(ctx, (int64_t)(lo + floor(qdos_rand() * (hi - lo + 1.0))));
}

#define LIST "(xs:[]f64 -- r:f64)"
#define PAIR "(xs:[]f64 ys:[]f64 -- a:f64 b:f64)"

static const struct {
	const char* name;
	const char* signature;
	qd_interp_native_fn fn;
} WORDS[] = {
		{"sum", LIST, w_sum},
		{"mean", LIST, w_mean},
		{"median", LIST, w_median},
		{"stdev", LIST, w_stdev},
		{"pstdev", LIST, w_pstdev},
		{"corr", "(xs:[]f64 ys:[]f64 -- r:f64)", w_corr},
		{"linreg", PAIR, w_linreg},
		{"quadreg", "(xs:[]f64 ys:[]f64 -- a:f64 b:f64 c:f64)", w_quadreg},
		{"expreg", PAIR, w_expreg},
		{"pwrreg", PAIR, w_pwrreg},
		{"lnreg", PAIR, w_lnreg},
		{"ncr", "(n:i64 r:i64 -- c:i64)", w_ncr},
		{"npr", "(n:i64 r:i64 -- p:i64)", w_npr},
		{"normalpdf", "(x:f64 mu:f64 sigma:f64 -- p:f64)", w_normalpdf},
		{"normalcdf", "(lo:f64 hi:f64 mu:f64 sigma:f64 -- p:f64)", w_normalcdf},
		{"invnorm", "(p:f64 mu:f64 sigma:f64 -- x:f64)", w_invnorm},
		{"binompdf", "(n:i64 p:f64 k:i64 -- q:f64)", w_binompdf},
		{"binomcdf", "(n:i64 p:f64 k:i64 -- q:f64)", w_binomcdf},
		{"rand", "( -- r:f64)", w_rand},
		{"randint", "(lo:i64 hi:i64 -- n:i64)", w_randint},
		{"randseed", "(n:i64 -- )", w_randseed},
};

void qdos_register_stats(qd_interp* interp) {
	for (size_t i = 0; i < sizeof(WORDS) / sizeof(*WORDS); i++) {
		qd_interp_register(interp, WORDS[i].name, WORDS[i].signature, WORDS[i].fn, NULL);
	}
}
