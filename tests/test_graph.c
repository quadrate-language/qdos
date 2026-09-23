/**
 * @file test_graph.c
 * @brief Sampling, scaling and drawing y = f(x), without the interpreter
 */

#include "check.h"

#include "../src/shell/graph.h"
#include "../src/shell/surface.h"

#include <math.h>
#include <string.h>

#define W 400
#define H 240
#define TOP 24
#define PLOT_H 167

static uint8_t g_fb[W * H];

static const qdos_graph_area AREA = {g_fb, W, 0, TOP, W, PLOT_H, 0x00, 0xFF};

static void clear(void) {
	memset(g_fb, 0xFF, sizeof(g_fb));
}

static bool ink_at(int x, int y) {
	return g_fb[(size_t)y * W + x] < 0x80;
}

static bool f_identity(void* user, double x, double* y) {
	(void)user;
	*y = x;
	return true;
}

static bool f_tan(void* user, double x, double* y) {
	(void)user;
	*y = tan(x);
	return true;
}

/* Undefined left of zero, the way sqrt is */
static bool f_sqrt(void* user, double x, double* y) {
	(void)user;
	if (x < 0.0) {
		return false;
	}
	*y = sqrt(x);
	return true;
}

static bool f_nan(void* user, double x, double* y) {
	(void)user;
	(void)x;
	*y = NAN;
	return true;
}

static bool f_huge(void* user, double x, double* y) {
	(void)user;
	*y = (x < 0.0) ? -1e300 : 1e300;
	return true;
}

static void test_sampling_takes_one_value_per_column(void) {
	qdos_graph_view v;
	qdos_graph_standard(&v);
	static qdos_graph_samples s;

	CHECK(qdos_graph_sample(&s, &v, W, f_identity, NULL) == W);
	CHECK(s.columns == W);

	// Each at the middle of its column, so the two ends are half a column in
	CHECK(fabs(s.y[0] - (-10.0 + 10.0 / W)) < 1e-12);
	CHECK(fabs(s.y[W - 1] - (10.0 - 10.0 / W)) < 1e-12);
	CHECK(fabs(qdos_graph_x(&v, 200, W) - (10.0 / W)) < 1e-12);
}

static void test_a_function_with_no_value_leaves_a_gap(void) {
	qdos_graph_view v;
	qdos_graph_standard(&v);
	static qdos_graph_samples s;

	CHECK(qdos_graph_sample(&s, &v, W, f_sqrt, NULL) == W / 2);
	CHECK(!s.ok[0] && !s.ok[W / 2 - 1]);
	CHECK(s.ok[W / 2] && s.ok[W - 1]);

	// Not finite is not a value either
	CHECK(qdos_graph_sample(&s, &v, W, f_nan, NULL) == 0);
}

static void test_fit_shows_every_value_with_a_margin(void) {
	qdos_graph_view v;
	qdos_graph_standard(&v);
	static qdos_graph_samples s;

	qdos_graph_sample(&s, &v, W, f_sqrt, NULL);
	CHECK(qdos_graph_fit(&s, &v));
	const double lowest = s.y[W / 2]; // the first column right of zero
	CHECK(v.y0 < lowest && v.y0 > lowest - 0.2);
	CHECK(v.y1 > sqrt(10.0) && v.y1 < sqrt(10.0) + 0.2);
	CHECK(v.x0 == -10.0 && v.x1 == 10.0); // x is left alone

	// Nothing to fit leaves the window as it was
	qdos_graph_view before = v;
	qdos_graph_sample(&s, &v, W, f_nan, NULL);
	CHECK(!qdos_graph_fit(&s, &v));
	CHECK(memcmp(&before, &v, sizeof(v)) == 0);
}

static bool f_constant(void* user, double x, double* y) {
	(void)user;
	(void)x;
	*y = 3.0;
	return true;
}

static void test_fit_gives_a_constant_some_height(void) {
	qdos_graph_view v;
	qdos_graph_standard(&v);
	static qdos_graph_samples s;

	qdos_graph_sample(&s, &v, W, f_constant, NULL);
	CHECK(qdos_graph_fit(&s, &v));
	CHECK(v.y0 == 2.0 && v.y1 == 4.0);
}

