/**
 * @file mathwords.c
 * @brief Quadrate ships these in lib/math; the interpreter does not register them
 */

#include "mathwords.h"

#include "complex.h"

#include <quadrate/math/math.h>
#include <quadrate/rt/runtime.h>
#include <quadrate/rt/stack.h>

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define QDOS_PI 3.14159265358979323846

static bool g_degrees;
static bool g_finite_only;

void qdos_math_set_degrees(bool degrees) {
	g_degrees = degrees;
}

bool qdos_math_degrees(void) {
	return g_degrees;
}

void qdos_math_set_finite_only(bool on) {
	g_finite_only = on;
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
static int whole_unary(qd_context* ctx, bool (*on_int)(int64_t, int64_t*), double (*on_real)(double)) {
	qd_stack_element_t top;
	if (qd_stack_pop(ctx->st, &top) != QD_STACK_OK) {
		return 1;
	}

	// Too big to stay whole goes on as a float, as a calculator's numbers do
	if (top.type == QD_STACK_TYPE_INT) {
		int64_t r;
		return on_int(top.value.i, &r) ? qd_push_i(ctx, r) : qd_push_f(ctx, on_real((double)top.value.i));
	}
	if (top.type == QD_STACK_TYPE_FLOAT) {
		return qd_push_f(ctx, on_real(top.value.f));
	}

	return qdos_math_error(ctx, "math", "NEEDS A NUMBER");
}

static bool sq_i(int64_t x, int64_t* r) {
	return !__builtin_mul_overflow(x, x, r);
}

static double sq_f(double x) {
	return x * x;
}

static bool cb_i(int64_t x, int64_t* r) {
	int64_t s;
	return !__builtin_mul_overflow(x, x, &s) && !__builtin_mul_overflow(s, x, r);
}

static double cb_f(double x) {
	return x * x * x;
}

static bool abs_i(int64_t x, int64_t* r) {
	*r = x < 0 ? -x : x;
	return x != INT64_MIN;
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

#undef CHECKED

/* Whole numbers to 20 as lib/math has them; past that a float, which reaches 170 */
static int w_fac(qd_context* ctx, void* user) {
	(void)user;
	double x = 0.0;
	if (!qdos_peek_number(ctx, 0, &x)) {
		return usr_math_fac(ctx);
	}
	if (!(x >= 0.0 && x == floor(x) && x <= 170.0)) {
		return qdos_math_error(ctx, "fac", "WHOLE, 0 TO 170");
	}
	if (x <= 20.0) {
		// lib/math takes an integer only, and 5.0 is whole
		qd_stack_element_t discard;
		if (qd_stack_pop(ctx->st, &discard) != QD_STACK_OK || qd_push_i(ctx, (int64_t)x) != 0) {
			return 1;
		}
		return usr_math_fac(ctx);
	}
	double r = 1.0;
	for (double k = 2.0; k <= x; k++) {
		r *= k;
	}
	return replace_top(ctx, r);
}

typedef enum {
	TRIG_SIN,
	TRIG_COS,
	TRIG_TAN
} trig_kind;

/* Degrees in, radians on to lib/math; a whole quarter turn is exact, as a calculator's is */
static int trig_in(qd_context* ctx, trig_kind kind, int (*op)(qd_context*)) {
	double x = 0.0;
	if (!g_degrees || !qdos_peek_number(ctx, 0, &x)) {
		return op(ctx);
	}
	if (isfinite(x) && fmod(x, 90.0) == 0.0) {
		static const double SIN[4] = {0.0, 1.0, 0.0, -1.0};
		const int q = (int)fmod(fmod(x / 90.0, 4.0) + 4.0, 4.0);
		if (kind == TRIG_TAN && q % 2 == 1) {
			return qdos_math_error(ctx, "tan", "UNDEFINED");
		}
		const double r = (kind == TRIG_SIN) ? SIN[q] : (kind == TRIG_COS) ? SIN[(q + 1) % 4] : 0.0;
		return replace_top(ctx, r);
	}
	// Within one turn first, or 390 loses digits that 30 keeps
	const double turn = fmod(fmod(x, 360.0) + 360.0, 360.0);
	if (replace_top(ctx, turn * QDOS_PI / 180.0) != 0) {
		return 1;
	}
	return op(ctx);
}

static int w_sin(qd_context* ctx, void* user) {
	(void)user;
	return trig_in(ctx, TRIG_SIN, usr_math_sin);
}

static int w_cos(qd_context* ctx, void* user) {
	(void)user;
	return trig_in(ctx, TRIG_COS, usr_math_cos);
}

static int w_tan(qd_context* ctx, void* user) {
	(void)user;
	return trig_in(ctx, TRIG_TAN, usr_math_tan);
}

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

	// The least integer over -1 is one past the greatest, and traps rather than wraps
	if (top.type == QD_STACK_TYPE_INT && second.type == QD_STACK_TYPE_INT &&
			!(second.value.i == INT64_MIN && top.value.i == -1) && second.value.i % top.value.i == 0) {
		return qd_push_i(ctx, second.value.i / top.value.i);
	}
	return qd_push_f(ctx, a / b);
}

/*
 * What the keypad's % does: a calculator's mod, which takes decimals and has
 * the sign of the divisor, so `-7 3 modulo` is 2 where Quadrate's mod gives -1.
 * Whole numbers stay whole.
 */
static int w_modulo(qd_context* ctx, void* user) {
	(void)user;
	double b = 0.0, a = 0.0;
	if (!qdos_peek_number(ctx, 0, &b) || !qdos_peek_number(ctx, 1, &a)) {
		return qdos_math_error(ctx, "modulo", "NEEDS 2 NUMBERS");
	}
	if (b == 0.0) {
		return qdos_math_error(ctx, "modulo", "ZERO DIVISOR");
	}

	qd_stack_element_t top, second;
	if (qd_stack_pop(ctx->st, &top) != QD_STACK_OK || qd_stack_pop(ctx->st, &second) != QD_STACK_OK) {
		return 1;
	}

	if (top.type == QD_STACK_TYPE_INT && second.type == QD_STACK_TYPE_INT) {
		// Anything over -1 leaves nothing, and the least integer over it traps
		int64_t r = (top.value.i == -1) ? 0 : second.value.i % top.value.i;
		if (r != 0 && (r < 0) != (top.value.i < 0)) {
			r += top.value.i;
		}
		return qd_push_i(ctx, r);
	}

	double r = fmod(a, b);
	if (r != 0.0 && (r < 0.0) != (b < 0.0)) {
		r += b;
	}
	// A remainder too small to add to b without rounding comes out as b itself
	return qd_push_f(ctx, (r == b) ? 0.0 : r);
}

typedef enum {
	ARITH_PLUS,
	ARITH_MINUS,
	ARITH_TIMES
} arith_kind;

/* What the keypad's + - x do: Quadrate's, except that an integer too big to stay one is a float */
static int arith(qd_context* ctx, const char* word, arith_kind kind) {
	double b = 0.0, a = 0.0;
	if (!qdos_peek_number(ctx, 0, &b) || !qdos_peek_number(ctx, 1, &a)) {
		return qdos_math_error(ctx, word, "NEEDS 2 NUMBERS");
	}
	qd_stack_element_t top, second;
	if (qd_stack_pop(ctx->st, &top) != QD_STACK_OK || qd_stack_pop(ctx->st, &second) != QD_STACK_OK) {
		return 1;
	}
	if (top.type == QD_STACK_TYPE_INT && second.type == QD_STACK_TYPE_INT) {
		int64_t r;
		const bool over = (kind == ARITH_PLUS)	  ? __builtin_add_overflow(second.value.i, top.value.i, &r)
						  : (kind == ARITH_MINUS) ? __builtin_sub_overflow(second.value.i, top.value.i, &r)
												  : __builtin_mul_overflow(second.value.i, top.value.i, &r);
		if (!over) {
			return qd_push_i(ctx, r);
		}
	}
	return qd_push_f(ctx, (kind == ARITH_PLUS) ? a + b : (kind == ARITH_MINUS) ? a - b : a * b);
}

static int w_plus(qd_context* ctx, void* user) {
	(void)user;
	return arith(ctx, "plus", ARITH_PLUS);
}

static int w_minus(qd_context* ctx, void* user) {
	(void)user;
	return arith(ctx, "minus", ARITH_MINUS);
}

static int w_times(qd_context* ctx, void* user) {
	(void)user;
	return arith(ctx, "times", ARITH_TIMES);
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
		{"modulo", BINARY, w_modulo},
		{"plus", BINARY, w_plus},
		{"minus", BINARY, w_minus},
		{"times", BINARY, w_times},
		{"atan2", BINARY, w_atan2},
		{"pi", "( -- r:f64)", w_pi},
		{"e", "( -- r:f64)", w_e},
};

#define WORD_COUNT (sizeof(WORDS) / sizeof(*WORDS))

/*
 * Every word through here, so that from the keypad a result with no finite
 * value is refused and its arguments put back. They are numbers or the word
 * would have failed, so putting them back is copying them.
 */
static int guarded(qd_context* ctx, void* user) {
	const size_t i = (size_t)(uintptr_t)user;
	const size_t arity = (strcmp(WORDS[i].signature, UNARY) == 0)	 ? 1
						 : (strcmp(WORDS[i].signature, BINARY) == 0) ? 2
																	 : 0;
	int answered;
	if (qdos_cpx_apply(ctx, WORDS[i].name, arity, &answered)) {
		return answered;
	}

	const size_t depth = qd_stack_size(ctx->st);
	qd_stack_element_t args[2];
	bool numbers = g_finite_only && depth >= arity;
	for (size_t k = 0; numbers && k < arity; k++) {
		numbers = qd_stack_element(ctx->st, depth - arity + k, &args[k]) == QD_STACK_OK &&
				  (args[k].type == QD_STACK_TYPE_INT || args[k].type == QD_STACK_TYPE_FLOAT);
	}

	const int result = WORDS[i].fn(ctx, NULL);
	double r = 0.0;
	if (result != 0 || !numbers || !qdos_peek_number(ctx, 0, &r) || isfinite(r)) {
		return result;
	}

	qd_stack_element_t discard;
	qd_stack_pop(ctx->st, &discard);
	for (size_t k = 0; k < arity; k++) {
		if (args[k].type == QD_STACK_TYPE_INT) {
			qd_push_i(ctx, args[k].value.i);
		} else {
			qd_push_f(ctx, args[k].value.f);
		}
	}
	return qdos_math_error(ctx, WORDS[i].name, isnan(r) ? "UNDEFINED" : "OVERFLOW");
}

void qdos_register_math(qd_interp* interp) {
	for (size_t i = 0; i < WORD_COUNT; i++) {
		qd_interp_register(interp, WORDS[i].name, WORDS[i].signature, guarded, (void*)(uintptr_t)i);
	}
}
