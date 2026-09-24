/**
 * @file graph.c
 * @brief Plotting y = f(x) into a framebuffer
 */

#include "graph.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

qdos_graph_shape qdos_graph_body_shape(const char* body) {
	bool any = false, t = false, theta = false;
	for (const char* p = body; *p;) {
		while (*p && isspace((unsigned char)*p)) {
			p++;
		}
		const char* word = p;
		while (*p && !isspace((unsigned char)*p)) {
			p++;
		}
		if (p == word) {
			break;
		}
		any = true;
		const size_t len = (size_t)(p - word);
		if (len == 1 && *word == 'y') {
			return QDOS_GRAPH_SURFACE;
		}
		t = t || (len == 1 && *word == 't');
		theta = theta || (len == 5 && strncmp(word, "theta", 5) == 0);
	}
	if (t) {
		return QDOS_GRAPH_PARAM;
	}
	if (theta) {
		return QDOS_GRAPH_POLAR;
	}
	return any ? QDOS_GRAPH_CURVE : QDOS_GRAPH_NONE;
}

void qdos_graph_standard(qdos_graph_view* view) {
	view->x0 = -10.0;
	view->x1 = 10.0;
	view->y0 = -10.0;
	view->y1 = 10.0;
	view->xscl = 0.0;
	view->yscl = 0.0;
}

/*
 * To a millionth of a pixel, so a window laid out on round steps reads round:
 * -10.025 plus 200.5 steps of 0.05 is 1.8e-15 in doubles, not nought
 */
static double snap(double v, double step) {
	const double q = step * 1e-6;
	return (q > 0.0 && fabs(v / q) < 1e15) ? round(v / q) * q : v;
}

double qdos_graph_x(const qdos_graph_view* view, int col, int columns) {
	const double step = (view->x1 - view->x0) / (double)columns;
	return snap(view->x0 + ((double)col + 0.5) * step, step);
}

double qdos_graph_y(const qdos_graph_view* view, int row, int rows) {
	const double step = (view->y1 - view->y0) / (double)(rows - 1);
	return snap(view->y1 - (double)row * step, step);
}

int qdos_graph_col(const qdos_graph_view* view, double x, int columns) {
	const double c = (x - view->x0) / (view->x1 - view->x0) * (double)columns - 0.5;
	if (!(c > -1e6)) {
		return -1000000;
	}
	return (c < 1e6) ? (int)lround(c) : 1000000;
}

int qdos_graph_sample_points(qdos_graph_points* p, double t0, double t1, double step, qdos_graph_fn2 fn, void* user) {
	p->t0 = t0;
	p->step = step;
	p->count = 0;
	if (!(step > 0.0) || !(t1 >= t0)) {
		return 0;
	}

	// Counted, so a step too small to move t cannot loop, and capped
	const double steps = floor((t1 - t0) / step + 1e-9) + 1.0;
	p->count = (steps < QDOS_GRAPH_MAX_POINTS) ? (int)steps : QDOS_GRAPH_MAX_POINTS;

	int good = 0;
	for (int k = 0; k < p->count; k++) {
		double x = 0.0, y = 0.0;
		p->ok[k] = fn(user, t0 + k * step, &x, &y) && isfinite(x) && isfinite(y);
		p->x[k] = p->ok[k] ? x : 0.0;
		p->y[k] = p->ok[k] ? y : 0.0;
		good += p->ok[k] ? 1 : 0;
	}
	return good;
}

/* lo and hi of @p n values where @p ok, widened a little, or for a constant by one */
static bool fit_range(const double* v, const bool* ok, int n, double* out_lo, double* out_hi) {
	bool any = false;
	double lo = 0.0, hi = 0.0;
	for (int i = 0; i < n; i++) {
		if (!ok[i]) {
			continue;
		}
		lo = (!any || v[i] < lo) ? v[i] : lo;
		hi = (!any || v[i] > hi) ? v[i] : hi;
		any = true;
	}
	if (!any) {
		return false;
	}

	// A constant has no height to fit, so give it one around its value
	const double span = hi - lo;
	if (span <= 1e-12 * fmax(1.0, fabs(hi))) {
		*out_lo = lo - 1.0;
		*out_hi = hi + 1.0;
		return true;
	}

	// A margin, or the extremes sit on the edge and read as cut off
	*out_lo = lo - span * 0.05;
	*out_hi = hi + span * 0.05;
	return true;
}