static void test_ticks_are_one_two_or_five(void) {
	CHECK(qdos_graph_tick_step(20.0, 8) == 2.0);
	CHECK(qdos_graph_tick_step(1.0, 8) == 0.1);
	CHECK(qdos_graph_tick_step(100.0, 8) == 10.0);
	CHECK(qdos_graph_tick_step(50.0, 8) == 5.0);
	CHECK(fabs(qdos_graph_tick_step(0.004, 8) - 0.0005) < 1e-15);
}

static void test_pan_and_zoom(void) {
	qdos_graph_view v;
	qdos_graph_standard(&v);

	qdos_graph_pan(&v, 0.25, -0.5);
	CHECK(v.x0 == -5.0 && v.x1 == 15.0);
	CHECK(v.y0 == -20.0 && v.y1 == 0.0);

	qdos_graph_standard(&v);
	qdos_graph_zoom(&v, 2.0, 0.0, 0.5);
	CHECK(v.x0 == -4.0 && v.x1 == 6.0); // about x = 2, which stays put
	CHECK(v.y0 == -5.0 && v.y1 == 5.0);
}

/** y = x in the standard window is the diagonal, with both axes through the middle. */
static void test_the_identity_draws_the_diagonal(void) {
	qdos_graph_view v;
	qdos_graph_standard(&v);
	static qdos_graph_samples s;
	qdos_graph_sample(&s, &v, W, f_identity, NULL);

	clear();
	qdos_graph_draw(&AREA, &v, &s);

	// Every column has ink on the line from top left to bottom right, give or
	// take the pixel the rounding leaves
	int missed = 0;
	for (int c = 0; c < W; c++) {
		const int row = TOP + (int)lround((PLOT_H - 1) * (1.0 - (c + 0.5) / W));
		bool near = false;
		for (int d = -1; d <= 1; d++) {
			const int y = row + d;
			near = near || (y >= TOP && y < TOP + PLOT_H && ink_at(c, y));
		}
		missed += near ? 0 : 1;
	}
	CHECK(missed == 0);

	// The x axis is a full row across the middle, the y axis a full column
	int axis_row = 0, axis_col = 0;
	for (int x = 0; x < W; x++) {
		axis_row += ink_at(x, TOP + PLOT_H / 2) ? 1 : 0;
	}
	for (int y = TOP; y < TOP + PLOT_H; y++) {
		axis_col += ink_at(W / 2 - 1, y) || ink_at(W / 2, y) ? 1 : 0;
	}
	CHECK(axis_row == W);
	CHECK(axis_col == PLOT_H);
}

static bool f_shallow(void* user, double x, double* y) {
	(void)user;
	*y = 0.3 * x + 5.0;
	return true;
}

/**
 * A shallow line is one pixel per column, stepping diagonally between rows.
 * Two in a column is a step drawn as a block, which reads as a staircase.
 */
static void test_a_shallow_line_is_one_pixel_per_column(void) {
	qdos_graph_view v;
	qdos_graph_standard(&v);
	static qdos_graph_samples s;
	qdos_graph_sample(&s, &v, W, f_shallow, NULL);

	clear();
	qdos_graph_draw(&AREA, &v, &s);

	// Above the x axis and its ticks, where only the curve is drawn
	const int axis = TOP + (PLOT_H - 1) / 2;
	int thick = 0, empty = 0;
	for (int x = 0; x < W; x++) {
		if (x >= W / 2 - 4 && x <= W / 2 + 3) {
			continue; // the y axis and its ticks
		}
		int lit = 0;
		for (int y = TOP; y < axis - 3; y++) {
			lit += ink_at(x, y) ? 1 : 0;
		}
		thick += (lit > 1) ? 1 : 0;
		empty += (lit == 0) ? 1 : 0;
	}
	CHECK(thick == 0);
	CHECK(empty == 0);
}

