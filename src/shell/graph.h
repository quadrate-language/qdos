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
	double x0, x1; ///< Left and right edges, x0 < x1
	double y0, y1; ///< Bottom and top edges, y0 < y1
} qdos_graph_view;

typedef struct {
	int columns;
	double y[QDOS_GRAPH_MAX_COLUMNS];
	bool ok[QDOS_GRAPH_MAX_COLUMNS]; ///< False where f failed or was not finite
} qdos_graph_samples;

/** @brief f(x); false where it has no value, which leaves a gap in the curve */
typedef bool (*qdos_graph_fn)(void* user, double x, double* y);

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
	QDOS_GRAPH_NONE = 0,   ///< Empty
	QDOS_GRAPH_CURVE = 1,  ///< In x alone: y = f(x)
	QDOS_GRAPH_SURFACE = 2 ///< Using y as well: z = f(x, y)
} qdos_graph_shape;

/** @brief A curve's shape, to tell several apart on one pair of axes */
typedef enum {
	QDOS_GRAPH_SOLID = 0,
	QDOS_GRAPH_DASHED,
	QDOS_GRAPH_DOTTED
} qdos_graph_style;

/** @brief A body mentioning y as a word of its own is a surface, one without is a curve */
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

/** @brief Move the window by a fraction of its size; positive is right and up */
void qdos_graph_pan(qdos_graph_view* view, double dx, double dy);

/** @brief Scale the window about (cx, cy); a factor under 1 zooms in */
void qdos_graph_zoom(qdos_graph_view* view, double cx, double cy, double factor);

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

/** @brief A cross on the curve at @p col, for trace */
void qdos_graph_draw_cursor(
		const qdos_graph_area* area, const qdos_graph_view* view, const qdos_graph_samples* s, int col);

#ifdef __cplusplus
}
#endif

#endif // QDOS_GRAPH_H
