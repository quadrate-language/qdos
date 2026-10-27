/**
 * @file mathwords.c
 * @brief Quadrate ships these in lib/math; the interpreter does not register them
 */

#include "mathwords.h"

#include "complex.h"
#include "decimal.h"

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
	} else if (element.type == QD_STACK_TYPE_F128) {
		*out = qd_f128_to_f64(element.value.q);
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

/* 10^x, which a calculator has on shift-log */
static int w_alog(qd_context* ctx, void* user) {
	(void)user;
	double x = 0.0;
	if (!qdos_peek_number(ctx, 0, &x)) {
		return qdos_math_error(ctx, "alog", "NEEDS A NUMBER");
	}
	return replace_top(ctx, pow(10.0, x));
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

/* log10 under the name on the key, so a refusal names the key */
static int w_log(qd_context* ctx, void* user) {
	(void)user;
	double x = 0.0;
	if (qdos_peek_number(ctx, 0, &x) && !(x > 0.0)) {
		return qdos_math_error(ctx, "log", "NEEDS MORE THAN 0");
	}
	return usr_math_log10(ctx);
}

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

/*
 * Degrees in, radians on to lib/math; a whole quarter turn is exact, as a
 * calculator's is. In radians that is a multiple of the double nearest pi/2,
 * so `pi sin` is 0 rather than the 1.2e-16 the rounding of pi leaves.
 */
static int trig_in(qd_context* ctx, trig_kind kind, int (*op)(qd_context*)) {
	double x = 0.0;
	if (!qdos_peek_number(ctx, 0, &x)) {
		return op(ctx);
	}
	const double quarter = g_degrees ? 90.0 : QDOS_PI / 2.0;
	const bool whole = g_degrees ? fmod(x, 90.0) == 0.0 : fabs(x) < 1e6 && x == quarter * nearbyint(x / quarter);
	if (isfinite(x) && whole) {
		static const double SIN[4] = {0.0, 1.0, 0.0, -1.0};
		const int q = (int)fmod(fmod(nearbyint(x / quarter), 4.0) + 4.0, 4.0);
		if (kind == TRIG_TAN && q % 2 == 1) {
			return qdos_math_error(ctx, "tan", "UNDEFINED");
		}
		const double r = (kind == TRIG_SIN) ? SIN[q] : (kind == TRIG_COS) ? SIN[(q + 1) % 4] : 0.0;
		return replace_top(ctx, r);
	}
	if (!g_degrees) {
		return op(ctx);
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

/* The keypad's %, as an HP's: y stays, and x becomes x percent of it */
static int w_percent(qd_context* ctx, void* user) {
	(void)user;
	double x = 0.0, y = 0.0;
	if (!qdos_peek_number(ctx, 0, &x) || !qdos_peek_number(ctx, 1, &y)) {
		return qdos_math_error(ctx, "percent", "NEEDS 2 NUMBERS");
	}
	const double r = y * x / 100.0;
	if (g_finite_only && !isfinite(r)) {
		return qdos_math_error(ctx, "percent", isnan(r) ? "UNDEFINED" : "OVERFLOW");
	}
	return replace_top(ctx, r);
}

/*
 * A calculator's mod, which takes decimals and has
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

/*
 * The words in decimal. With decimal on, a word that has an f128 counterpart computes
 * in it, to 34 digits: sqrt 2 is the decimal128 nearest the root, not a double's 17
 * digits. A word that keeps whole numbers whole still does, when its arguments were
 * whole and the answer is one that fits an i64.
 */

static qd_f128_t q_int(int64_t v) {
	return qd_f128_from_i64(v);
}

static bool q_less(qd_f128_t a, qd_f128_t b) {
	return qd_f128_compare(a, b) == -1;
}

static bool q_equal(qd_f128_t a, qd_f128_t b) {
	return qd_f128_compare(a, b) == 0;
}

static qd_f128_t q_degrees_to_radians(qd_f128_t degrees) {
	return qd_f128_div(qd_f128_mul(degrees, qd_f128_pi()), q_int(180));
}

static qd_f128_t q_radians_out(qd_f128_t radians) {
	return g_degrees ? qd_f128_div(qd_f128_mul(radians, q_int(180)), qd_f128_pi()) : radians;
}

/*
 * As trig_in: a whole quarter turn is exact, in degrees or as a multiple of pi/2 in
 * radians, so `180 sin` and `pi sin` are 0. Returns false, having said why, for the
 * tangent of a right angle.
 */
static bool q_trig(qd_context* ctx, trig_kind kind, qd_f128_t x, qd_f128_t* out) {
	if (qd_f128_is_finite(x)) {
		const qd_f128_t quarter = g_degrees ? q_int(90) : qd_f128_div(qd_f128_pi(), q_int(2));
		const qd_f128_t turns = qd_f128_round_even(qd_f128_div(x, quarter));
		// pi is rounded, and so is pi/2: within a few units in the last place of x is the turn
		const qd_f128_t off = qd_f128_abs(qd_f128_sub(qd_f128_mul(turns, quarter), x));
		const bool on_turn =
				g_degrees ? qd_f128_is_zero(off)
						  : qd_f128_is_zero(off) ||
									q_less(off, qd_f128_scaleb(q_int(4), qd_f128_to_i64(qd_f128_logb(x)) - 33));
		if (on_turn) {
			const int64_t q = ((qd_f128_to_i64(qd_f128_fmod(turns, q_int(4))) % 4) + 4) % 4;
			if (kind == TRIG_TAN && q % 2 == 1) {
				qdos_math_error(ctx, "tan", "UNDEFINED");
				return false;
			}
			static const int64_t SIN[4] = {0, 1, 0, -1};
			*out = q_int(kind == TRIG_SIN ? SIN[q] : kind == TRIG_COS ? SIN[(q + 1) % 4] : 0);
			return true;
		}
	}
	qd_f128_t radians = x;
	if (g_degrees) {
		qd_f128_t turn = qd_f128_fmod(x, q_int(360));
		if (qd_f128_is_signed(turn)) {
			turn = qd_f128_add(turn, q_int(360));
		}
		radians = q_degrees_to_radians(turn);
	}
	*out = kind == TRIG_SIN ? qd_f128_sin(radians) : kind == TRIG_COS ? qd_f128_cos(radians) : qd_f128_tan(radians);
	return true;
}

// A word given whole numbers keeps a whole answer whole
static bool keeps_integers(const char* name) {
	static const char* const WORDS_KEEPING[] = {"sq", "cb", "abs", "ceil", "floor", "round", "trunc", "min", "max",
			"divide", "modulo", "plus", "minus", "times", "fac", "fmod", "pow"};
	for (size_t i = 0; i < sizeof(WORDS_KEEPING) / sizeof(*WORDS_KEEPING); i++) {
		if (strcmp(name, WORDS_KEEPING[i]) == 0) {
			return true;
		}
	}
	return false;
}

/*
 * The word on f128 arguments, the deeper one being a. Returns -1 for a word with no
 * decimal counterpart, which then goes through qdos_decimal_call(); otherwise what a
 * native returns, the result pushed or the reason set.
 */
static int decimal_word(qd_context* ctx, const char* name, size_t arity) {
	qd_f128_t a = q_int(0), b = q_int(0);
	if ((arity >= 1 && !qdos_decimal_peek(ctx, 0, arity == 2 ? &b : &a)) ||
			(arity == 2 && !qdos_decimal_peek(ctx, 1, &a))) {
		return qdos_math_error(ctx, name, arity == 2 ? "NEEDS 2 NUMBERS" : "NEEDS A NUMBER");
	}
	const qd_f128_t x = a;
	const qd_f128_t zero = q_int(0), one = q_int(1);
	qd_f128_t r;

	if (strcmp(name, "sq") == 0) {
		r = qd_f128_mul(x, x);
	} else if (strcmp(name, "cb") == 0) {
		r = qd_f128_mul(qd_f128_mul(x, x), x);
	} else if (strcmp(name, "abs") == 0) {
		r = qd_f128_abs(x);
	} else if (strcmp(name, "ceil") == 0) {
		r = qd_f128_ceil(x);
	} else if (strcmp(name, "floor") == 0) {
		r = qd_f128_floor(x);
	} else if (strcmp(name, "round") == 0) {
		r = qd_f128_round(x);
	} else if (strcmp(name, "trunc") == 0) {
		r = qd_f128_trunc(x);
	} else if (strcmp(name, "sqrt") == 0) {
		if (q_less(x, zero)) {
			return qdos_math_error(ctx, name, "NEEDS 0 OR MORE");
		}
		r = qd_f128_sqrt(x);
	} else if (strcmp(name, "ln") == 0 || strcmp(name, "log10") == 0 || strcmp(name, "log") == 0) {
		if (!q_less(zero, x)) {
			return qdos_math_error(ctx, name, "NEEDS MORE THAN 0");
		}
		r = strcmp(name, "ln") == 0 ? qd_f128_ln(x) : qd_f128_log10(x);
	} else if (strcmp(name, "exp") == 0) {
		r = qd_f128_exp(x);
	} else if (strcmp(name, "alog") == 0) {
		r = qd_f128_pow(q_int(10), x);
	} else if (strcmp(name, "inv") == 0) {
		if (qd_f128_is_zero(x)) {
			return qdos_math_error(ctx, name, "CANNOT DIVIDE BY 0");
		}
		r = qd_f128_div(one, x);
	} else if (strcmp(name, "fac") == 0) {
		if (!q_equal(qd_f128_trunc(x), x) || q_less(x, zero) || q_less(q_int(1000), x)) {
			return qdos_math_error(ctx, name, "WHOLE, 0 TO 1000");
		}
		r = one;
		for (int64_t k = 2, n = qd_f128_to_i64(x); k <= n; k++) {
			r = qd_f128_mul(r, q_int(k));
		}
	} else if (strcmp(name, "sin") == 0 || strcmp(name, "cos") == 0 || strcmp(name, "tan") == 0) {
		const trig_kind kind = name[0] == 's' ? TRIG_SIN : name[0] == 'c' ? TRIG_COS : TRIG_TAN;
		if (!q_trig(ctx, kind, x, &r)) {
			return 1;
		}
	} else if (strcmp(name, "asin") == 0 || strcmp(name, "acos") == 0) {
		if (q_less(x, q_int(-1)) || q_less(one, x)) {
			return qdos_math_error(ctx, name, "NEEDS -1 TO 1");
		}
		r = q_radians_out(name[1] == 's' ? qd_f128_asin(x) : qd_f128_acos(x));
	} else if (strcmp(name, "atan") == 0) {
		r = q_radians_out(qd_f128_atan(x));
	} else if (strcmp(name, "pow") == 0) {
		r = qd_f128_pow(a, b);
	} else if (strcmp(name, "hypot") == 0) {
		r = qd_f128_sqrt(qd_f128_add(qd_f128_mul(a, a), qd_f128_mul(b, b)));
	} else if (strcmp(name, "min") == 0 || strcmp(name, "max") == 0) {
		// Keeps the value it picked, so an integer stays one
		qd_stack_element_t top, second;
		if (qd_stack_pop(ctx->st, &top) != QD_STACK_OK || qd_stack_pop(ctx->st, &second) != QD_STACK_OK) {
			return 1;
		}
		const qd_stack_element_t kept = (strcmp(name, "max") == 0) == q_less(a, b) ? top : second;
		return (kept.type == QD_STACK_TYPE_INT)	   ? qd_push_i(ctx, kept.value.i)
			   : (kept.type == QD_STACK_TYPE_F128) ? qd_push_q(ctx, kept.value.q)
												   : qd_push_f(ctx, kept.value.f);
	} else if (strcmp(name, "fmod") == 0 || strcmp(name, "divide") == 0 || strcmp(name, "modulo") == 0) {
		if (qd_f128_is_zero(b)) {
			return qdos_math_error(ctx, name, "ZERO DIVISOR");
		}
		if (strcmp(name, "divide") == 0) {
			r = qd_f128_div(a, b);
		} else {
			r = qd_f128_fmod(a, b);
			// modulo has the sign of the divisor
			if (name[0] == 'm' && !qd_f128_is_zero(r) && qd_f128_is_signed(r) != qd_f128_is_signed(b)) {
				r = qd_f128_add(r, b);
			}
		}
	} else if (strcmp(name, "plus") == 0) {
		r = qd_f128_add(a, b);
	} else if (strcmp(name, "minus") == 0) {
		r = qd_f128_sub(a, b);
	} else if (strcmp(name, "times") == 0) {
		r = qd_f128_mul(a, b);
	} else if (strcmp(name, "atan2") == 0) {
		r = q_radians_out(qd_f128_atan2(a, b));
	} else if (strcmp(name, "pi") == 0) {
		r = qd_f128_pi();
	} else if (strcmp(name, "e") == 0) {
		r = qd_f128_e();
	} else {
		return -1;
	}

	qd_stack_element_t discard;
	for (size_t k = 0; k < arity; k++) {
		qd_stack_pop(ctx->st, &discard);
	}
	return qd_push_q(ctx, r);
}

/* The keypad's % in decimal: y stays as it was, and x becomes x percent of it */
static int decimal_percent(qd_context* ctx) {
	qd_f128_t x, y;
	if (!qdos_decimal_peek(ctx, 0, &x) || !qdos_decimal_peek(ctx, 1, &y)) {
		return qdos_math_error(ctx, "percent", "NEEDS 2 NUMBERS");
	}
	qd_stack_element_t discard;
	qd_stack_pop(ctx->st, &discard);
	return qd_push_q(ctx, qd_f128_div(qd_f128_mul(y, x), q_int(100)));
}

/* An f128 that is a whole number an i64 holds, as that i64 */
static void whole_to_integer(qd_context* ctx) {
	qd_stack* st = ctx->st;
	qd_stack_element_t* top = &st->data[st->size - 1];
	if (top->type != QD_STACK_TYPE_F128 || !qd_f128_is_finite(top->value.q)) {
		return;
	}
	const qd_f128_t q = top->value.q;
	const int64_t i = qd_f128_to_i64(q);
	if (q_equal(qd_f128_trunc(q), q) && q_equal(q_int(i), q)) {
		top->value.i = i;
		top->type = QD_STACK_TYPE_INT;
	}
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
		{"log", UNARY, w_log},
		{"log2", UNARY, w_log2},
		{"alog", UNARY, w_alog},
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
		{"percent", "(y:f64 x:f64 -- y:f64 r:f64)", w_percent},
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
	bool numbers = depth >= arity;
	bool whole = true;
	for (size_t k = 0; numbers && k < arity; k++) {
		numbers = qd_stack_element(ctx->st, depth - arity + k, &args[k]) == QD_STACK_OK &&
				  (args[k].type == QD_STACK_TYPE_INT || args[k].type == QD_STACK_TYPE_FLOAT ||
						  args[k].type == QD_STACK_TYPE_F128);
		whole = whole && args[k].type == QD_STACK_TYPE_INT;
	}

	int result;
	if (!qdos_decimal()) {
		result = WORDS[i].fn(ctx, NULL);
	} else if (strcmp(WORDS[i].name, "percent") == 0) {
		result = decimal_percent(ctx);
	} else {
		result = numbers ? decimal_word(ctx, WORDS[i].name, arity) : -1;
		if (result == -1) {
			result = qdos_decimal_call(ctx, arity, WORDS[i].fn, NULL);
		} else if (result == 0 && (whole || strcmp(WORDS[i].name, "fac") == 0) && keeps_integers(WORDS[i].name)) {
			whole_to_integer(ctx);
		}
	}

	// From the keypad a result with no finite value is refused, and the arguments put back
	if (result != 0 || !numbers || !g_finite_only || qd_stack_size(ctx->st) == 0) {
		return result;
	}
	const qd_stack_element_t* top = &ctx->st->data[qd_stack_size(ctx->st) - 1];
	bool finite = true, nan = false;
	if (top->type == QD_STACK_TYPE_FLOAT) {
		finite = isfinite(top->value.f);
		nan = isnan(top->value.f);
	} else if (top->type == QD_STACK_TYPE_F128) {
		finite = qd_f128_is_finite(top->value.q);
		nan = qd_f128_is_nan(top->value.q);
	}
	if (finite) {
		return result;
	}

	qd_stack_element_t discard;
	qd_stack_pop(ctx->st, &discard);
	for (size_t k = 0; k < arity; k++) {
		if (args[k].type == QD_STACK_TYPE_INT) {
			qd_push_i(ctx, args[k].value.i);
		} else if (args[k].type == QD_STACK_TYPE_F128) {
			qd_push_q(ctx, args[k].value.q);
		} else {
			qd_push_f(ctx, args[k].value.f);
		}
	}
	return qdos_math_error(ctx, WORDS[i].name, nan ? "UNDEFINED" : "OVERFLOW");
}

void qdos_register_math(qd_interp* interp) {
	for (size_t i = 0; i < WORD_COUNT; i++) {
		qd_interp_register(interp, WORDS[i].name, WORDS[i].signature, guarded, (void*)(uintptr_t)i);
	}
}