/** Nothing outside the plot area, however far off the curve goes. */
static void test_drawing_stays_in_the_area(void) {
	qdos_graph_view v;
	qdos_graph_standard(&v);
	static qdos_graph_samples s;

	qdos_graph_fn fns[3] = {f_identity, f_tan, f_huge};
	for (int i = 0; i < 3; i++) {
		qdos_graph_sample(&s, &v, W, fns[i], NULL);
		clear();
		qdos_graph_draw(&AREA, &v, &s);
		qdos_graph_draw_cursor(&AREA, &v, &s, 0);
		qdos_graph_draw_cursor(&AREA, &v, &s, W - 1);

		int outside = 0;
		for (int y = 0; y < H; y++) {
			if (y >= TOP && y < TOP + PLOT_H) {
				continue;
			}
			for (int x = 0; x < W; x++) {
				outside += ink_at(x, y) ? 1 : 0;
			}
		}
		CHECK(outside == 0);
	}
}

/** tan's poles are gaps, not walls: no column is inked top to bottom. */
static void test_a_pole_is_not_joined(void) {
	qdos_graph_view v;
	qdos_graph_standard(&v);
	v.y0 = -3.0;
	v.y1 = 3.0;
	static qdos_graph_samples s;
	qdos_graph_sample(&s, &v, W, f_tan, NULL);

	clear();
	qdos_graph_draw(&AREA, &v, &s);

	int walls = 0;
	for (int x = 0; x < W; x++) {
		// The y axis is the one full column there should be
		if (x == W / 2 || x == W / 2 - 1) {
			continue;
		}
		int lit = 0;
		for (int y = TOP; y < TOP + PLOT_H; y++) {
			lit += ink_at(x, y) ? 1 : 0;
		}
		walls += (lit >= PLOT_H - 2) ? 1 : 0;
	}
	CHECK(walls == 0);
}

/** Trace inverts, so the cursor shows on the curve it sits on, and draws nowhere else. */
static void test_the_cursor_inverts_around_its_point(void) {
	qdos_graph_view v;
	qdos_graph_standard(&v);
	static qdos_graph_samples s;
	qdos_graph_sample(&s, &v, W, f_identity, NULL);

	clear();
	qdos_graph_draw_cursor(&AREA, &v, &s, 300);
	int lit = 0;
	for (int i = 0; i < W * H; i++) {
		lit += g_fb[i] < 0x80 ? 1 : 0;
	}
	CHECK(lit == 16); // four arms of four

	// Twice is none: it is an inversion, not ink
	qdos_graph_draw_cursor(&AREA, &v, &s, 300);
	lit = 0;
	for (int i = 0; i < W * H; i++) {
		lit += g_fb[i] < 0x80 ? 1 : 0;
	}
	CHECK(lit == 0);

	// No value there, no cursor
	qdos_graph_sample(&s, &v, W, f_sqrt, NULL);
	clear();
	qdos_graph_draw_cursor(&AREA, &v, &s, 10);
	CHECK(!ink_at(10, TOP + PLOT_H / 2));
}

/** A zoom deep enough to lose precision still ends, rather than looping on ticks. */
static void test_a_deep_zoom_still_draws(void) {
	qdos_graph_view v = {1e15, 1e15 + 1e-1, -1e-300, 1e-300};
	static qdos_graph_samples s;
	qdos_graph_sample(&s, &v, W, f_identity, NULL);
	clear();
	qdos_graph_draw(&AREA, &v, &s);
	CHECK(true); // reaching here is the test
}

static bool f_ripple(void* user, double x, double y, double* z) {
	(void)user;
	*z = sin(sqrt(x * x + y * y));
	return true;
}

/* The front half, y < 0, raised a step above the back */
static bool f_step(void* user, double x, double y, double* z) {
	(void)user;
	(void)x;
	*z = (y < 0.0) ? 1.0 : 0.0;
	return true;
}

/* The same, with everything behind the step's foot cut away */
static bool f_step_cut(void* user, double x, double y, double* z) {
	if (y > 1.0) {
		return false;
	}
	return f_step(user, x, y, z);
}

/* A hole in the middle, the way a word that fails there leaves one */
static bool f_holed(void* user, double x, double y, double* z) {
	(void)user;
	if (fabs(x) < 2.0 && fabs(y) < 2.0) {
		return false;
	}
	*z = x + y;
	return true;
}

static int ink_count(void) {
	int lit = 0;
	for (int i = 0; i < W * H; i++) {
		lit += g_fb[i] < 0x80 ? 1 : 0;
	}
	return lit;
}

