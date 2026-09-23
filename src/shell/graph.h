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

/**
 * @brief What a function declared in Quadrate can be plotted as
 *
 * Read off the signature, `fn f(x:f64 -- y:f64)`, rather than found by
 * running the word: running it to see is running whatever else it does.
 */
typedef enum {
	QDOS_GRAPH_NONE = 0,
	QDOS_GRAPH_CURVE = 1,  ///< Takes x, leaves y
	QDOS_GRAPH_SURFACE = 2 ///< Takes x and y, leaves z
} qdos_graph_shape;

/** @brief One function found by qdos_graph_scan(); @p name is not terminated */
typedef void (*qdos_graph_visit)(void* user, const char* name, size_t len, qdos_graph_shape shape);

/** @brief Every `fn` declared in @p source, with what it can be plotted as */
void qdos_graph_scan(const char* source, qdos_graph_visit visit, void* user);

/** @brief What a stack effect, `(x:f64 -- r:f64)` with its parentheses, can be plotted as */
qdos_graph_shape qdos_graph_shape_of_signature(const char* signature);

/** @brief What @p name, as declared in @p source, can be plotted as */
qdos_graph_shape qdos_graph_shape_of(const char* source, const char* name);

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

/** @brief Axes where they are in view, with ticks, and the curve over them */
void qdos_graph_draw(const qdos_graph_area* area, const qdos_graph_view* view, const qdos_graph_samples* s);

/** @brief A cross on the curve at @p col, for trace */
void qdos_graph_draw_cursor(
		const qdos_graph_area* area, const qdos_graph_view* view, const qdos_graph_samples* s, int col);

#ifdef __cplusplus
}
#endif

#endif // QDOS_GRAPH_H
