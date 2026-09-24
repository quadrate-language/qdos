/**
 * @file numeric.c
 * @brief Roots, extrema, slopes and areas of a function known only by calling it
 */

#include "numeric.h"

#include <float.h>
#include <math.h>

/* How finely [a, b] is looked over before anything is narrowed down. Enough
 * to see each hump of sin over -10..10 several times. */
#define SCAN 64

#define ITERATIONS 100

/* f at SCAN + 1 evenly spaced points across [a, b] */
typedef struct {
	double x[SCAN + 1];
	double y[SCAN + 1];
	bool ok[SCAN + 1];
} scan;

static void scan_take(scan* s, qdos_graph_fn fn, void* user, double a, double b) {
	for (int k = 0; k <= SCAN; k++) {
		s->x[k] = (k == SCAN) ? b : a + (b - a) * k / SCAN;
		s->ok[k] = fn(user, s->x[k], &s->y[k]) && isfinite(s->y[k]);
	}
}

/* Brent's method, on an interval where f changes sign */
static bool brent_root(qdos_graph_fn fn, void* user, double a, double b, double fa, double fb, double* out) {
	double c = b, fc = fb, d = 0.0, e = 0.0;
	for (int i = 0; i < ITERATIONS; i++) {
		if ((fb > 0.0) == (fc > 0.0)) {
			c = a;
			fc = fa;
			d = e = b - a;
		}
		if (fabs(fc) < fabs(fb)) {
			a = b;
			b = c;
			c = a;
			fa = fb;
			fb = fc;
			fc = fa;
		}
		const double tol = 2.0 * DBL_EPSILON * fabs(b) + 1e-300;
		const double m = 0.5 * (c - b);
		if (fabs(m) <= tol || fb == 0.0) {
			*out = b;
			return true;
		}
		if (fabs(e) >= tol && fabs(fa) > fabs(fb)) {
			// Inverse quadratic interpolation, or the secant where it cannot be
			double p, q;
			const double s = fb / fa;
			if (a == c) {
				p = 2.0 * m * s;
				q = 1.0 - s;
			} else {
				const double r = fb / fc, t = fa / fc;
				p = s * (2.0 * m * t * (t - r) - (b - a) * (r - 1.0));
				q = (t - 1.0) * (r - 1.0) * (s - 1.0);
			}
			if (p > 0.0) {
				q = -q;
			}
			p = fabs(p);
			if (2.0 * p < fmin(3.0 * m * q - fabs(tol * q), fabs(e * q))) {
				e = d;
				d = p / q;
			} else {
				d = m;
				e = m;
			}
		} else {
			d = m;
			e = m;
		}
		a = b;
		fa = fb;
		b += (fabs(d) > tol) ? d : (m > 0.0 ? tol : -tol);
		if (!fn(user, b, &fb) || !isfinite(fb)) {
			return false;
		}
	}
	*out = b;
	return true;
}

#define GOLDEN 0.38196601125010515 // 2 - phi

/* Golden-section search for the lowest of sign * f on [a, b] */
static bool golden(qdos_graph_fn fn, void* user, double sign, double a, double b, double* x, double* y) {
	double x1 = a + GOLDEN * (b - a), x2 = b - GOLDEN * (b - a);
	double f1, f2;
	if (!fn(user, x1, &f1) || !fn(user, x2, &f2) || !isfinite(f1) || !isfinite(f2)) {
		return false;
	}
	f1 *= sign;
	f2 *= sign;
	for (int i = 0; i < ITERATIONS && fabs(b - a) > 1e-12 * (1.0 + fabs(x1)); i++) {
		if (f1 <= f2) {
			b = x2;
			x2 = x1;
			f2 = f1;
			x1 = a + GOLDEN * (b - a);
			if (!fn(user, x1, &f1) || !isfinite(f1)) {
				return false;
			}
			f1 *= sign;
		} else {
			a = x1;
			x1 = x2;
			f1 = f2;
			x2 = b - GOLDEN * (b - a);
			if (!fn(user, x2, &f2) || !isfinite(f2)) {
				return false;
			}
			f2 *= sign;
		}
	}
	*x = (f1 <= f2) ? x1 : x2;
	*y = sign * ((f1 <= f2) ? f1 : f2);
	return true;
}