static void test_a_surface_is_sampled_on_its_grid(void) {
	static qdos_surface s;
	CHECK(qdos_surface_sample(&s, 24, -10, 10, -5, 5, f_ripple, NULL) == 24 * 24);
	CHECK(s.n == 24);
	CHECK(s.z0 >= -1.0 && s.z1 <= 1.0 && s.z0 < s.z1);

	// Corner to corner, ends included
	CHECK(fabs(s.z[0][0] - sin(sqrt(125.0))) < 1e-12);
	CHECK(fabs(s.z[23][23] - sin(sqrt(125.0))) < 1e-12);

	// Holes are holes, and do not count towards the range
	CHECK(qdos_surface_sample(&s, 24, -10, 10, -10, 10, f_holed, NULL) < 24 * 24);
	CHECK(!s.ok[12][12] && s.ok[0][0]);

	// The grid is kept inside what the arrays hold
	CHECK(qdos_surface_sample(&s, 1000, -1, 1, -1, 1, f_ripple, NULL) == QDOS_SURFACE_MAX * QDOS_SURFACE_MAX);
}

/** From every direction, including below, nothing lands outside the plot. */
static void test_a_surface_stays_in_the_area(void) {
	static qdos_surface s;
	qdos_surface_sample(&s, 24, -10, 10, -10, 10, f_ripple, NULL);

	int outside = 0, empty = 0;
	for (int az = 0; az < 360; az += 15) {
		for (int el = -80; el <= 80; el += 20) {
			const qdos_surface_view v = {az, el, 1.0};
			clear();
			qdos_surface_draw(&AREA, &s, &v);
			empty += ink_count() == 0 ? 1 : 0;
			for (int y = 0; y < H; y++) {
				if (y >= TOP && y < TOP + PLOT_H) {
					continue;
				}
				for (int x = 0; x < W; x++) {
					outside += ink_at(x, y) ? 1 : 0;
				}
			}
		}
	}
	CHECK(outside == 0);
	CHECK(empty == 0);

	// Zoomed far in it is clipped, not drawn off the edge of the buffer
	const qdos_surface_view close = {30, 25, 8.0};
	clear();
	qdos_surface_draw(&AREA, &s, &close);
	CHECK(ink_count() > 0);
}

/**
 * What is behind is hidden: seen nearly edge on, a raised front half hides
 * the back half entirely, so cutting the back away changes nothing drawn.
 */
static void test_the_front_hides_the_back(void) {
	static qdos_surface whole, cut;
	qdos_surface_sample(&whole, 24, -10, 10, -10, 10, f_step, NULL);
	qdos_surface_sample(&cut, 24, -10, 10, -10, 10, f_step_cut, NULL);
	CHECK(whole.z0 == cut.z0 && whole.z1 == cut.z1);

	const qdos_surface_view v = {0, 5, 1.0};
	static uint8_t a[W * H];
	clear();
	qdos_surface_draw(&AREA, &whole, &v);
	memcpy(a, g_fb, sizeof(a));
	clear();
	qdos_surface_draw(&AREA, &cut, &v);

	int differ = 0;
	for (int i = 0; i < W * H; i++) {
		differ += ((a[i] < 0x80) != (g_fb[i] < 0x80)) ? 1 : 0;
	}
	CHECK(differ == 0);

	// And from high above, where the back is in plain sight, it is drawn
	const qdos_surface_view above = {0, 70, 1.0};
	clear();
	qdos_surface_draw(&AREA, &whole, &above);
	const int all = ink_count();
	clear();
	qdos_surface_draw(&AREA, &cut, &above);
	CHECK(all > ink_count());
}

/** A whole turn is the same view. */
static void test_a_full_turn_is_the_same_picture(void) {
	static qdos_surface s;
	qdos_surface_sample(&s, 24, -10, 10, -10, 10, f_ripple, NULL);

	static uint8_t a[W * H];
	const qdos_surface_view v = {30, 25, 1.0}, turned = {390, 25, 1.0};
	clear();
	qdos_surface_draw(&AREA, &s, &v);
	memcpy(a, g_fb, sizeof(a));
	clear();
	qdos_surface_draw(&AREA, &s, &turned);
	CHECK(memcmp(a, g_fb, sizeof(a)) == 0);
}

