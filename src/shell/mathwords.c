/**
 * @file mathwords.c
 * @brief Quadrate ships these in lib/math; the interpreter does not register them
 */

#include "mathwords.h"

#include <quadrate/math/math.h>
#include <quadrate/rt/runtime.h>
#include <quadrate/rt/stack.h>

#include <math.h>
#include <stddef.h>
#include <stdio.h>

#define QDOS_PI 3.14159265358979323846

static bool g_degrees;

void qdos_math_set_degrees(bool degrees) {
	g_degrees = degrees;
}

bool qdos_math_degrees(void) {
	return g_degrees;
}

bool qdos_peek_number(qd_context* ctx, size_t depth, double* out) {
	const size_t size = qd_stack_size(ctx->st);
	if (depth >= size) {
		return false;
	}

	qd_stack_element_t element;
	if (qd_stack_element(ctx->st, size - 1 - depth, &element) != QD_STACK_OK) {
		return false;
	}

	if (element.type == QD_STACK_TYPE_INT) {
		*out = (double)element.value.i;
	} else if (element.type == QD_STACK_TYPE_FLOAT) {
		*out = element.value.f;
	} else {
		return false;
	}
	return true;
}

int qdos_math_error(qd_context* ctx, const char* word, const char* text) {
	char message[80];
	snprintf(message, sizeof(message), "%s: %s", word, text);
	qd_set_error_msg(ctx, message);
	return 1;
}

/** @brief Swap the top value for @p value, leaving the rest of the stack */
static int replace_top(qd_context* ctx, double value) {
	qd_stack_element_t discard;
	if (qd_stack_pop(ctx->st, &discard) != QD_STACK_OK) {
		return 1;
	}
	return qd_push_f(ctx, value);
}

/* usr_math_* take only a context; a native takes userdata as well */
#define WRAP(name)                                                                                                     \
	static int w_##name(qd_context* ctx, void* user) {                                                                 \
		(void)user;                                                                                                    \
		return usr_math_##name(ctx);                                                                                   \
	}

WRAP(sinh)
WRAP(cosh)
WRAP(tanh)
WRAP(asinh)
WRAP(cbrt)
WRAP(exp)
WRAP(exp2)
WRAP(ceil)
WRAP(floor)
WRAP(round)
WRAP(trunc)
WRAP(pow)
WRAP(hypot)
WRAP(atan2)

#undef WRAP

/*
 * lib/math writes these in Quadrate rather than C, so there is no usr_math_
 * symbol to call and they are done here. Whole numbers stay whole: `3 sq` is
 * 9 on a calculator, not 9.0, and the bitwise words still take it.
 */
static int whole_unary(qd_context* ctx, int64_t (*on_int)(int64_t), double (*on_real)(double)) {
	qd_stack_element_t top;
	if (qd_stack_pop(ctx->st, &top) != QD_STACK_OK) {
		return 1;
	}

	if (top.type == QD_STACK_TYPE_INT) {
		return qd_push_i(ctx, on_int(top.value.i));
	}
	if (top.type == QD_STACK_TYPE_FLOAT) {
		return qd_push_f(ctx, on_real(top.value.f));
	}

	return qdos_math_error(ctx, "math", "NEEDS A NUMBER");
}

static int64_t sq_i(int64_t x) {
	return x * x;
}

static double sq_f(double x) {
	return x * x;
}

static int64_t cb_i(int64_t x) {
	return x * x * x;
}

static double cb_f(double x) {
	return x * x * x;
}

static int64_t abs_i(int64_t x) {
	return x < 0 ? -x : x;
}

static double abs_f(double x) {
	return fabs(x);
}

static int w_sq(qd_context* ctx, void* user) {
	(void)user;
	return whole_unary(ctx, sq_i, sq_f);
}

static int w_cb(qd_context* ctx, void* user) {
	(void)user;
	return whole_unary(ctx, cb_i, cb_f);
}

static int w_abs(qd_context* ctx, void* user) {
	(void)user;
	return whole_unary(ctx, abs_i, abs_f);
}

static int w_inv(qd_context* ctx, void* user) {
	(void)user;
	double x = 0.0;
	if (!qdos_peek_number(ctx, 0, &x)) {
		return qdos_math_error(ctx, "inv", "NEEDS A NUMBER");
	}
	if (x == 0.0) {
		return qdos_math_error(ctx, "inv", "CANNOT DIVIDE BY 0");
	}

	return replace_top(ctx, 1.0 / x);
}

