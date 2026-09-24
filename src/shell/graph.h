/**
 * @file graph.h
 * @brief Plotting y = f(x) into a framebuffer
 *
 * Nothing here knows about the interpreter: the function arrives as a
 * callback, so the sampling, scaling and drawing are tested with plain C.
 */

#ifndef QDOS_GRAPH_H
#define QDOS_GRAPH_H

#include <qdos/hal.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief One sample per pixel column, so the curve is as fine as the panel */
#define QDOS_GRAPH_MAX_COLUMNS QDOS_SCREEN_W

/** @brief The window onto the plane */
typedef struct {
	double x0, x1;	   ///< Left and right edges, x0 < x1
	double y0, y1;	   ///< Bottom and top edges, y0 < y1
	double xscl, yscl; ///< Between tick marks; nought picks a spacing to suit the range
} qdos_graph_view;

typedef struct {
	int columns;
	double y[QDOS_GRAPH_MAX_COLUMNS];
	bool ok[QDOS_GRAPH_MAX_COLUMNS]; ///< False where f failed or was not finite
} qdos_graph_samples;

/** @brief f(x); false where it has no value, which leaves a gap in the curve */
typedef bool (*qdos_graph_fn)(void* user, double x, double* y);

/** @brief As many points as a parametric or polar curve is drawn through */
#define QDOS_GRAPH_MAX_POINTS 1024

/** @brief A curve taken at steps of t rather than across the columns */
typedef struct {
	int count;
	double t0, step; ///< Point k is at t0 + k step
	double x[QDOS_GRAPH_MAX_POINTS];
	double y[QDOS_GRAPH_MAX_POINTS];
	bool ok[QDOS_GRAPH_MAX_POINTS];
} qdos_graph_points;

/** @brief (x, y) at t; false where there is no point */
typedef bool (*qdos_graph_fn2)(void* user, double t, double* x, double* y);

/** @brief Where on the plot is drawn, in framebuffer pixels */
typedef struct {
	uint8_t* fb;
	int stride; ///< Pixels per framebuffer row
	int left, top, width, height;
	uint8_t ink;
	uint8_t paper; ///< What a surface fills its cells with, to hide what is behind
} qdos_graph_area;

/** @brief What a Y= slot's body plots as */
typedef enum {
	QDOS_GRAPH_NONE = 0,	///< Empty
	QDOS_GRAPH_CURVE = 1,	///< In x alone: y = f(x)
	QDOS_GRAPH_SURFACE = 2, ///< Using y as well: z = f(x, y)
	QDOS_GRAPH_PARAM = 3,	///< In t, leaving x and y
	QDOS_GRAPH_POLAR = 4	///< In theta, leaving r
} qdos_graph_shape;

/** @brief A curve's shape, to tell several apart on one pair of axes */
typedef enum {
	QDOS_GRAPH_SOLID = 0,
	QDOS_GRAPH_DASHED,
	QDOS_GRAPH_DOTTED
} qdos_graph_style;

/**
 * @brief What a body plots as, by the variables it names
 *
 * y makes a surface, t a parametric curve, theta a polar one; a body naming
 * none of them is a curve in x. The first of those found, in that order, wins.
 */
qdos_graph_shape qdos_graph_body_shape(const char* body);

/** @brief The standard window, -10 to 10 on both axes */
void qdos_graph_standard(qdos_graph_view* view);

/** @brief x at the middle of @p col of @p columns */
double qdos_graph_x(const qdos_graph_view* view, int col, int columns);

/** @brief f at every column; returns how many had a value */
int qdos_graph_sample(qdos_graph_samples* s, const qdos_graph_view* view, int columns, qdos_graph_fn fn, void* user);

/**
 * @brief Set y0 and y1 to show every value sampled, with a margin
 * @return false, leaving the view alone, when there is nothing to fit
 */
bool qdos_graph_fit(const qdos_graph_samples* s, qdos_graph_view* view);

/** @brief y that pixel row @p row of @p rows stands for, the top one being y1 */
double qdos_graph_y(const qdos_graph_view* view, int row, int rows);

/** @brief The column whose middle is nearest @p x; may be off the plot */
int qdos_graph_col(const qdos_graph_view* view, double x, int columns);