/* |f|, for finding a root that touches the axis without crossing it */
typedef struct {
	qdos_graph_fn fn;
	void* user;
} wrapped;

static bool abs_of(void* user, double x, double* y) {
	const wrapped* w = user;
	if (!w->fn(w->user, x, y)) {
		return false;
	}
	*y = fabs(*y);
	return true;
}

bool qdos_num_root(qdos_graph_fn fn, void* user, double a, double b, double* x) {
	if (a > b) {
		const double t = a;
		a = b;
		b = t;
	}

	scan s;
	scan_take(&s, fn, user, a, b);

	double scale = 1.0;
	for (int k = 0; k <= SCAN; k++) {
		scale = s.ok[k] ? fmax(scale, fabs(s.y[k])) : scale;
	}

	for (int k = 0; k <= SCAN; k++) {
		if (s.ok[k] && s.y[k] == 0.0) {
			*x = s.x[k];
			return true;
		}
		if (k == SCAN || !s.ok[k] || !s.ok[k + 1] || (s.y[k] > 0.0) == (s.y[k + 1] > 0.0)) {
			continue;
		}

		// A change of sign across a pole, as tan's, is not a root: what it
		// narrows down to is huge rather than nought
		double r, fr;
		if (brent_root(fn, user, s.x[k], s.x[k + 1], s.y[k], s.y[k + 1], &r) && fn(user, r, &fr) &&
				fabs(fr) <= 1e-6 * fmax(1.0, fmax(fabs(s.y[k]), fabs(s.y[k + 1])))) {
			*x = r;
			return true;
		}
	}

	// No crossing: the lowest |f| sampled may still touch nought
	int best = -1;
	for (int k = 0; k <= SCAN; k++) {
		if (s.ok[k] && (best < 0 || fabs(s.y[k]) < fabs(s.y[best]))) {
			best = k;
		}
	}
	if (best < 0) {
		return false;
	}
	const int lo = (best > 0) ? best - 1 : 0, hi = (best < SCAN) ? best + 1 : SCAN;
	wrapped w = {fn, user};
	double m, fm;
	if (!golden(abs_of, &w, 1.0, s.x[lo], s.x[hi], &m, &fm) || fm > 1e-10 * scale) {
		return false;
	}
	*x = m;
	return true;
}

static bool extremum(qdos_graph_fn fn, void* user, double sign, double a, double b, double* x, double* y) {
	if (a > b) {
		const double t = a;
		a = b;
		b = t;
	}

	scan s;
	scan_take(&s, fn, user, a, b);
	int best = -1;
	for (int k = 0; k <= SCAN; k++) {
		if (s.ok[k] && (best < 0 || sign * s.y[k] < sign * s.y[best])) {
			best = k;
		}
	}
	if (best < 0) {
		return false;
	}

	*x = s.x[best];
	*y = s.y[best];
	const int lo = (best > 0) ? best - 1 : 0, hi = (best < SCAN) ? best + 1 : SCAN;
	double m, fm;
	if (golden(fn, user, sign, s.x[lo], s.x[hi], &m, &fm) && sign * fm <= sign * *y) {
		*x = m;
		*y = fm;
	}
	return true;
}

bool qdos_num_minimum(qdos_graph_fn fn, void* user, double a, double b, double* x, double* y) {
	return extremum(fn, user, 1.0, a, b, x, y);
}

bool qdos_num_maximum(qdos_graph_fn fn, void* user, double a, double b, double* x, double* y) {
	return extremum(fn, user, -1.0, a, b, x, y);
}

/* The central difference at step h */
static bool central(qdos_graph_fn fn, void* user, double x, double h, double* d) {
	double lo, hi;
	if (!fn(user, x - h, &lo) || !fn(user, x + h, &hi) || !isfinite(lo) || !isfinite(hi)) {
		return false;
	}
	*d = (hi - lo) / (2.0 * h);
	return true;
}