/** @brief Keeps the value it picked, so an integer stays one */
static int pick_of_two(qd_context* ctx, const char* word, bool want_greater) {
	double b = 0.0, a = 0.0;
	if (!qdos_peek_number(ctx, 0, &b) || !qdos_peek_number(ctx, 1, &a)) {
		return qdos_math_error(ctx, word, "NEEDS 2 NUMBERS");
	}

	qd_stack_element_t top, second;
	if (qd_stack_pop(ctx->st, &top) != QD_STACK_OK || qd_stack_pop(ctx->st, &second) != QD_STACK_OK) {
		return 1;
	}

	const qd_stack_element_t kept = (want_greater == (b > a)) ? top : second;
	return (kept.type == QD_STACK_TYPE_INT) ? qd_push_i(ctx, kept.value.i) : qd_push_f(ctx, kept.value.f);
}

static int w_min(qd_context* ctx, void* user) {
	(void)user;
	return pick_of_two(ctx, "min", false);
}

static int w_max(qd_context* ctx, void* user) {
	(void)user;
	return pick_of_two(ctx, "max", true);
}

/*
 * lib/math has a domain to fall out of and says so by ending the run. The
 * shell survives that now, but a plain refusal reads better than a runtime
 * message, so these check first and keep lib/math's arithmetic.
 */
#define CHECKED(name, domain, rule)                                                                                    \
	static int w_##name(qd_context* ctx, void* user) {                                                                 \
		(void)user;                                                                                                    \
		double x = 0.0;                                                                                                \
		if (qdos_peek_number(ctx, 0, &x) && !(domain))                                                                 \
			return qdos_math_error(ctx, #name, rule);                                                                  \
		return usr_math_##name(ctx);                                                                                   \
	}

CHECKED(sqrt, x >= 0.0, "NEEDS 0 OR MORE")
CHECKED(ln, x > 0.0, "NEEDS MORE THAN 0")
CHECKED(log10, x > 0.0, "NEEDS MORE THAN 0")
CHECKED(log2, x > 0.0, "NEEDS MORE THAN 0")
CHECKED(acosh, x >= 1.0, "NEEDS 1 OR MORE")
CHECKED(atanh, x > -1.0 && x < 1.0, "NEEDS -1 TO 1")
CHECKED(fac, x >= 0.0 && x == floor(x) && x <= 170.0, "WHOLE, 0 TO 170")

#undef CHECKED

/* Degrees in, radians on to lib/math */
#define TRIG_IN(name)                                                                                                  \
	static int w_##name(qd_context* ctx, void* user) {                                                                 \
		(void)user;                                                                                                    \
		double x = 0.0;                                                                                                \
		if (g_degrees && qdos_peek_number(ctx, 0, &x) && replace_top(ctx, x * QDOS_PI / 180.0) != 0)                   \
			return 1;                                                                                                  \
		return usr_math_##name(ctx);                                                                                   \
	}

TRIG_IN(sin)
TRIG_IN(cos)
TRIG_IN(tan)

#undef TRIG_IN

/* Radians out of lib/math, degrees back to the user */
#define TRIG_OUT(name, domain, rule)                                                                                   \
	static int w_##name(qd_context* ctx, void* user) {                                                                 \
		(void)user;                                                                                                    \
		double x = 0.0;                                                                                                \
		if (qdos_peek_number(ctx, 0, &x) && !(domain))                                                                 \
			return qdos_math_error(ctx, #name, rule);                                                                  \
		const int result = usr_math_##name(ctx);                                                                       \
		double radians = 0.0;                                                                                          \
		if (result != 0 || !g_degrees || !qdos_peek_number(ctx, 0, &radians))                                          \
			return result;                                                                                             \
		return replace_top(ctx, radians * 180.0 / QDOS_PI);                                                            \
	}

TRIG_OUT(asin, x >= -1.0 && x <= 1.0, "NEEDS -1 TO 1")
TRIG_OUT(acos, x >= -1.0 && x <= 1.0, "NEEDS -1 TO 1")
TRIG_OUT(atan, true, "")

#undef TRIG_OUT

