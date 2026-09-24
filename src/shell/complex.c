/**
 * @file complex.c
 * @brief Complex numbers, held as runtime objects the calculator recognises
 */

#include "complex.h"

#include "mathwords.h"

#include <quadrate/rt/qd_struct.h>
#include <quadrate/rt/runtime.h>

#include <complex.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define QDOS_PI 3.14159265358979323846

typedef struct {
	double re, im;
} cpx;

static qdos_cpx_mode g_mode;

void qdos_cpx_set_mode(qdos_cpx_mode mode) {
	g_mode = (mode >= 0 && mode < QDOS_CPX__COUNT) ? mode : QDOS_CPX_REAL;
}

qdos_cpx_mode qdos_cpx_get_mode(void) {
	return g_mode;
}

/* Nothing to free; its address is what marks an object as one of these */
static void cpx_destroy(void* object) {
	(void)object;
}

bool qdos_cpx_of(const qd_stack_element_t* element, double* re, double* im) {
	if (element->type != QD_STACK_TYPE_PTR || element->value.p == NULL || !qd_struct_is_valid(element->value.p)) {
		return false;
	}
	const qd_struct_header_t* header = qd_struct_get_header(element->value.p);
	if (header == NULL || header->destructor != cpx_destroy) {
		return false;
	}
	const cpx* z = (const cpx*)element->value.p;
	*re = z->re;
	*im = z->im;
	return true;
}

int qdos_cpx_push(qd_context* ctx, double re, double im) {
	// What rounding leaves of a part that should be nought: i squared is -1, not -1+1e-16i
	if (fabs(im) <= 1e-13 * fabs(re)) {
		im = 0.0;
	}
	if (fabs(re) <= 1e-13 * fabs(im)) {
		re = 0.0;
	}
	if (im == 0.0) {
		return qd_push_f(ctx, re);
	}

	cpx* z = (cpx*)qd_struct_alloc(sizeof(cpx), cpx_destroy);
	if (z == NULL) {
		qd_set_error_msg(ctx, "OUT OF MEMORY");
		return 1;
	}
	z->re = re;
	z->im = im;

	// The one reference it was made with passes to the stack, as an array's does
	if (qd_push_p(ctx, z) != 0) {
		qd_struct_release(z);
		return 1;
	}
	return 0;
}

/* Argument @p depth as a complex number, a real one included; false for anything else */
static bool peek(qd_context* ctx, size_t depth, double complex* out, bool* is_complex) {
	const size_t size = qd_stack_size(ctx->st);
	qd_stack_element_t element;
	if (depth >= size || qd_stack_element(ctx->st, size - 1 - depth, &element) != QD_STACK_OK) {
		return false;
	}

	double re, im;
	*is_complex = qdos_cpx_of(&element, &re, &im);
	if (*is_complex) {
		*out = CMPLX(re, im);
		return true;
	}
	if (element.type == QD_STACK_TYPE_INT) {
		*out = CMPLX((double)element.value.i, 0.0);
		return true;
	}
	if (element.type == QD_STACK_TYPE_FLOAT) {
		*out = CMPLX(element.value.f, 0.0);
		return true;
	}
	return false;
}

static void drop(qd_context* ctx, size_t count) {
	for (size_t k = 0; k < count; k++) {
		qd_stack_element_t element;
		if (qd_stack_pop(ctx->st, &element) != QD_STACK_OK) {
			return;
		}
		if (element.type == QD_STACK_TYPE_PTR) {
			qd_ptr_release(element.value.p);
		}
	}
}

/* The arguments go only once the answer is known to be one */
static int answer(qd_context* ctx, const char* word, size_t arity, double complex z) {
	if (!isfinite(creal(z)) || !isfinite(cimag(z))) {
		return qdos_math_error(ctx, word, (isnan(creal(z)) || isnan(cimag(z))) ? "UNDEFINED" : "OVERFLOW");
	}
	drop(ctx, arity);
	return qdos_cpx_push(ctx, creal(z), cimag(z));
}

static int real_answer(qd_context* ctx, size_t arity, double x) {
	drop(ctx, arity);
	return qd_push_f(ctx, x);
}

