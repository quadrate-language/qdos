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
	bool any = false;
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
		if (p - word == 1 && *word == 'y') {
			return QDOS_GRAPH_SURFACE;
		}
	}
	return any ? QDOS_GRAPH_CURVE : QDOS_GRAPH_NONE;
}

void qdos_graph_standard(qdos_graph_view* view) {
	view->x0 = -10.0;
	view->x1 = 10.0;
	view->y0 = -10.0;
	view->y1 = 10.0;
}

double qdos_graph_x(const qdos_graph_view* view, int col, int columns) {
	return view->x0 + ((double)col + 0.5) * (view->x1 - view->x0) / (double)columns;
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
	bool any = false;
	double lo = 0.0, hi = 0.0;
	for (int c = 0; c < s->columns; c++) {
		if (!s->ok[c]) {
			continue;
		}
		lo = (!any || s->y[c] < lo) ? s->y[c] : lo;
		hi = (!any || s->y[c] > hi) ? s->y[c] : hi;
		any = true;
	}
	if (!any) {
		return false;
	}

	// A constant has no height to fit, so give it one around its value
	const double span = hi - lo;
	if (span <= 1e-12 * fmax(1.0, fabs(hi))) {
		view->y0 = lo - 1.0;
		view->y1 = hi + 1.0;
		return true;
	}

	// A margin, or the extremes sit on the edge and read as cut off
	view->y0 = lo - span * 0.05;
	view->y1 = hi + span * 0.05;
	return true;
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
#define MAX_TICKS 64

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
		const double step = qdos_graph_tick_step(v->x1 - v->x0, 8);
		const double first = ceil(v->x0 / step);
		for (int k = 0; k < MAX_TICKS && (first + k) * step <= v->x1; k++) {
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
		const double step = qdos_graph_tick_step(v->y1 - v->y0, 6);
		const double first = ceil(v->y0 / step);
		for (int k = 0; k < MAX_TICKS && (first + k) * step <= v->y1; k++) {
			const int row = (int)lround(row_of(a, v, (first + k) * step));
			for (int d = -TICK; d <= TICK; d++) {
				dot(a, col + d, row);
			}
		}
	}
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