bool qdos_num_derivative(qdos_graph_fn fn, void* user, double x, double* d) {
	// Two steps, one half the other, combined so their leading errors cancel.
	// Powers of two, so x plus the step is exact and a polynomial comes out exact.
	const double h = ldexp(1.0, ilogb(fmax(1.0, fabs(x))) - 10);
	double wide, narrow;
	if (!central(fn, user, x, h, &wide) || !central(fn, user, x, h / 2.0, &narrow)) {
		return false;
	}
	*d = (4.0 * narrow - wide) / 3.0;
	return true;
}

/* Gauss-Kronrod, 7 and 15 points: the difference between the two is the error */
static const double XGK[8] = {0.991455371120812639206854697526329, 0.949107912342758524526189684047851,
		0.864864423359769072789712788640926, 0.741531185599394439863864773280788, 0.586087235467691130294144845693013,
		0.405845151377397166906606412076961, 0.207784955007898467600689403773245, 0.0};
static const double WGK[8] = {0.022935322010529224963732008058970, 0.063092092629978553290700663189204,
		0.104790010322250183839876322541518, 0.140653259715525918745189590510238, 0.169004726639267902826583426598550,
		0.190350578064785409913256402421014, 0.204432940075298892414161999234649, 0.209482141084727828012999174891714};
static const double WG[4] = {0.129484966168869693270611432679082, 0.279705391489276667901467771423780,
		0.381830050505118944950369775488975, 0.417959183673469387755102040816327};

/* Never at the ends, so ln from 0 or 1/sqrt from 0 still integrate */
static bool kronrod(qdos_graph_fn fn, void* user, double a, double b, double* value, double* error) {
	const double c = 0.5 * (a + b), h = 0.5 * (b - a);
	double fc;
	if (!fn(user, c, &fc) || !isfinite(fc)) {
		return false;
	}
	double k = fc * WGK[7], g = fc * WG[3];
	for (int j = 0; j < 7; j++) {
		double f1, f2;
		if (!fn(user, c - h * XGK[j], &f1) || !fn(user, c + h * XGK[j], &f2) || !isfinite(f1) || !isfinite(f2)) {
			return false;
		}
		k += WGK[j] * (f1 + f2);
		if (j % 2 == 1) {
			g += WG[j / 2] * (f1 + f2);
		}
	}
	*value = k * h;
	*error = fabs((k - g) * h);
	return true;
}

/* Intervals kept at once; each is fifteen calls */
#define PIECES 128

bool qdos_num_integral(qdos_graph_fn fn, void* user, double a, double b, double* area) {
	if (a == b) {
		*area = 0.0;
		return true;
	}

	double lo[PIECES], hi[PIECES], val[PIECES], err[PIECES];
	int count = 1;
	lo[0] = a;
	hi[0] = b;
	if (!kronrod(fn, user, a, b, &val[0], &err[0])) {
		return false;
	}

	// Halve the worst piece until the error is small or the budget is spent
	for (;;) {
		double total = 0.0, error = 0.0;
		int worst = 0;
		for (int i = 0; i < count; i++) {
			total += val[i];
			error += err[i];
			worst = (err[i] > err[worst]) ? i : worst;
		}
		if (error <= fmax(1e-12, 1e-10 * fabs(total)) || count == PIECES) {
			*area = total;
			return true;
		}

		const double mid = 0.5 * (lo[worst] + hi[worst]);
		double v1, e1, v2, e2;
		if (!kronrod(fn, user, lo[worst], mid, &v1, &e1) || !kronrod(fn, user, mid, hi[worst], &v2, &e2)) {
			return false;
		}
		lo[count] = mid;
		hi[count] = hi[worst];
		val[count] = v2;
		err[count] = e2;
		hi[worst] = mid;
		val[worst] = v1;
		err[worst] = e1;
		count++;
	}
}