bool qdos_graph_fit_points(const qdos_graph_points* p, qdos_graph_view* view) {
	return fit_range(p->y, p->ok, p->count, &view->y0, &view->y1);
}

int qdos_graph_sample(qdos_graph_samples* s, const qdos_graph_view* view, int columns, qdos_graph_fn fn, void* user) {
	if (columns > QDOS_GRAPH_MAX_COLUMNS) {
		columns = QDOS_GRAPH_MAX_COLUMNS;
	}
	s->columns = columns;

	int good = 0;
	for (int c = 0; c < columns; c++) {
		double y = 0.0;
		s->ok[c] = fn(user, qdos_graph_x(view, c, columns), &y) && isfinite(y);
		s->y[c] = s->ok[c] ? y : 0.0;
		good += s->ok[c] ? 1 : 0;
	}
	return good;
}

bool qdos_graph_fit(const qdos_graph_samples* s, qdos_graph_view* view) {
	return fit_range(s->y, s->ok, s->columns, &view->y0, &view->y1);
}

void qdos_graph_pan(qdos_graph_view* view, double dx, double dy) {
	const double w = view->x1 - view->x0, h = view->y1 - view->y0;
	view->x0 += dx * w;
	view->x1 += dx * w;
	view->y0 += dy * h;
	view->y1 += dy * h;
}

void qdos_graph_zoom(qdos_graph_view* view, double cx, double cy, double factor) {
	view->x0 = cx - (cx - view->x0) * factor;
	view->x1 = cx + (view->x1 - cx) * factor;
	view->y0 = cy - (cy - view->y0) * factor;
	view->y1 = cy + (view->y1 - cy) * factor;
}

/* Pixels a unit is apart, across and down */
#define DECIMAL_STEP 0.05
#define TRIG_STEP_DEG 1.875
#define PI 3.14159265358979323846

/* x0 and x1 so the middle column stands for @p centre, @p step apart */
static void columns_about(qdos_graph_view* view, double centre, double step, int columns) {
	view->x0 = centre - ((double)(columns / 2) + 0.5) * step;
	view->x1 = view->x0 + (double)columns * step;
}

/* y0 and y1 so a row stands for @p centre, @p step apart */
static void rows_about(qdos_graph_view* view, double centre, double step, int rows) {
	view->y1 = centre + (double)((rows - 1) / 2) * step;
	view->y0 = view->y1 - (double)(rows - 1) * step;
}

void qdos_graph_decimal(qdos_graph_view* view, int columns, int rows) {
	columns_about(view, 0.0, DECIMAL_STEP, columns);
	rows_about(view, 0.0, DECIMAL_STEP, rows);
	view->xscl = 1.0;
	view->yscl = 1.0;
}

void qdos_graph_integer(qdos_graph_view* view, int columns, int rows) {
	columns_about(view, round((view->x0 + view->x1) / 2.0), 1.0, columns);
	rows_about(view, round((view->y0 + view->y1) / 2.0), 1.0, rows);
	view->xscl = 10.0;
	view->yscl = 10.0;
}

void qdos_graph_square(qdos_graph_view* view, int columns, int rows) {
	const double sx = (view->x1 - view->x0) / (double)columns;
	const double sy = (view->y1 - view->y0) / (double)(rows - 1);
	const double s = fmax(sx, sy);
	const double cx = (view->x0 + view->x1) / 2.0, cy = (view->y0 + view->y1) / 2.0;
	view->x0 = cx - s * (double)columns / 2.0;
	view->x1 = cx + s * (double)columns / 2.0;
	view->y0 = cy - s * (double)(rows - 1) / 2.0;
	view->y1 = cy + s * (double)(rows - 1) / 2.0;
}

void qdos_graph_trig(qdos_graph_view* view, int columns, int rows, bool degrees) {
	(void)rows;
	columns_about(view, 0.0, degrees ? TRIG_STEP_DEG : TRIG_STEP_DEG * PI / 180.0, columns);
	view->y0 = -4.0;
	view->y1 = 4.0;
	view->xscl = degrees ? 90.0 : PI / 2.0;
	view->yscl = 1.0;
}