/* Whether a real argument leaves the reals here, and the mode lets it */
static bool escapes(const char* word, double complex a, double complex b) {
	if (g_mode == QDOS_CPX_REAL) {
		return false;
	}
	if (strcmp(word, "sqrt") == 0) {
		return creal(a) < 0.0;
	}
	if (strcmp(word, "ln") == 0 || strcmp(word, "log") == 0 || strcmp(word, "log10") == 0) {
		return creal(a) < 0.0;
	}
	if (strcmp(word, "pow") == 0) {
		return creal(a) < 0.0 && creal(b) != floor(creal(b));
	}
	return false;
}

bool qdos_cpx_apply(qd_context* ctx, const char* word, size_t arity, int* result) {
	if (arity < 1 || arity > 2) {
		return false;
	}

	// For two, a is under b as the word reads them: `a b divide` is a over b
	double complex a = 0, b = 0;
	bool a_cpx = false, b_cpx = false;
	if (arity == 1) {
		if (!peek(ctx, 0, &a, &a_cpx)) {
			return false;
		}
	} else if (!peek(ctx, 0, &b, &b_cpx) || !peek(ctx, 1, &a, &a_cpx)) {
		return false;
	}
	if (!a_cpx && !b_cpx && !escapes(word, a, b)) {
		return false;
	}

	if (strcmp(word, "plus") == 0) {
		*result = answer(ctx, word, 2, a + b);
	} else if (strcmp(word, "minus") == 0) {
		*result = answer(ctx, word, 2, a - b);
	} else if (strcmp(word, "times") == 0) {
		*result = answer(ctx, word, 2, a * b);
	} else if (strcmp(word, "divide") == 0) {
		*result = (b == 0) ? qdos_math_error(ctx, word, "ZERO DIVISOR") : answer(ctx, word, 2, a / b);
	} else if (strcmp(word, "pow") == 0) {
		*result = (a == 0 && creal(b) <= 0.0) ? qdos_math_error(ctx, word, "0 TO THAT IS UNDEFINED")
											  : answer(ctx, word, 2, cpow(a, b));
	} else if (strcmp(word, "sq") == 0) {
		*result = answer(ctx, word, 1, a * a);
	} else if (strcmp(word, "inv") == 0) {
		*result = (a == 0) ? qdos_math_error(ctx, word, "CANNOT DIVIDE BY 0") : answer(ctx, word, 1, 1.0 / a);
	} else if (strcmp(word, "abs") == 0) {
		*result = real_answer(ctx, 1, cabs(a));
	} else if (strcmp(word, "sqrt") == 0) {
		*result = answer(ctx, word, 1, csqrt(a));
	} else if (strcmp(word, "exp") == 0) {
		*result = answer(ctx, word, 1, cexp(a));
	} else if (strcmp(word, "ln") == 0) {
		*result = (a == 0) ? qdos_math_error(ctx, word, "NEEDS MORE THAN 0") : answer(ctx, word, 1, clog(a));
	} else if (strcmp(word, "log") == 0 || strcmp(word, "log10") == 0) {
		*result =
				(a == 0) ? qdos_math_error(ctx, word, "NEEDS MORE THAN 0") : answer(ctx, word, 1, clog(a) / log(10.0));
	} else {
		*result = qdos_math_error(ctx, word, "NOT FOR COMPLEX");
	}
	return true;
}

static double angle_out(double radians) {
	return qdos_math_degrees() ? radians * 180.0 / QDOS_PI : radians;
}

/* One real, trimmed to @p digits significant ones */
static void real_text(double x, int digits, char* out, size_t cap) {
	snprintf(out, cap, "%.*g", digits, x);
}

void qdos_cpx_format(double re, double im, char* out, size_t cap, size_t room) {
	for (int digits = 10; digits >= 2; digits--) {
		char a[32], b[32];
		if (g_mode == QDOS_CPX_POLAR) {
			real_text(hypot(re, im), digits, a, sizeof(a));
			real_text(angle_out(atan2(im, re)), digits, b, sizeof(b));
			snprintf(out, cap, "%se^(%si)", a, b);
		} else {
			// A unit imaginary part is written i, as a TI writes it
			const double mag = fabs(im);
			if (mag == 1.0) {
				b[0] = '\0';
			} else {
				real_text(mag, digits, b, sizeof(b));
			}
			if (re == 0.0) {
				snprintf(out, cap, "%s%si", im < 0.0 ? "-" : "", b);
			} else {
				real_text(re, digits, a, sizeof(a));
				snprintf(out, cap, "%s%c%si", a, im < 0.0 ? '-' : '+', b);
			}
		}
		if (strlen(out) <= room) {
			return;
		}
	}
}

