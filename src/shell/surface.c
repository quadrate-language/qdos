/**
 * @file surface.c
 * @brief Plotting z = f(x, y) as a wireframe, hidden lines removed
 *
 * Hidden lines go by the painter's method: every cell of the grid is sorted by
 * depth and drawn from the back, filled with paper and outlined in ink, so a
 * nearer cell covers what is behind it. Nothing needs a depth per pixel, which
 * a one-bit panel has no use for, and it holds from every direction -- the
 * floating horizon it replaced assumed each row of the grid sat at one depth,
 * and came apart looking along a diagonal.
 */

#include "surface.h"

#include <math.h>
#include <stdlib.h>

#define PI 3.14159265358979323846

/* The box's height against its sides, which run from -1 to 1. Low, because
 * the panel is wide: a tall box fits its height and wastes half the width. */
#define Z_HALF 0.4

/* Pixels kept clear round the box at zoom 1 */
#define MARGIN 4

/* How much wider than true the box may be drawn to fill the plot */
#define STRETCH_MAX 1.5

void qdos_surface_standard(qdos_surface_view* view) {
	view->azimuth = 30.0;
	view->elevation = 25.0;
	view->zoom = 1.0;
}

int qdos_surface_sample(
		qdos_surface* s, int n, double x0, double x1, double y0, double y1, qdos_surface_fn fn, void* user) {
	if (n < 2) {
		n = 2;
	}
	if (n > QDOS_SURFACE_MAX) {
		n = QDOS_SURFACE_MAX;
	}
	s->n = n;
	s->x0 = x0;
	s->x1 = x1;
	s->y0 = y0;
	s->y1 = y1;

	int good = 0;
	for (int i = 0; i < n; i++) {
		const double x = x0 + (x1 - x0) * i / (n - 1);
		for (int j = 0; j < n; j++) {
			const double y = y0 + (y1 - y0) * j / (n - 1);
			double z = 0.0;
			s->ok[i][j] = fn(user, x, y, &z) && isfinite(z);
			s->z[i][j] = s->ok[i][j] ? z : 0.0;
			if (!s->ok[i][j]) {
				continue;
			}
			s->z0 = (good == 0 || z < s->z0) ? z : s->z0;
			s->z1 = (good == 0 || z > s->z1) ? z : s->z1;
			good++;
		}
	}
	if (good == 0) {
		s->z0 = s->z1 = 0.0;
	}
	return good;
}

/* Everything the projection needs, worked out once per drawing */
typedef struct {
	double ca, sa, ce, se;
	double scale_x, scale_y, cx, cy;
	double zmid, zscale;
	int n;
} projection;

/* The grid point's place in the box: u and v from -1 to 1, w within Z_HALF */
static void box_point(const qdos_surface* s, const projection* p, int i, int j, double* u, double* v, double* w) {
	*u = -1.0 + 2.0 * i / (p->n - 1);
	*v = -1.0 + 2.0 * j / (p->n - 1);
	*w = (s->z[i][j] - p->zmid) * p->zscale;
}

/* Turned about z, tilted towards the viewer, and flattened: up the screen is up */
static void flatten(const projection* p, double u, double v, double w, double* sx, double* sy) {
	const double xr = u * p->ca - v * p->sa;
	const double yr = u * p->sa + v * p->ca;
	*sx = xr;
	*sy = w * p->ce + yr * p->se;
}

static projection make_projection(const qdos_graph_area* area, const qdos_surface* s, const qdos_surface_view* view) {
	projection p;
	const double a = view->azimuth * PI / 180.0, e = view->elevation * PI / 180.0;
	p.ca = cos(a);
	p.sa = sin(a);
	p.ce = cos(e);
	p.se = sin(e);
	p.n = s->n;

	// A flat surface has no height to scale, and sits in the middle of the box
	p.zmid = (s->z0 + s->z1) / 2.0;
	p.zscale = (s->z1 > s->z0) ? Z_HALF / ((s->z1 - s->z0) / 2.0) : 0.0;

	// Fitted to the box's corners rather than to the surface, so turning it
	// does not make it breathe in and out as its extremes come round
	double lo_x = 0, hi_x = 0, lo_y = 0, hi_y = 0;
	for (int k = 0; k < 8; k++) {
		double sx, sy;
		flatten(&p, (k & 1) ? 1.0 : -1.0, (k & 2) ? 1.0 : -1.0, (k & 4) ? Z_HALF : -Z_HALF, &sx, &sy);
		lo_x = (k == 0 || sx < lo_x) ? sx : lo_x;
		hi_x = (k == 0 || sx > hi_x) ? sx : hi_x;
		lo_y = (k == 0 || sy < lo_y) ? sy : lo_y;
		hi_y = (k == 0 || sy > hi_y) ? sy : hi_y;
	}
	const double fit_x = (area->width - 2 * MARGIN) / (hi_x - lo_x);
	const double fit_y = (area->height - 2 * MARGIN) / (hi_y - lo_y);
	// Wider than it is true to, up to a point: the plot is a letterbox, and a
	// box fitted to its height alone leaves a third of the width empty
	p.scale_y = view->zoom * fmin(fit_x, fit_y);
	p.scale_x = view->zoom * fmin(fit_x, fit_y * STRETCH_MAX);
	p.cx = area->left + area->width / 2.0 - p.scale_x * (lo_x + hi_x) / 2.0;
	p.cy = area->top + area->height / 2.0 + p.scale_y * (lo_y + hi_y) / 2.0;
	return p;
}