double qdos_graph_tick_step(double range, int ticks) {
	const double raw = range / (double)ticks;
	const double p = pow(10.0, floor(log10(raw)));
	if (5.0 * p <= raw) {
		return 5.0 * p;
	}
	if (2.0 * p <= raw) {
		return 2.0 * p;
	}
	return p;
}

static void dot(const qdos_graph_area* a, int x, int y) {
	if (x < a->left || x >= a->left + a->width || y < a->top || y >= a->top + a->height) {
		return;
	}
	a->fb[(size_t)y * a->stride + x] = a->ink;
}

static void flip(const qdos_graph_area* a, int x, int y) {
	if (x < a->left || x >= a->left + a->width || y < a->top || y >= a->top + a->height) {
		return;
	}
	uint8_t* p = &a->fb[(size_t)y * a->stride + x];
	*p = (uint8_t)(0xFF - *p);
}

/*
 * A line, one pixel per step in the longer direction, so a step between rows
 * is a single diagonal pixel. Filling each column from one sample's row to the
 * next inked both columns at every step and read as a staircase of blocks.
 */
/* Whether the @p n th pixel along a curve is inked, in @p style. Counted along
 * the curve rather than across the screen, so a steep stretch is dashed too. */
static bool inked(qdos_graph_style style, unsigned n) {
	switch (style) {
	case QDOS_GRAPH_DASHED:
		return (n / 4) % 2 == 0;
	case QDOS_GRAPH_DOTTED:
		return n % 3 == 0;
	default:
		return true;
	}
}

static void line(const qdos_graph_area* a, int x0, int y0, int x1, int y1, qdos_graph_style style, unsigned* n) {
	const int dx = abs(x1 - x0), sx = (x0 < x1) ? 1 : -1;
	const int dy = -abs(y1 - y0), sy = (y0 < y1) ? 1 : -1;
	int err = dx + dy;
	for (;;) {
		// The shared end of two segments is counted once
		if (!(x0 == x1 && y0 == y1)) {
			if (inked(style, (*n)++)) {
				dot(a, x0, y0);
			}
		} else if (inked(style, *n)) {
			dot(a, x0, y0);
		}
		if (x0 == x1 && y0 == y1) {
			return;
		}
		const int e2 = 2 * err;
		if (e2 >= dy) {
			err += dy;
			x0 += sx;
		}
		if (e2 <= dx) {
			err += dx;
			y0 += sy;
		}
	}
}

/* Screen position of a point on the plane, unclamped */
static double col_of(const qdos_graph_area* a, const qdos_graph_view* v, double x) {
	return a->left + (x - v->x0) / (v->x1 - v->x0) * a->width - 0.5;
}

static double row_of(const qdos_graph_area* a, const qdos_graph_view* v, double y) {
	return a->top + (v->y1 - y) / (v->y1 - v->y0) * (a->height - 1);
}

/* Ticks this far either side of an axis */
#define TICK 2

/* Counted rather than stepped: past a deep enough zoom, adding the step to a
 * double stops changing it, and a loop on the value never ends */
#define MAX_TICKS 256

/* The spacing asked for, unless it would run the ticks together */
static double axis_step(double range, int ticks, double scl) {
	if (scl > 0.0) {
		return (range / scl <= MAX_TICKS - 2) ? scl : 0.0;
	}
	return qdos_graph_tick_step(range, ticks);
}

void qdos_graph_draw_axes(const qdos_graph_area* a, const qdos_graph_view* v) {
	const double ax = col_of(a, v, 0.0);
	const double ay = row_of(a, v, 0.0);
	const bool x_axis = ay >= a->top && ay < a->top + a->height;
	const bool y_axis = ax >= a->left && ax < a->left + a->width;

	if (x_axis) {
		const int row = (int)lround(ay);
		for (int x = a->left; x < a->left + a->width; x++) {
			dot(a, x, row);
		}
		const double step = axis_step(v->x1 - v->x0, 8, v->xscl);
		const double first = (step > 0.0) ? ceil(v->x0 / step) : 0.0;
		for (int k = 0; step > 0.0 && k < MAX_TICKS && (first + k) * step <= v->x1; k++) {
			const int col = (int)lround(col_of(a, v, (first + k) * step));
			for (int d = -TICK; d <= TICK; d++) {
				dot(a, col, row + d);
			}
		}
	}

	if (y_axis) {
		const int col = (int)lround(ax);
		for (int y = a->top; y < a->top + a->height; y++) {
			dot(a, col, y);
		}
		const double step = axis_step(v->y1 - v->y0, 6, v->yscl);
		const double first = (step > 0.0) ? ceil(v->y0 / step) : 0.0;
		for (int k = 0; step > 0.0 && k < MAX_TICKS && (first + k) * step <= v->y1; k++) {
			const int row = (int)lround(row_of(a, v, (first + k) * step));
			for (int d = -TICK; d <= TICK; d++) {
				dot(a, col + d, row);
			}
		}
	}
}