/** A slot's body is a surface when it uses y as a word, a curve when it does not. */
static void test_a_body_says_what_it_plots_as(void) {
	CHECK(qdos_graph_body_shape("x sin x *") == QDOS_GRAPH_CURVE);
	CHECK(qdos_graph_body_shape("x x * y y * +") == QDOS_GRAPH_SURFACE);
	CHECK(qdos_graph_body_shape("  y  ") == QDOS_GRAPH_SURFACE);
	CHECK(qdos_graph_body_shape("") == QDOS_GRAPH_NONE);
	CHECK(qdos_graph_body_shape("   ") == QDOS_GRAPH_NONE);

	// y inside another word is not y
	CHECK(qdos_graph_body_shape("x yx * y2 +") == QDOS_GRAPH_CURVE);
	CHECK(qdos_graph_body_shape("3") == QDOS_GRAPH_CURVE); // a constant is a flat curve
}

static int ink_in_plot(void) {
	int lit = 0;
	for (int y = TOP; y < TOP + PLOT_H; y++) {
		for (int x = 0; x < W; x++) {
			lit += ink_at(x, y) ? 1 : 0;
		}
	}
	return lit;
}

/** Dashed and dotted are the solid curve with gaps in it, so several can be told apart. */
static void test_curves_can_be_told_apart(void) {
	qdos_graph_view v;
	qdos_graph_standard(&v);
	static qdos_graph_samples s;
	qdos_graph_sample(&s, &v, W, f_shallow, NULL);

	int ink[3];
	static uint8_t solid[W * H];
	const qdos_graph_style styles[3] = {QDOS_GRAPH_SOLID, QDOS_GRAPH_DASHED, QDOS_GRAPH_DOTTED};
	for (int i = 0; i < 3; i++) {
		clear();
		qdos_graph_draw_curve(&AREA, &v, &s, styles[i]);
		ink[i] = ink_in_plot();
		if (i == 0) {
			memcpy(solid, g_fb, sizeof(solid));
			continue;
		}

		// Every pixel of a broken line is one the solid line has
		int stray = 0;
		for (int p = 0; p < W * H; p++) {
			stray += (g_fb[p] < 0x80 && solid[p] >= 0x80) ? 1 : 0;
		}
		CHECK(stray == 0);
	}

	// About half for dashes, four on and four off; a third for dots
	CHECK(ink[1] > ink[0] * 4 / 10 && ink[1] < ink[0] * 6 / 10);
	CHECK(ink[2] > ink[0] * 25 / 100 && ink[2] < ink[0] * 42 / 100);

	// And without the axes, which the curve alone does not draw
	clear();
	qdos_graph_draw_axes(&AREA, &v);
	const int axes = ink_in_plot();
	clear();
	qdos_graph_draw(&AREA, &v, &s);
	const int both = ink_in_plot();
	CHECK(axes > 0);
	CHECK(both <= axes + ink[0]);				   // the two may share a pixel where they cross
	CHECK(both >= axes + ink[0] - 4);			   // but no more than that
	CHECK(both > (axes > ink[0] ? axes : ink[0])); // and each adds to the other
}

int main(void) {
	test_sampling_takes_one_value_per_column();
	test_a_function_with_no_value_leaves_a_gap();
	test_fit_shows_every_value_with_a_margin();
	test_fit_gives_a_constant_some_height();
	test_ticks_are_one_two_or_five();
	test_pan_and_zoom();
	test_the_identity_draws_the_diagonal();
	test_a_shallow_line_is_one_pixel_per_column();
	test_drawing_stays_in_the_area();
	test_a_pole_is_not_joined();
	test_the_cursor_inverts_around_its_point();
	test_a_deep_zoom_still_draws();
	test_a_surface_is_sampled_on_its_grid();
	test_a_surface_stays_in_the_area();
	test_the_front_hides_the_back();
	test_a_full_turn_is_the_same_picture();
	test_a_body_says_what_it_plots_as();
	test_curves_can_be_told_apart();
	return check_report("graph");
}