static void screen_point(const qdos_surface* s, const projection* p, int i, int j, int* x, int* y) {
	double u, v, w, sx, sy;
	box_point(s, p, i, j, &u, &v, &w);
	flatten(p, u, v, w, &sx, &sy);
	*x = (int)lround(p->cx + sx * p->scale_x);
	*y = (int)lround(p->cy - sy * p->scale_y);
}

static void dot(const qdos_graph_area* a, int x, int y, uint8_t level) {
	if (x < a->left || x >= a->left + a->width || y < a->top || y >= a->top + a->height) {
		return;
	}
	a->fb[(size_t)y * a->stride + x] = level;
}

static void line(const qdos_graph_area* a, int x0, int y0, int x1, int y1) {
	const int dx = abs(x1 - x0), sx = (x0 < x1) ? 1 : -1;
	const int dy = -abs(y1 - y0), sy = (y0 < y1) ? 1 : -1;
	int err = dx + dy;
	for (;;) {
		dot(a, x0, y0, a->ink);
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

/*
 * A cell's inside, a scanline at a time, even-odd: seen nearly edge on a cell
 * can project to a bow tie, and even-odd fills that as two triangles rather
 * than guessing.
 */
static void fill(const qdos_graph_area* a, const int* x, const int* y) {
	int lo = y[0], hi = y[0];
	for (int k = 1; k < 4; k++) {
		lo = (y[k] < lo) ? y[k] : lo;
		hi = (y[k] > hi) ? y[k] : hi;
	}
	lo = (lo < a->top) ? a->top : lo;
	hi = (hi > a->top + a->height - 1) ? a->top + a->height - 1 : hi;

	for (int row = lo; row <= hi; row++) {
		const double yc = row + 0.5;
		double cross[4];
		int count = 0;
		for (int k = 0; k < 4; k++) {
			const int m = (k + 1) % 4;
			if ((y[k] <= yc) == (y[m] <= yc)) {
				continue;
			}
			cross[count++] = x[k] + (yc - y[k]) * (double)(x[m] - x[k]) / (double)(y[m] - y[k]);
		}
		for (int p = 1; p < count; p++) {
			for (int q = p; q > 0 && cross[q - 1] > cross[q]; q--) {
				const double t = cross[q];
				cross[q] = cross[q - 1];
				cross[q - 1] = t;
			}
		}
		for (int p = 0; p + 1 < count; p += 2) {
			for (int col = (int)ceil(cross[p] - 0.5); col <= (int)floor(cross[p + 1] - 0.5); col++) {
				dot(a, col, row, a->paper);
			}
		}
	}
}

typedef struct {
	double depth;
	int i, j; ///< The corner nearest the origin of the grid
} cell;

static int deepest_first(const void* pa, const void* pb) {
	const double a = ((const cell*)pa)->depth, b = ((const cell*)pb)->depth;
	return (a < b) - (a > b);
}

void qdos_surface_draw(const qdos_graph_area* area, const qdos_surface* s, const qdos_surface_view* view) {
	const projection p = make_projection(area, s, view);
	const int n = s->n;

	int sx[QDOS_SURFACE_MAX][QDOS_SURFACE_MAX], sy[QDOS_SURFACE_MAX][QDOS_SURFACE_MAX];
	double depth[QDOS_SURFACE_MAX][QDOS_SURFACE_MAX];
	for (int i = 0; i < n; i++) {
		for (int j = 0; j < n; j++) {
			if (!s->ok[i][j]) {
				continue;
			}
			screen_point(s, &p, i, j, &sx[i][j], &sy[i][j]);

			// Along the line of sight: further round in the turn, and lower
			// when looked at from above
			double u, v, w;
			box_point(s, &p, i, j, &u, &v, &w);
			depth[i][j] = (u * p.sa + v * p.ca) * p.ce - w * p.se;
		}
	}

	// Only whole cells: one with a corner missing is a hole in the surface
	static cell cells[(QDOS_SURFACE_MAX - 1) * (QDOS_SURFACE_MAX - 1)];
	int count = 0;
	for (int i = 0; i + 1 < n; i++) {
		for (int j = 0; j + 1 < n; j++) {
			if (!s->ok[i][j] || !s->ok[i + 1][j] || !s->ok[i + 1][j + 1] || !s->ok[i][j + 1]) {
				continue;
			}
			cells[count].depth = (depth[i][j] + depth[i + 1][j] + depth[i + 1][j + 1] + depth[i][j + 1]) / 4.0;
			cells[count].i = i;
			cells[count].j = j;
			count++;
		}
	}
	qsort(cells, (size_t)count, sizeof(*cells), deepest_first);

	for (int k = 0; k < count; k++) {
		const int i = cells[k].i, j = cells[k].j;
		const int x[4] = {sx[i][j], sx[i + 1][j], sx[i + 1][j + 1], sx[i][j + 1]};
		const int y[4] = {sy[i][j], sy[i + 1][j], sy[i + 1][j + 1], sy[i][j + 1]};
		fill(area, x, y);
		for (int e = 0; e < 4; e++) {
			line(area, x[e], y[e], x[(e + 1) % 4], y[(e + 1) % 4]);
		}
	}
}