void qdos_graph_draw_grid(const qdos_graph_area* a, const qdos_graph_view* v) {
	const double xs = axis_step(v->x1 - v->x0, 8, v->xscl);
	const double ys = axis_step(v->y1 - v->y0, 6, v->yscl);
	if (xs <= 0.0 || ys <= 0.0) {
		return;
	}
	const double fx = ceil(v->x0 / xs), fy = ceil(v->y0 / ys);
	for (int i = 0; i < MAX_TICKS && (fx + i) * xs <= v->x1; i++) {
		const int col = (int)lround(col_of(a, v, (fx + i) * xs));
		for (int j = 0; j < MAX_TICKS && (fy + j) * ys <= v->y1; j++) {
			dot(a, col, (int)lround(row_of(a, v, (fy + j) * ys)));
		}
	}
}

/*
 * Liang-Barsky: cut the segment to the plot, a pixel beyond each edge so a
 * line leaving it still reaches the edge. False when none of it is inside.
 */
static bool clip(const qdos_graph_area* a, double* x0, double* y0, double* x1, double* y1) {
	if (!isfinite(*x0) || !isfinite(*y0) || !isfinite(*x1) || !isfinite(*y1)) {
		return false;
	}
	const double lo_x = a->left - 1.0, hi_x = a->left + a->width;
	const double lo_y = a->top - 1.0, hi_y = a->top + a->height;
	const double dx = *x1 - *x0, dy = *y1 - *y0;
	const double p[4] = {-dx, dx, -dy, dy};
	const double q[4] = {*x0 - lo_x, hi_x - *x0, *y0 - lo_y, hi_y - *y0};
	double t0 = 0.0, t1 = 1.0;
	for (int i = 0; i < 4; i++) {
		if (p[i] == 0.0) {
			if (q[i] < 0.0) {
				return false;
			}
			continue;
		}
		const double t = q[i] / p[i];
		if (p[i] < 0.0) {
			t0 = fmax(t0, t);
		} else {
			t1 = fmin(t1, t);
		}
	}
	if (t0 > t1) {
		return false;
	}
	const double ox = *x0, oy = *y0;
	*x0 = ox + t0 * dx;
	*y0 = oy + t0 * dy;
	*x1 = ox + t1 * dx;
	*y1 = oy + t1 * dy;
	return true;
}

static void segment(const qdos_graph_area* a, const qdos_graph_view* v, double x0, double y0, double x1, double y1,
		qdos_graph_style style, unsigned* n) {
	double c0 = col_of(a, v, x0), r0 = row_of(a, v, y0);
	double c1 = col_of(a, v, x1), r1 = row_of(a, v, y1);
	if (!clip(a, &c0, &r0, &c1, &r1)) {
		return;
	}
	line(a, (int)lround(c0), (int)lround(r0), (int)lround(c1), (int)lround(r1), style, n);
}

void qdos_graph_draw_segment(const qdos_graph_area* area, const qdos_graph_view* view, double x0, double y0, double x1,
		double y1, qdos_graph_style style) {
	unsigned n = 0;
	segment(area, view, x0, y0, x1, y1, style, &n);
}