/* ---------------------------------------------------------------------------
 * The words
 * ------------------------------------------------------------------------- */

static bool pop_parts(qd_context* ctx, const char* word, size_t count, double complex* out) {
	for (size_t k = 0; k < count; k++) {
		bool is_complex;
		if (!peek(ctx, count - 1 - k, &out[k], &is_complex)) {
			qdos_math_error(ctx, word, count == 1 ? "NEEDS A NUMBER" : "NEEDS 2 NUMBERS");
			return false;
		}
	}
	return true;
}

/* Two reals, the real part under the imaginary one */
static int w_complex(qd_context* ctx, void* user) {
	(void)user;
	double complex p[2];
	if (!pop_parts(ctx, "complex", 2, p)) {
		return 1;
	}
	if (cimag(p[0]) != 0.0 || cimag(p[1]) != 0.0) {
		return qdos_math_error(ctx, "complex", "NEEDS 2 REAL NUMBERS");
	}
	drop(ctx, 2);
	return qdos_cpx_push(ctx, creal(p[0]), creal(p[1]));
}

static int w_csplit(qd_context* ctx, void* user) {
	(void)user;
	double complex z;
	if (!pop_parts(ctx, "csplit", 1, &z)) {
		return 1;
	}
	drop(ctx, 1);
	qd_push_f(ctx, creal(z));
	return qd_push_f(ctx, cimag(z));
}

/* The angle in the calculator's angle mode */
static int w_polar(qd_context* ctx, void* user) {
	(void)user;
	double complex p[2];
	if (!pop_parts(ctx, "polar", 2, p)) {
		return 1;
	}
	if (cimag(p[0]) != 0.0 || cimag(p[1]) != 0.0) {
		return qdos_math_error(ctx, "polar", "NEEDS 2 REAL NUMBERS");
	}
	const double theta = qdos_math_degrees() ? creal(p[1]) * QDOS_PI / 180.0 : creal(p[1]);
	return answer(ctx, "polar", 2, creal(p[0]) * CMPLX(cos(theta), sin(theta)));
}

static int part(qd_context* ctx, const char* word, int which) {
	double complex z;
	if (!pop_parts(ctx, word, 1, &z)) {
		return 1;
	}
	switch (which) {
	case 0:
		return real_answer(ctx, 1, creal(z));
	case 1:
		return real_answer(ctx, 1, cimag(z));
	case 2:
		return real_answer(ctx, 1, angle_out(carg(z)));
	default:
		return answer(ctx, word, 1, conj(z));
	}
}

static int w_real(qd_context* ctx, void* user) {
	(void)user;
	return part(ctx, "real", 0);
}

static int w_imag(qd_context* ctx, void* user) {
	(void)user;
	return part(ctx, "imag", 1);
}

static int w_angle(qd_context* ctx, void* user) {
	(void)user;
	return part(ctx, "angle", 2);
}

static int w_conj(qd_context* ctx, void* user) {
	(void)user;
	return part(ctx, "conj", 3);
}

static int w_i(qd_context* ctx, void* user) {
	(void)user;
	return qdos_cpx_push(ctx, 0.0, 1.0);
}

void qdos_register_complex(qd_interp* interp) {
	qd_interp_register(interp, "complex", "(re:f64 im:f64 -- z:ptr)", w_complex, NULL);
	qd_interp_register(interp, "csplit", "(z:ptr -- re:f64 im:f64)", w_csplit, NULL);
	qd_interp_register(interp, "polar", "(r:f64 theta:f64 -- z:ptr)", w_polar, NULL);
	qd_interp_register(interp, "real", "(z:ptr -- x:f64)", w_real, NULL);
	qd_interp_register(interp, "imag", "(z:ptr -- y:f64)", w_imag, NULL);
	qd_interp_register(interp, "angle", "(z:ptr -- theta:f64)", w_angle, NULL);
	qd_interp_register(interp, "conj", "(z:ptr -- w:ptr)", w_conj, NULL);
	qd_interp_register(interp, "i", "( -- z:ptr)", w_i, NULL);
}