/* The divisor is the top of the stack, the dividend under it */
static int w_fmod(qd_context* ctx, void* user) {
	(void)user;
	double divisor = 0.0;
	if (qdos_peek_number(ctx, 0, &divisor) && divisor == 0.0) {
		return qdos_math_error(ctx, "fmod", "ZERO DIVISOR");
	}
	return usr_math_fmod(ctx);
}

/*
 * A calculator divides; it does not truncate. `22 7 /` is 3 in Quadrate and
 * that is the language being consistent, but it is the wrong answer on a
 * calculator. An exact result stays an integer so the bitwise words still
 * take it.
 */
static int w_divide(qd_context* ctx, void* user) {
	(void)user;
	double b = 0.0, a = 0.0;
	if (!qdos_peek_number(ctx, 0, &b) || !qdos_peek_number(ctx, 1, &a)) {
		return qdos_math_error(ctx, "divide", "NEEDS 2 NUMBERS");
	}
	if (b == 0.0) {
		return qdos_math_error(ctx, "divide", "ZERO DIVISOR");
	}

	qd_stack_element_t top, second;
	if (qd_stack_pop(ctx->st, &top) != QD_STACK_OK || qd_stack_pop(ctx->st, &second) != QD_STACK_OK) {
		return 1;
	}

	if (top.type == QD_STACK_TYPE_INT && second.type == QD_STACK_TYPE_INT && second.value.i % top.value.i == 0) {
		return qd_push_i(ctx, second.value.i / top.value.i);
	}
	return qd_push_f(ctx, a / b);
}

static int w_pi(qd_context* ctx, void* user) {
	(void)user;
	return qd_push_f(ctx, QDOS_PI);
}

static int w_e(qd_context* ctx, void* user) {
	(void)user;
	return qd_push_f(ctx, 2.71828182845904523536);
}

#define UNARY "(x:f64 -- r:f64)"
#define BINARY "(a:f64 b:f64 -- r:f64)"

static const struct {
	const char* name;
	const char* signature;
	qd_interp_native_fn fn;
} WORDS[] = {
		{"sinh", UNARY, w_sinh},
		{"cosh", UNARY, w_cosh},
		{"tanh", UNARY, w_tanh},
		{"asinh", UNARY, w_asinh},
		{"sq", UNARY, w_sq},
		{"cb", UNARY, w_cb},
		{"cbrt", UNARY, w_cbrt},
		{"exp", UNARY, w_exp},
		{"exp2", UNARY, w_exp2},
		{"ceil", UNARY, w_ceil},
		{"floor", UNARY, w_floor},
		{"round", UNARY, w_round},
		{"trunc", UNARY, w_trunc},
		{"abs", UNARY, w_abs},
		{"sin", UNARY, w_sin},
		{"cos", UNARY, w_cos},
		{"tan", UNARY, w_tan},
		{"asin", UNARY, w_asin},
		{"acos", UNARY, w_acos},
		{"atan", UNARY, w_atan},
		{"sqrt", UNARY, w_sqrt},
		{"ln", UNARY, w_ln},
		{"log10", UNARY, w_log10},
		// What a calculator means by log, so the cap can say it
		{"log", UNARY, w_log10},
		{"log2", UNARY, w_log2},
		{"acosh", UNARY, w_acosh},
		{"atanh", UNARY, w_atanh},
		{"inv", UNARY, w_inv},
		{"fac", UNARY, w_fac},
		{"pow", BINARY, w_pow},
		{"hypot", BINARY, w_hypot},
		{"min", BINARY, w_min},
		{"max", BINARY, w_max},
		{"fmod", BINARY, w_fmod},
		{"divide", BINARY, w_divide},
		{"atan2", BINARY, w_atan2},
		{"pi", "( -- r:f64)", w_pi},
		{"e", "( -- r:f64)", w_e},
};

void qdos_register_math(qd_interp* interp) {
	for (size_t i = 0; i < sizeof(WORDS) / sizeof(*WORDS); i++) {
		qd_interp_register(interp, WORDS[i].name, WORDS[i].signature, WORDS[i].fn, NULL);
	}
}

void qdos_math_visit(qdos_math_visitor visit, void* user) {
	for (size_t i = 0; i < sizeof(WORDS) / sizeof(*WORDS); i++) {
		visit(user, WORDS[i].name, WORDS[i].signature);
	}
}