void qdos_graph_draw_points(
		const qdos_graph_area* area, const qdos_graph_view* view, const qdos_graph_points* p, qdos_graph_style style) {
	unsigned n = 0;
	for (int k = 0; k < p->count; k++) {
		if (!p->ok[k]) {
			continue;
		}
		if (k + 1 < p->count && p->ok[k + 1]) {
			segment(area, view, p->x[k], p->y[k], p->x[k + 1], p->y[k + 1], style, &n);
		} else if (k == 0 || !p->ok[k - 1]) {
			// Alone, with nothing either side to join to
			const double c = col_of(area, view, p->x[k]), r = row_of(area, view, p->y[k]);
			if (isfinite(c) && isfinite(r) && fabs(c) < 1e6 && fabs(r) < 1e6) {
				dot(area, (int)lround(c), (int)lround(r));
			}
		}
	}
}

/* A row clamped to the plot, for filling towards one that may be far off it */
static int clamp_row(const qdos_graph_area* a, double r) {
	const double lo = a->top, hi = a->top + a->height - 1;
	return (int)lround(fmax(lo, fmin(hi, r)));
}

void qdos_graph_shade(
		const qdos_graph_area* area, const qdos_graph_view* view, const qdos_graph_samples* s, double a, double b) {
	const double lo = fmin(a, b), hi = fmax(a, b);
	const int axis = clamp_row(area, row_of(area, view, 0.0));
	for (int c = 0; c < s->columns; c++) {
		const double x = qdos_graph_x(view, c, s->columns);
		if (!s->ok[c] || x < lo || x > hi) {
			continue;
		}
		const int px = area->left + c * area->width / s->columns;
		const int r = clamp_row(area, row_of(area, view, s->y[c]));
		for (int y = (r < axis ? r : axis); y <= (r < axis ? axis : r); y++) {
			if ((px + y) % 2 == 0) {
				dot(area, px, y);
			}
		}
	}
}

/* Half the side of a scatter plot's square */
#define MARK 2

void qdos_graph_draw_mark(const qdos_graph_area* area, const qdos_graph_view* view, double x, double y) {
	const double c = col_of(area, view, x), r = row_of(area, view, y);
	if (!(fabs(c) < 1e6) || !(fabs(r) < 1e6)) {
		return;
	}
	const int cx = (int)lround(c), cy = (int)lround(r);
	for (int d = -MARK; d <= MARK; d++) {
		dot(area, cx + d, cy - MARK);
		dot(area, cx + d, cy + MARK);
		dot(area, cx - MARK, cy + d);
		dot(area, cx + MARK, cy + d);
	}
}

void qdos_graph_draw_rect(
		const qdos_graph_area* area, const qdos_graph_view* view, double x0, double y0, double x1, double y1) {
	qdos_graph_draw_segment(area, view, x0, y0, x1, y0, QDOS_GRAPH_SOLID);
	qdos_graph_draw_segment(area, view, x1, y0, x1, y1, QDOS_GRAPH_SOLID);
	qdos_graph_draw_segment(area, view, x1, y1, x0, y1, QDOS_GRAPH_SOLID);
	qdos_graph_draw_segment(area, view, x0, y1, x0, y0, QDOS_GRAPH_SOLID);
}

/* A run of pixels along a row, between two columns that may be off the plot */
static void hline(const qdos_graph_area* a, double c0, double c1, int y) {
	const double lo = fmax(fmin(c0, c1), a->left - 1.0), hi = fmin(fmax(c0, c1), (double)(a->left + a->width));
	for (int x = (int)lround(lo); x <= (int)lround(hi); x++) {
		dot(a, x, y);
	}
}

static void vline(const qdos_graph_area* a, double c, int y0, int y1) {
	if (!(fabs(c) < 1e6)) {
		return;
	}
	for (int y = y0; y <= y1; y++) {
		dot(a, (int)lround(c), y);
	}
}

/* Half the height of the box, and of the ticks closing the whiskers */
#define BOX_HALF 8
#define WHISKER_END 4

void qdos_graph_draw_boxplot(const qdos_graph_area* area, const qdos_graph_view* view, double min, double q1,
		double median, double q3, double max) {
	const int mid = area->top + area->height / 4;
	const double c_min = col_of(area, view, min), c_q1 = col_of(area, view, q1);
	const double c_med = col_of(area, view, median), c_q3 = col_of(area, view, q3);
	const double c_max = col_of(area, view, max);
	hline(area, c_min, c_q1, mid);
	hline(area, c_q3, c_max, mid);
	vline(area, c_min, mid - WHISKER_END, mid + WHISKER_END);
	vline(area, c_max, mid - WHISKER_END, mid + WHISKER_END);
	hline(area, c_q1, c_q3, mid - BOX_HALF);
	hline(area, c_q1, c_q3, mid + BOX_HALF);
	vline(area, c_q1, mid - BOX_HALF, mid + BOX_HALF);
	vline(area, c_q3, mid - BOX_HALF, mid + BOX_HALF);
	vline(area, c_med, mid - BOX_HALF, mid + BOX_HALF);
}