/** @brief fn at t0, t0 + step, ... up to t1; returns how many had a value */
int qdos_graph_sample_points(qdos_graph_points* p, double t0, double t1, double step, qdos_graph_fn2 fn, void* user);

/** @brief As qdos_graph_fit, for points: y0 and y1 to show them all */
bool qdos_graph_fit_points(const qdos_graph_points* p, qdos_graph_view* view);

/** @brief Move the window by a fraction of its size; positive is right and up */
void qdos_graph_pan(qdos_graph_view* view, double dx, double dy);

/** @brief Scale the window about (cx, cy); a factor under 1 zooms in */
void qdos_graph_zoom(qdos_graph_view* view, double cx, double cy, double factor);

/**
 * @brief Each pixel a round 0.05 apart in both directions, 0 on a column and a row
 *
 * The standard window puts no column on a whole number, since columns are
 * sampled at their middles; this one does, so trace can land on them.
 */
void qdos_graph_decimal(qdos_graph_view* view, int columns, int rows);

/** @brief One unit a pixel, about the whole number nearest the middle */
void qdos_graph_integer(qdos_graph_view* view, int columns, int rows);

/** @brief Widen x or y about the middle so a unit is as long on both axes */
void qdos_graph_square(qdos_graph_view* view, int columns, int rows);

/** @brief Two turns across, a column every 1.875 degrees, ticks every quarter turn */
void qdos_graph_trig(qdos_graph_view* view, int columns, int rows, bool degrees);

/**
 * @brief A spacing for tick marks: 1, 2 or 5 times a power of ten
 *
 * The largest such step that still gives at least @p ticks across @p range.
 */
double qdos_graph_tick_step(double range, int ticks);

/** @brief Axes where they are in view, with ticks, and the curve over them, solid */
void qdos_graph_draw(const qdos_graph_area* area, const qdos_graph_view* view, const qdos_graph_samples* s);

/** @brief The axes alone, for drawing several curves over */
void qdos_graph_draw_axes(const qdos_graph_area* area, const qdos_graph_view* view);

/** @brief One curve, in @p style, without the axes */
void qdos_graph_draw_curve(
		const qdos_graph_area* area, const qdos_graph_view* view, const qdos_graph_samples* s, qdos_graph_style style);

/** @brief A dot at every crossing of the tick marks */
void qdos_graph_draw_grid(const qdos_graph_area* area, const qdos_graph_view* view);

/** @brief Points joined in order, where both ends of a step have a value */
void qdos_graph_draw_points(
		const qdos_graph_area* area, const qdos_graph_view* view, const qdos_graph_points* p, qdos_graph_style style);

/** @brief A straight line between two points on the plane, cut to the plot */
void qdos_graph_draw_segment(const qdos_graph_area* area, const qdos_graph_view* view, double x0, double y0, double x1,
		double y1, qdos_graph_style style);

/** @brief Dither between the curve and the x axis over the columns from @p a to @p b */
void qdos_graph_shade(
		const qdos_graph_area* area, const qdos_graph_view* view, const qdos_graph_samples* s, double a, double b);

/** @brief A small square round (x, y), a scatter plot's mark */
void qdos_graph_draw_mark(const qdos_graph_area* area, const qdos_graph_view* view, double x, double y);

/** @brief The outline of a rectangle on the plane */
void qdos_graph_draw_rect(
		const qdos_graph_area* area, const qdos_graph_view* view, double x0, double y0, double x1, double y1);

/**
 * @brief A box and whiskers across the upper part of the plot
 *
 * Its height is fixed rather than on the y axis, which a box plot has no use for.
 */
void qdos_graph_draw_boxplot(const qdos_graph_area* area, const qdos_graph_view* view, double min, double q1,
		double median, double q3, double max);

/** @brief A cross at (x, y), inverted so it shows on whatever it is over */
void qdos_graph_draw_cursor_at(const qdos_graph_area* area, const qdos_graph_view* view, double x, double y);

/** @brief A cross on the curve at @p col, for trace */
void qdos_graph_draw_cursor(
		const qdos_graph_area* area, const qdos_graph_view* view, const qdos_graph_samples* s, int col);

#ifdef __cplusplus
}
#endif

#endif // QDOS_GRAPH_H
