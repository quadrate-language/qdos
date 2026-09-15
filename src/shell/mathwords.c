/**
 * @file mathwords.c
 * @brief Quadrate ships these in lib/math; the interpreter does not register them
 */

#include "mathwords.h"

#include <quadrate/math/math.h>
#include <quadrate/rt/context.h>
#include <quadrate/rt/runtime.h>
#include <quadrate/rt/stack.h>

#include <math.h>
#include <stdbool.h>
#include <stddef.h>

/* usr_math_* take only a context; a native takes userdata as well */
#define WRAP(name) \
	static int w_##name(qd_context* ctx, void* user) { \
		(void)user; \
		return usr_math_##name(ctx); \
	}

WRAP(sin)
WRAP(cos)
WRAP(tan)
WRAP(atan)
WRAP(sinh)
WRAP(cosh)
WRAP(tanh)
WRAP(asinh)
WRAP(sq)
WRAP(cb)
WRAP(cbrt)
WRAP(exp)
WRAP(exp2)
WRAP(ceil)
WRAP(floor)
WRAP(round)
WRAP(trunc)
WRAP(abs)
WRAP(atan2)
WRAP(pow)
WRAP(hypot)
WRAP(min)
WRAP(max)

#undef WRAP

/** @brief Argument @p depth from the top as a double, ints included; false if absent */
static bool peek_number(qd_context* ctx, size_t depth, double* out) {
	const size_t size = qd_stack_size(ctx->st);
	if (depth >= size)
		return false;

	qd_stack_element_t element;
	if (qd_stack_element(ctx->st, size - 1 - depth, &element) != QD_STACK_OK)
		return false;

	if (element.type == QD_STACK_TYPE_INT)
		*out = (double)element.value.i;
	else if (element.type == QD_STACK_TYPE_FLOAT)
		*out = element.value.f;
	else
		return false;
	return true;
}

/*
 * lib/math treats a domain error as fatal and aborts the process. A calculator
 * cannot die because someone pressed LN once too often, so these check the
 * argument first and raise an ordinary error. Anything they let through is
 * still lib/math's to compute, including how it coerces an integer.
 */
#define CHECKED(name, domain, rule) \
	static int w_##name(qd_context* ctx, void* user) { \
		(void)user; \
		double x = 0.0; \
		if (peek_number(ctx, 0, &x) && !(domain)) { \
			qd_set_error_msg(ctx, #name ": " rule); \
			return 1; \
		} \
		return usr_math_##name(ctx); \
	}

CHECKED(sqrt, x >= 0.0, "NEEDS 0 OR MORE")
CHECKED(ln, x > 0.0, "NEEDS MORE THAN 0")
CHECKED(log10, x > 0.0, "NEEDS MORE THAN 0")
CHECKED(log2, x > 0.0, "NEEDS MORE THAN 0")
CHECKED(asin, x >= -1.0 && x <= 1.0, "NEEDS -1 TO 1")
CHECKED(acos, x >= -1.0 && x <= 1.0, "NEEDS -1 TO 1")
CHECKED(acosh, x >= 1.0, "NEEDS 1 OR MORE")
CHECKED(atanh, x > -1.0 && x < 1.0, "NEEDS BETWEEN -1 AND 1")
CHECKED(inv, x != 0.0, "CANNOT DIVIDE BY 0")
CHECKED(fac, x >= 0.0 && x == floor(x) && x <= 170.0, "NEEDS A WHOLE NUMBER 0 TO 170")

#undef CHECKED

/* The divisor is the top of the stack, the dividend under it */
static int w_fmod(qd_context* ctx, void* user) {
	(void)user;
	double divisor = 0.0;
	if (peek_number(ctx, 0, &divisor) && divisor == 0.0) {
		qd_set_error_msg(ctx, "fmod: CANNOT DIVIDE BY 0");
		return 1;
	}
	return usr_math_fmod(ctx);
}

#define UNARY "(x:f64 -- r:f64)"
#define BINARY "(a:f64 b:f64 -- r:f64)"

void qdos_register_math(qd_interp* interp) {
	static const struct {
		const char* name;
		const char* signature;
		qd_interp_native_fn fn;
	} WORDS[] = {
			{"sin", UNARY, w_sin},
			{"cos", UNARY, w_cos},
			{"tan", UNARY, w_tan},
			{"atan", UNARY, w_atan},
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
			{"sqrt", UNARY, w_sqrt},
			{"ln", UNARY, w_ln},
			{"log10", UNARY, w_log10},
			{"log2", UNARY, w_log2},
			{"asin", UNARY, w_asin},
			{"acos", UNARY, w_acos},
			{"acosh", UNARY, w_acosh},
			{"atanh", UNARY, w_atanh},
			{"inv", UNARY, w_inv},
			{"fac", UNARY, w_fac},
			{"atan2", BINARY, w_atan2},
			{"pow", BINARY, w_pow},
			{"hypot", BINARY, w_hypot},
			{"min", BINARY, w_min},
			{"max", BINARY, w_max},
			{"fmod", BINARY, w_fmod},
	};

	for (size_t i = 0; i < sizeof(WORDS) / sizeof(*WORDS); i++)
		qd_interp_register(interp, WORDS[i].name, WORDS[i].signature, WORDS[i].fn, NULL);
}
