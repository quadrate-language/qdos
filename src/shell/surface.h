/**
 * @file surface.h
 * @brief Plotting z = f(x, y) as a wireframe, hidden lines removed
 *
 * The grid is sampled once and kept, so turning the view only reprojects it:
 * f is interpreted Quadrate and slow next to arithmetic, and a surface that
 * called it again for every step of a rotation could not be turned at all.
 */

#ifndef QDOS_SURFACE_H
#define QDOS_SURFACE_H

#include "graph.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Grid points along each side, at most */
#define QDOS_SURFACE_MAX 32

/** @brief What a surface is sampled at unless asked otherwise */
#define QDOS_SURFACE_DEFAULT 24

typedef struct {
	int n;										  ///< Grid points along each side
	double x0, x1;								  ///< The grid's extent in x
	double y0, y1;								  ///< and in y
	double z0, z1;								  ///< The lowest and highest value sampled; equal for a flat one
	double z[QDOS_SURFACE_MAX][QDOS_SURFACE_MAX]; ///< [i][j] at x_i, y_j
	bool ok[QDOS_SURFACE_MAX][QDOS_SURFACE_MAX];
} qdos_surface;

/** @brief f(x, y); false where it has no value, which leaves a hole */
typedef bool (*qdos_surface_fn)(void* user, double x, double y, double* z);

/** @brief Where it is looked at from */
typedef struct {
	double azimuth;	  ///< Degrees round the z axis
	double elevation; ///< Degrees above the xy plane
	double zoom;	  ///< 1 fits the whole box in the area
} qdos_surface_view;

void qdos_surface_standard(qdos_surface_view* view);

/**
 * @brief f at every grid point, and the range of what came back
 * @return How many points had a value
 */
int qdos_surface_sample(
		qdos_surface* s, int n, double x0, double x1, double y0, double y1, qdos_surface_fn fn, void* user);

/**
 * @brief The wireframe, each cell drawn over whatever is behind it
 *
 * Needs the area's paper as well as its ink: a cell is filled with paper to
 * hide what is behind it, then outlined in ink.
 */
void qdos_surface_draw(const qdos_graph_area* area, const qdos_surface* s, const qdos_surface_view* view);

#ifdef __cplusplus
}
#endif

#endif // QDOS_SURFACE_H