void qdos_graph_draw(const qdos_graph_area* area, const qdos_graph_view* view, const qdos_graph_samples* s) {
	qdos_graph_draw_axes(area, view);
	qdos_graph_draw_curve(area, view, s, QDOS_GRAPH_SOLID);
}

void qdos_graph_draw_curve(
		const qdos_graph_area* area, const qdos_graph_view* view, const qdos_graph_samples* s, qdos_graph_style style) {
	unsigned n = 0;
	const int top = area->top, bottom = area->top + area->height - 1;
	for (int c = 0; c < s->columns; c++) {
		if (!s->ok[c]) {
			continue;
		}

		const int x = area->left + c * area->width / s->columns;
		const double r0 = row_of(area, view, s->y[c]);
		const bool joined = c + 1 < s->columns && s->ok[c + 1];
		if (!joined) {
			if (r0 >= top && r0 <= bottom && inked(style, n++)) {
				dot(area, x, (int)lround(r0));
			}
			continue;
		}

		// Off the top on one side and off the bottom on the other is a pole,
		// like tan's, not a steep stretch of curve: joining it draws a wall
		const double r1 = row_of(area, view, s->y[c + 1]);
		if ((r0 < top && r1 > bottom) || (r1 < top && r0 > bottom)) {
			continue;
		}

		// Cut to the plot where it leaves it, along the line rather than by
		// clamping the far end, which would bend a steep one as it went out
		const int x1 = area->left + (c + 1) * area->width / s->columns;
		double t0 = 0.0, t1 = 1.0;
		if (r0 != r1) {
			const double ta = (top - 1.0 - r0) / (r1 - r0), tb = (bottom + 1.0 - r0) / (r1 - r0);
			t0 = fmax(t0, fmin(ta, tb));
			t1 = fmin(t1, fmax(ta, tb));
		} else if (r0 < top - 1.0 || r0 > bottom + 1.0) {
			continue;
		}
		if (t0 > t1) {
			continue;
		}
		line(area, (int)lround(x + t0 * (x1 - x)), (int)lround(r0 + t0 * (r1 - r0)), (int)lround(x + t1 * (x1 - x)),
				(int)lround(r0 + t1 * (r1 - r0)), style, &n);
	}
}

/* Arms of the trace cross */
#define CURSOR_ARM 4

void qdos_graph_draw_cursor_at(const qdos_graph_area* area, const qdos_graph_view* view, double x, double y) {
	const double c = col_of(area, view, x), row = row_of(area, view, y);
	if (!(c >= area->left && c < area->left + area->width) ||
			!(row >= area->top && row <= area->top + area->height - 1)) {
		return;
	}
	const int cx = (int)lround(c), cy = (int)lround(row);
	for (int d = 1; d <= CURSOR_ARM; d++) {
		flip(area, cx - d, cy);
		flip(area, cx + d, cy);
		flip(area, cx, cy - d);
		flip(area, cx, cy + d);
	}
}

void qdos_graph_draw_cursor(
		const qdos_graph_area* area, const qdos_graph_view* view, const qdos_graph_samples* s, int col) {
	if (col < 0 || col >= s->columns || !s->ok[col]) {
		return;
	}

	// Inverted rather than inked, so it shows on the curve it sits on
	// Off the plot there is nowhere to put it, and a far enough row would
	// overflow the rounding
	const double row = row_of(area, view, s->y[col]);
	if (row < area->top || row > area->top + area->height - 1) {
		return;
	}
	const int x = area->left + col * area->width / s->columns;
	const int y = (int)lround(row);
	for (int d = 1; d <= CURSOR_ARM; d++) {
		flip(area, x - d, y);
		flip(area, x + d, y);
		flip(area, x, y - d);
		flip(area, x, y + d);
	}
}
