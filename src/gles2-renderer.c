/*
 * Copyright © 2012 Intel Corporation
 *
 * Permission to use, copy, modify, distribute, and sell this software and
 * its documentation for any purpose is hereby granted without fee, provided
 * that the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of the copyright holders not be used in
 * advertising or publicity pertaining to distribution of the software
 * without specific, written prior permission.  The copyright holders make
 * no representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <float.h>
#include <assert.h>

#include "compositor.h"

static const char *
egl_error_string(EGLint code)
{
#define MYERRCODE(x) case x: return #x;
	switch (code) {
	MYERRCODE(EGL_SUCCESS)
	MYERRCODE(EGL_NOT_INITIALIZED)
	MYERRCODE(EGL_BAD_ACCESS)
	MYERRCODE(EGL_BAD_ALLOC)
	MYERRCODE(EGL_BAD_ATTRIBUTE)
	MYERRCODE(EGL_BAD_CONTEXT)
	MYERRCODE(EGL_BAD_CONFIG)
	MYERRCODE(EGL_BAD_CURRENT_SURFACE)
	MYERRCODE(EGL_BAD_DISPLAY)
	MYERRCODE(EGL_BAD_SURFACE)
	MYERRCODE(EGL_BAD_MATCH)
	MYERRCODE(EGL_BAD_PARAMETER)
	MYERRCODE(EGL_BAD_NATIVE_PIXMAP)
	MYERRCODE(EGL_BAD_NATIVE_WINDOW)
	MYERRCODE(EGL_CONTEXT_LOST)
	default:
		return "unknown";
	}
#undef MYERRCODE
}

static void
print_egl_error_state(void)
{
	EGLint code;

	code = eglGetError();
	weston_log("EGL error state: %s (0x%04lx)\n",
		egl_error_string(code), (long)code);
}

struct polygon8 {
	GLfloat x[8];
	GLfloat y[8];
	int n;
};

struct clip_context {
	struct {
		GLfloat x;
		GLfloat y;
	} prev;

	struct {
		GLfloat x1, y1;
		GLfloat x2, y2;
	} clip;

	struct {
		GLfloat *x;
		GLfloat *y;
	} vertices;
};

static GLfloat
float_difference(GLfloat a, GLfloat b)
{
	/* http://www.altdevblogaday.com/2012/02/22/comparing-floating-point-numbers-2012-edition/ */
	static const GLfloat max_diff = 4.0f * FLT_MIN;
	static const GLfloat max_rel_diff = 4.0e-5;
	GLfloat diff = a - b;
	GLfloat adiff = fabsf(diff);

	if (adiff <= max_diff)
		return 0.0f;

	a = fabsf(a);
	b = fabsf(b);
	if (adiff <= (a > b ? a : b) * max_rel_diff)
		return 0.0f;

	return diff;
}

/* A line segment (p1x, p1y)-(p2x, p2y) intersects the line x = x_arg.
 * Compute the y coordinate of the intersection.
 */
static GLfloat
clip_intersect_y(GLfloat p1x, GLfloat p1y, GLfloat p2x, GLfloat p2y,
		 GLfloat x_arg)
{
	GLfloat a;
	GLfloat diff = float_difference(p1x, p2x);

	/* Practically vertical line segment, yet the end points have already
	 * been determined to be on different sides of the line. Therefore
	 * the line segment is part of the line and intersects everywhere.
	 * Return the end point, so we use the whole line segment.
	 */
	if (diff == 0.0f)
		return p2y;

	a = (x_arg - p2x) / diff;
	return p2y + (p1y - p2y) * a;
}

/* A line segment (p1x, p1y)-(p2x, p2y) intersects the line y = y_arg.
 * Compute the x coordinate of the intersection.
 */
static GLfloat
clip_intersect_x(GLfloat p1x, GLfloat p1y, GLfloat p2x, GLfloat p2y,
		 GLfloat y_arg)
{
	GLfloat a;
	GLfloat diff = float_difference(p1y, p2y);

	/* Practically horizontal line segment, yet the end points have already
	 * been determined to be on different sides of the line. Therefore
	 * the line segment is part of the line and intersects everywhere.
	 * Return the end point, so we use the whole line segment.
	 */
	if (diff == 0.0f)
		return p2x;

	a = (y_arg - p2y) / diff;
	return p2x + (p1x - p2x) * a;
}

enum path_transition {
	PATH_TRANSITION_OUT_TO_OUT = 0,
	PATH_TRANSITION_OUT_TO_IN = 1,
	PATH_TRANSITION_IN_TO_OUT = 2,
	PATH_TRANSITION_IN_TO_IN = 3,
};

static void
clip_append_vertex(struct clip_context *ctx, GLfloat x, GLfloat y)
{
	*ctx->vertices.x++ = x;
	*ctx->vertices.y++ = y;
}

static enum path_transition
path_transition_left_edge(struct clip_context *ctx, GLfloat x, GLfloat y)
{
	return ((ctx->prev.x >= ctx->clip.x1) << 1) | (x >= ctx->clip.x1);
}

static enum path_transition
path_transition_right_edge(struct clip_context *ctx, GLfloat x, GLfloat y)
{
	return ((ctx->prev.x < ctx->clip.x2) << 1) | (x < ctx->clip.x2);
}

static enum path_transition
path_transition_top_edge(struct clip_context *ctx, GLfloat x, GLfloat y)
{
	return ((ctx->prev.y >= ctx->clip.y1) << 1) | (y >= ctx->clip.y1);
}

static enum path_transition
path_transition_bottom_edge(struct clip_context *ctx, GLfloat x, GLfloat y)
{
	return ((ctx->prev.y < ctx->clip.y2) << 1) | (y < ctx->clip.y2);
}

static void
clip_polygon_leftright(struct clip_context *ctx,
		       enum path_transition transition,
		       GLfloat x, GLfloat y, GLfloat clip_x)
{
	GLfloat yi;

	switch (transition) {
	case PATH_TRANSITION_IN_TO_IN:
		clip_append_vertex(ctx, x, y);
		break;
	case PATH_TRANSITION_IN_TO_OUT:
		yi = clip_intersect_y(ctx->prev.x, ctx->prev.y, x, y, clip_x);
		clip_append_vertex(ctx, clip_x, yi);
		break;
	case PATH_TRANSITION_OUT_TO_IN:
		yi = clip_intersect_y(ctx->prev.x, ctx->prev.y, x, y, clip_x);
		clip_append_vertex(ctx, clip_x, yi);
		clip_append_vertex(ctx, x, y);
		break;
	case PATH_TRANSITION_OUT_TO_OUT:
		/* nothing */
		break;
	default:
		assert(0 && "bad enum path_transition");
	}

	ctx->prev.x = x;
	ctx->prev.y = y;
}

static void
clip_polygon_topbottom(struct clip_context *ctx,
		       enum path_transition transition,
		       GLfloat x, GLfloat y, GLfloat clip_y)
{
	GLfloat xi;

	switch (transition) {
	case PATH_TRANSITION_IN_TO_IN:
		clip_append_vertex(ctx, x, y);
		break;
	case PATH_TRANSITION_IN_TO_OUT:
		xi = clip_intersect_x(ctx->prev.x, ctx->prev.y, x, y, clip_y);
		clip_append_vertex(ctx, xi, clip_y);
		break;
	case PATH_TRANSITION_OUT_TO_IN:
		xi = clip_intersect_x(ctx->prev.x, ctx->prev.y, x, y, clip_y);
		clip_append_vertex(ctx, xi, clip_y);
		clip_append_vertex(ctx, x, y);
		break;
	case PATH_TRANSITION_OUT_TO_OUT:
		/* nothing */
		break;
	default:
		assert(0 && "bad enum path_transition");
	}

	ctx->prev.x = x;
	ctx->prev.y = y;
}

static void
clip_context_prepare(struct clip_context *ctx, const struct polygon8 *src,
		      GLfloat *dst_x, GLfloat *dst_y)
{
	ctx->prev.x = src->x[src->n - 1];
	ctx->prev.y = src->y[src->n - 1];
	ctx->vertices.x = dst_x;
	ctx->vertices.y = dst_y;
}

static int
clip_polygon_left(struct clip_context *ctx, const struct polygon8 *src,
		  GLfloat *dst_x, GLfloat *dst_y)
{
	enum path_transition trans;
	int i;

	clip_context_prepare(ctx, src, dst_x, dst_y);
	for (i = 0; i < src->n; i++) {
		trans = path_transition_left_edge(ctx, src->x[i], src->y[i]);
		clip_polygon_leftright(ctx, trans, src->x[i], src->y[i],
				       ctx->clip.x1);
	}
	return ctx->vertices.x - dst_x;
}

static int
clip_polygon_right(struct clip_context *ctx, const struct polygon8 *src,
		   GLfloat *dst_x, GLfloat *dst_y)
{
	enum path_transition trans;
	int i;

	clip_context_prepare(ctx, src, dst_x, dst_y);
	for (i = 0; i < src->n; i++) {
		trans = path_transition_right_edge(ctx, src->x[i], src->y[i]);
		clip_polygon_leftright(ctx, trans, src->x[i], src->y[i],
				       ctx->clip.x2);
	}
	return ctx->vertices.x - dst_x;
}

static int
clip_polygon_top(struct clip_context *ctx, const struct polygon8 *src,
		 GLfloat *dst_x, GLfloat *dst_y)
{
	enum path_transition trans;
	int i;

	clip_context_prepare(ctx, src, dst_x, dst_y);
	for (i = 0; i < src->n; i++) {
		trans = path_transition_top_edge(ctx, src->x[i], src->y[i]);
		clip_polygon_topbottom(ctx, trans, src->x[i], src->y[i],
				       ctx->clip.y1);
	}
	return ctx->vertices.x - dst_x;
}

static int
clip_polygon_bottom(struct clip_context *ctx, const struct polygon8 *src,
		    GLfloat *dst_x, GLfloat *dst_y)
{
	enum path_transition trans;
	int i;

	clip_context_prepare(ctx, src, dst_x, dst_y);
	for (i = 0; i < src->n; i++) {
		trans = path_transition_bottom_edge(ctx, src->x[i], src->y[i]);
		clip_polygon_topbottom(ctx, trans, src->x[i], src->y[i],
				       ctx->clip.y2);
	}
	return ctx->vertices.x - dst_x;
}

#define max(a, b) (((a) > (b)) ? (a) : (b))
#define min(a, b) (((a) > (b)) ? (b) : (a))
#define clip(x, a, b)  min(max(x, a), b)

/*
 * Compute the boundary vertices of the intersection of the global coordinate
 * aligned rectangle 'rect', and an arbitrary quadrilateral produced from
 * 'surf_rect' when transformed from surface coordinates into global coordinates.
 * The vertices are written to 'ex' and 'ey', and the return value is the
 * number of vertices. Vertices are produced in clockwise winding order.
 * Guarantees to produce either zero vertices, or 3-8 vertices with non-zero
 * polygon area.
 */
static int
calculate_edges(struct weston_surface *es, pixman_box32_t *rect,
		pixman_box32_t *surf_rect, GLfloat *ex, GLfloat *ey)
{
	struct polygon8 polygon;
	struct clip_context ctx;
	int i, n;
	GLfloat min_x, max_x, min_y, max_y;
	struct polygon8 surf = {
		{ surf_rect->x1, surf_rect->x2, surf_rect->x2, surf_rect->x1 },
		{ surf_rect->y1, surf_rect->y1, surf_rect->y2, surf_rect->y2 },
		4
	};

	ctx.clip.x1 = rect->x1;
	ctx.clip.y1 = rect->y1;
	ctx.clip.x2 = rect->x2;
	ctx.clip.y2 = rect->y2;

	/* transform surface to screen space: */
	for (i = 0; i < surf.n; i++)
		weston_surface_to_global_float(es, surf.x[i], surf.y[i],
					       &surf.x[i], &surf.y[i]);

	/* find bounding box: */
	min_x = max_x = surf.x[0];
	min_y = max_y = surf.y[0];

	for (i = 1; i < surf.n; i++) {
		min_x = min(min_x, surf.x[i]);
		max_x = max(max_x, surf.x[i]);
		min_y = min(min_y, surf.y[i]);
		max_y = max(max_y, surf.y[i]);
	}

	/* First, simple bounding box check to discard early transformed
	 * surface rects that do not intersect with the clip region:
	 */
	if ((min_x >= ctx.clip.x2) || (max_x <= ctx.clip.x1) ||
	    (min_y >= ctx.clip.y2) || (max_y <= ctx.clip.y1))
		return 0;

	/* Simple case, bounding box edges are parallel to surface edges,
	 * there will be only four edges.  We just need to clip the surface
	 * vertices to the clip rect bounds:
	 */
	if (!es->transform.enabled) {
		for (i = 0; i < surf.n; i++) {
			ex[i] = clip(surf.x[i], ctx.clip.x1, ctx.clip.x2);
			ey[i] = clip(surf.y[i], ctx.clip.y1, ctx.clip.y2);
		}
		return surf.n;
	}

	/* Transformed case: use a general polygon clipping algorithm to
	 * clip the surface rectangle with each side of 'rect'.
	 * The algorithm is Sutherland-Hodgman, as explained in
	 * http://www.codeguru.com/cpp/misc/misc/graphics/article.php/c8965/Polygon-Clipping.htm
	 * but without looking at any of that code.
	 */
	polygon.n = clip_polygon_left(&ctx, &surf, polygon.x, polygon.y);
	surf.n = clip_polygon_right(&ctx, &polygon, surf.x, surf.y);
	polygon.n = clip_polygon_top(&ctx, &surf, polygon.x, polygon.y);
	surf.n = clip_polygon_bottom(&ctx, &polygon, surf.x, surf.y);

	/* Get rid of duplicate vertices */
	ex[0] = surf.x[0];
	ey[0] = surf.y[0];
	n = 1;
	for (i = 1; i < surf.n; i++) {
		if (float_difference(ex[n - 1], surf.x[i]) == 0.0f &&
		    float_difference(ey[n - 1], surf.y[i]) == 0.0f)
			continue;
		ex[n] = surf.x[i];
		ey[n] = surf.y[i];
		n++;
	}
	if (float_difference(ex[n - 1], surf.x[0]) == 0.0f &&
	    float_difference(ey[n - 1], surf.y[0]) == 0.0f)
		n--;

	if (n < 3)
		return 0;

	return n;
}

static int
texture_region(struct weston_surface *es, pixman_region32_t *region,
		pixman_region32_t *surf_region)
{
	struct weston_compositor *ec = es->compositor;
	GLfloat *v, inv_width, inv_height;
	unsigned int *vtxcnt, nvtx = 0;
	pixman_box32_t *rects, *surf_rects;
	int i, j, k, nrects, nsurf;

	rects = pixman_region32_rectangles(region, &nrects);
	surf_rects = pixman_region32_rectangles(surf_region, &nsurf);

	/* worst case we can have 8 vertices per rect (ie. clipped into
	 * an octagon):
	 */
	v = wl_array_add(&ec->vertices, nrects * nsurf * 8 * 4 * sizeof *v);
	vtxcnt = wl_array_add(&ec->vtxcnt, nrects * nsurf * sizeof *vtxcnt);

	inv_width = 1.0 / es->pitch;
	inv_height = 1.0 / es->geometry.height;

	for (i = 0; i < nrects; i++) {
		pixman_box32_t *rect = &rects[i];
		for (j = 0; j < nsurf; j++) {
			pixman_box32_t *surf_rect = &surf_rects[j];
			GLfloat sx, sy;
			GLfloat ex[8], ey[8];          /* edge points in screen space */
			int n;

			/* The transformed surface, after clipping to the clip region,
			 * can have as many as eight sides, emitted as a triangle-fan.
			 * The first vertex in the triangle fan can be chosen arbitrarily,
			 * since the area is guaranteed to be convex.
			 *
			 * If a corner of the transformed surface falls outside of the
			 * clip region, instead of emitting one vertex for the corner
			 * of the surface, up to two are emitted for two corresponding
			 * intersection point(s) between the surface and the clip region.
			 *
			 * To do this, we first calculate the (up to eight) points that
			 * form the intersection of the clip rect and the transformed
			 * surface.
			 */
			n = calculate_edges(es, rect, surf_rect, ex, ey);
			if (n < 3)
				continue;

			/* emit edge points: */
			for (k = 0; k < n; k++) {
				weston_surface_from_global_float(es, ex[k], ey[k], &sx, &sy);
				/* position: */
				*(v++) = ex[k];
				*(v++) = ey[k];
				/* texcoord: */
				*(v++) = sx * inv_width;
				*(v++) = sy * inv_height;
			}

			vtxcnt[nvtx++] = n;
		}
	}

	return nvtx;
}

static void
triangle_fan_debug(struct weston_surface *surface, int first, int count)
{
	struct weston_compositor *compositor = surface->compositor;
	int i;
	GLushort *buffer;
	GLushort *index;
	int nelems;
	static int color_idx = 0;
	static const GLfloat color[][4] = {
			{ 1.0, 0.0, 0.0, 1.0 },
			{ 0.0, 1.0, 0.0, 1.0 },
			{ 0.0, 0.0, 1.0, 1.0 },
			{ 1.0, 1.0, 1.0, 1.0 },
	};

	nelems = (count - 1 + count - 2) * 2;

	buffer = malloc(sizeof(GLushort) * nelems);
	index = buffer;

	for (i = 1; i < count; i++) {
		*index++ = first;
		*index++ = first + i;
	}

	for (i = 2; i < count; i++) {
		*index++ = first + i - 1;
		*index++ = first + i;
	}

	glUseProgram(compositor->solid_shader.program);
	glUniform4fv(compositor->solid_shader.color_uniform, 1,
			color[color_idx++ % ARRAY_LENGTH(color)]);
	glDrawElements(GL_LINES, nelems, GL_UNSIGNED_SHORT, buffer);
	glUseProgram(compositor->current_shader->program);
	free(buffer);
}

static void
repaint_region(struct weston_surface *es, pixman_region32_t *region,
		pixman_region32_t *surf_region)
{
	struct weston_compositor *ec = es->compositor;
	GLfloat *v;
	unsigned int *vtxcnt;
	int i, first, nfans;

	/* The final region to be painted is the intersection of
	 * 'region' and 'surf_region'. However, 'region' is in the global
	 * coordinates, and 'surf_region' is in the surface-local
	 * coordinates. texture_region() will iterate over all pairs of
	 * rectangles from both regions, compute the intersection
	 * polygon for each pair, and store it as a triangle fan if
	 * it has a non-zero area (at least 3 vertices, actually).
	 */
	nfans = texture_region(es, region, surf_region);

	v = ec->vertices.data;
	vtxcnt = ec->vtxcnt.data;

	/* position: */
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof *v, &v[0]);
	glEnableVertexAttribArray(0);

	/* texcoord: */
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof *v, &v[2]);
	glEnableVertexAttribArray(1);

	for (i = 0, first = 0; i < nfans; i++) {
		glDrawArrays(GL_TRIANGLE_FAN, first, vtxcnt[i]);
		if (ec->fan_debug)
			triangle_fan_debug(es, first, vtxcnt[i]);
		first += vtxcnt[i];
	}

	glDisableVertexAttribArray(1);
	glDisableVertexAttribArray(0);

	ec->vertices.size = 0;
	ec->vtxcnt.size = 0;
}

static void
weston_compositor_use_shader(struct weston_compositor *compositor,
			     struct weston_shader *shader)
{
	if (compositor->current_shader == shader)
		return;

	glUseProgram(shader->program);
	compositor->current_shader = shader;
}

static void
weston_shader_uniforms(struct weston_shader *shader,
		       struct weston_surface *surface,
		       struct weston_output *output)
{
	int i;

	glUniformMatrix4fv(shader->proj_uniform,
			   1, GL_FALSE, output->matrix.d);
	glUniform4fv(shader->color_uniform, 1, surface->color);
	glUniform1f(shader->alpha_uniform, surface->alpha);

	for (i = 0; i < surface->num_textures; i++)
		glUniform1i(shader->tex_uniforms[i], i);
}

static void
draw_surface(struct weston_surface *es, struct weston_output *output,
	     pixman_region32_t *damage) /* in global coordinates */
{
	struct weston_compositor *ec = es->compositor;
	/* repaint bounding region in global coordinates: */
	pixman_region32_t repaint;
	/* non-opaque region in surface coordinates: */
	pixman_region32_t surface_blend;
	pixman_region32_t *buffer_damage;
	pixman_region32_t opaque;
	GLint filter;
	int i;

	pixman_region32_init(&repaint);
	pixman_region32_intersect(&repaint,
				  &es->transform.boundingbox, damage);
	pixman_region32_subtract(&repaint, &repaint, &es->clip);

	if (!pixman_region32_not_empty(&repaint))
		goto out;

	buffer_damage = &output->buffer_damage[output->current_buffer];
	pixman_region32_subtract(buffer_damage, buffer_damage, &repaint);

	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

	if (ec->fan_debug) {
		weston_compositor_use_shader(ec, &ec->solid_shader);
		weston_shader_uniforms(&ec->solid_shader, es, output);
	}

	weston_compositor_use_shader(ec, es->shader);
	weston_shader_uniforms(es->shader, es, output);

	if (es->transform.enabled || output->zoom.active)
		filter = GL_LINEAR;
	else
		filter = GL_NEAREST;

	for (i = 0; i < es->num_textures; i++) {
		glActiveTexture(GL_TEXTURE0 + i);
		glBindTexture(es->target, es->textures[i]);
		glTexParameteri(es->target, GL_TEXTURE_MIN_FILTER, filter);
		glTexParameteri(es->target, GL_TEXTURE_MAG_FILTER, filter);
	}

	/* blended region is whole surface minus opaque region: */
	pixman_region32_init_rect(&surface_blend, 0, 0,
				  es->geometry.width, es->geometry.height);
	pixman_region32_init(&opaque);
	pixman_region32_copy(&opaque, &es->opaque);
	if (pixman_region32_not_empty(&es->crop)) {
		pixman_region32_copy(&surface_blend, &es->crop);
		pixman_region32_subtract(&opaque,
				  &opaque, &es->crop);
		if (pixman_region32_not_empty(&opaque))
			pixman_region32_intersect(&opaque,
					  &opaque, &es->crop);
	}
	pixman_region32_subtract(&surface_blend, &surface_blend, &opaque);

	if (pixman_region32_not_empty(&opaque)) {
		if (es->shader == &ec->texture_shader_rgba) {
			/* Special case for RGBA textures with possibly
			 * bad data in alpha channel: use the shader
			 * that forces texture alpha = 1.0.
			 * Xwayland surfaces need this.
			 */
			weston_compositor_use_shader(ec, &ec->texture_shader_rgbx);
			weston_shader_uniforms(&ec->texture_shader_rgbx, es, output);
		}

		if (es->alpha < 1.0)
			glEnable(GL_BLEND);
		else
			glDisable(GL_BLEND);

		repaint_region(es, &repaint, &opaque);
	}
	pixman_region32_fini(&opaque);

	if (pixman_region32_not_empty(&surface_blend)) {
		weston_compositor_use_shader(ec, es->shader);
		glEnable(GL_BLEND);
		repaint_region(es, &repaint, &surface_blend);
	}

	pixman_region32_fini(&surface_blend);

out:
	pixman_region32_fini(&repaint);
}

static void
repaint_surfaces(struct weston_output *output, pixman_region32_t *damage)
{
	struct weston_compositor *compositor = output->compositor;
	struct weston_surface *surface;

	wl_list_for_each_reverse(surface, &compositor->surface_list, link)
		if (surface->plane == &compositor->primary_plane)
			draw_surface(surface, output, damage);
}

static void
gles2_renderer_repaint_output(struct weston_output *output,
			      pixman_region32_t *output_damage)
{
	struct weston_compositor *compositor = output->compositor;
	EGLBoolean ret;
	static int errored;
	int32_t width, height, i;

	width = output->current->width +
		output->border.left + output->border.right;
	height = output->current->height +
		output->border.top + output->border.bottom;

	glViewport(0, 0, width, height);

	ret = eglMakeCurrent(compositor->egl_display, output->egl_surface,
			     output->egl_surface, compositor->egl_context);
	if (ret == EGL_FALSE) {
		if (errored)
			return;
		errored = 1;
		weston_log("Failed to make EGL context current.\n");
		print_egl_error_state();
		return;
	}

	/* if debugging, redraw everything outside the damage to clean up
	 * debug lines from the previous draw on this buffer:
	 */
	if (compositor->fan_debug) {
		pixman_region32_t undamaged;
		pixman_region32_init(&undamaged);
		pixman_region32_subtract(&undamaged, &output->region,
					 output_damage);
		compositor->fan_debug = 0;
		repaint_surfaces(output, &undamaged);
		compositor->fan_debug = 1;
		pixman_region32_fini(&undamaged);
	}

	for (i = 0; i < 2; i++)
		pixman_region32_union(&output->buffer_damage[i],
				      &output->buffer_damage[i],
				      output_damage);

	pixman_region32_union(output_damage, output_damage,
			      &output->buffer_damage[output->current_buffer]);

	repaint_surfaces(output, output_damage);

	wl_signal_emit(&output->frame_signal, output);

	ret = eglSwapBuffers(compositor->egl_display, output->egl_surface);
	if (ret == EGL_FALSE && !errored) {
		errored = 1;
		weston_log("Failed in eglSwapBuffers.\n");
		print_egl_error_state();
	}

	output->current_buffer ^= 1;

}

static void
gles2_renderer_flush_damage(struct weston_surface *surface)
{
#ifdef GL_UNPACK_ROW_LENGTH
	pixman_box32_t *rectangles;
	void *data;
	int i, n;
#endif

	glBindTexture(GL_TEXTURE_2D, surface->textures[0]);

	if (!surface->compositor->has_unpack_subimage) {
		glTexImage2D(GL_TEXTURE_2D, 0, GL_BGRA_EXT,
			     surface->pitch, surface->buffer->height, 0,
			     GL_BGRA_EXT, GL_UNSIGNED_BYTE,
			     wl_shm_buffer_get_data(surface->buffer));

		return;
	}

#ifdef GL_UNPACK_ROW_LENGTH
	/* Mesa does not define GL_EXT_unpack_subimage */
	glPixelStorei(GL_UNPACK_ROW_LENGTH, surface->pitch);
	data = wl_shm_buffer_get_data(surface->buffer);
	rectangles = pixman_region32_rectangles(&surface->damage, &n);
	for (i = 0; i < n; i++) {
		glPixelStorei(GL_UNPACK_SKIP_PIXELS, rectangles[i].x1);
		glPixelStorei(GL_UNPACK_SKIP_ROWS, rectangles[i].y1);
		glTexSubImage2D(GL_TEXTURE_2D, 0,
				rectangles[i].x1, rectangles[i].y1,
				rectangles[i].x2 - rectangles[i].x1,
				rectangles[i].y2 - rectangles[i].y1,
				GL_BGRA_EXT, GL_UNSIGNED_BYTE, data);
	}
#endif
}

static void
ensure_textures(struct weston_surface *es, int num_textures)
{
	int i;

	if (num_textures <= es->num_textures)
		return;

	for (i = es->num_textures; i < num_textures; i++) {
		glGenTextures(1, &es->textures[i]);
		glBindTexture(es->target, es->textures[i]);
		glTexParameteri(es->target,
				GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(es->target,
				GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}
	es->num_textures = num_textures;
	glBindTexture(es->target, 0);
}

static void
gles2_renderer_attach(struct weston_surface *es, struct wl_buffer *buffer)
{
	struct weston_compositor *ec = es->compositor;
	EGLint attribs[3], format;
	int i, num_planes;

	if (!buffer) {
		for (i = 0; i < es->num_images; i++) {
			ec->destroy_image(ec->egl_display, es->images[i]);
			es->images[i] = NULL;
		}
		es->num_images = 0;
		glDeleteTextures(es->num_textures, es->textures);
		es->num_textures = 0;
		return;
	}

	if (wl_buffer_is_shm(buffer)) {
		es->pitch = wl_shm_buffer_get_stride(buffer) / 4;
		es->target = GL_TEXTURE_2D;

		ensure_textures(es, 1);
		glBindTexture(GL_TEXTURE_2D, es->textures[0]);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_BGRA_EXT,
			     es->pitch, buffer->height, 0,
			     GL_BGRA_EXT, GL_UNSIGNED_BYTE, NULL);
		if (wl_shm_buffer_get_format(buffer) == WL_SHM_FORMAT_XRGB8888)
			es->shader = &ec->texture_shader_rgbx;
		else
			es->shader = &ec->texture_shader_rgba;
	} else if (ec->query_buffer(ec->egl_display, buffer,
				    EGL_TEXTURE_FORMAT, &format)) {
		for (i = 0; i < es->num_images; i++)
			ec->destroy_image(ec->egl_display, es->images[i]);
		es->num_images = 0;
		es->target = GL_TEXTURE_2D;
		switch (format) {
		case EGL_TEXTURE_RGB:
		case EGL_TEXTURE_RGBA:
		default:
			num_planes = 1;
			es->shader = &ec->texture_shader_rgba;
			break;
		case EGL_TEXTURE_EXTERNAL_WL:
			num_planes = 1;
			es->target = GL_TEXTURE_EXTERNAL_OES;
			es->shader = &ec->texture_shader_egl_external;
			break;
		case EGL_TEXTURE_Y_UV_WL:
			num_planes = 2;
			es->shader = &ec->texture_shader_y_uv;
			break;
		case EGL_TEXTURE_Y_U_V_WL:
			num_planes = 3;
			es->shader = &ec->texture_shader_y_u_v;
			break;
		case EGL_TEXTURE_Y_XUXV_WL:
			num_planes = 2;
			es->shader = &ec->texture_shader_y_xuxv;
			break;
		}

		ensure_textures(es, num_planes);
		for (i = 0; i < num_planes; i++) {
			attribs[0] = EGL_WAYLAND_PLANE_WL;
			attribs[1] = i;
			attribs[2] = EGL_NONE;
			es->images[i] = ec->create_image(ec->egl_display,
							 NULL,
							 EGL_WAYLAND_BUFFER_WL,
							 buffer, attribs);
			if (!es->images[i]) {
				weston_log("failed to create img for plane %d\n", i);
				continue;
			}
			es->num_images++;

			glActiveTexture(GL_TEXTURE0 + i);
			glBindTexture(es->target, es->textures[i]);
			ec->image_target_texture_2d(es->target,
						    es->images[i]);
		}

		es->pitch = buffer->width;
	} else {
		weston_log("unhandled buffer type!\n");
	}
}

static void
gles2_renderer_destroy_surface(struct weston_surface *surface)
{
	struct weston_compositor *ec = surface->compositor;
	int i;

	glDeleteTextures(surface->num_textures, surface->textures);

	for (i = 0; i < surface->num_images; i++)
		ec->destroy_image(ec->egl_display, surface->images[i]);
}

static const char vertex_shader[] =
	"uniform mat4 proj;\n"
	"attribute vec2 position;\n"
	"attribute vec2 texcoord;\n"
	"varying vec2 v_texcoord;\n"
	"void main()\n"
	"{\n"
	"   gl_Position = proj * vec4(position, 0.0, 1.0);\n"
	"   v_texcoord = texcoord;\n"
	"}\n";

/* Declare common fragment shader uniforms */
#define FRAGMENT_CONVERT_YUV						\
	"  y *= alpha;\n"						\
	"  u *= alpha;\n"						\
	"  v *= alpha;\n"						\
	"  gl_FragColor.r = y + 1.59602678 * v;\n"			\
	"  gl_FragColor.g = y - 0.39176229 * u - 0.81296764 * v;\n"	\
	"  gl_FragColor.b = y + 2.01723214 * u;\n"			\
	"  gl_FragColor.a = alpha;\n"

static const char texture_fragment_shader_rgba[] =
	"precision mediump float;\n"
	"varying vec2 v_texcoord;\n"
	"uniform sampler2D tex;\n"
	"uniform float alpha;\n"
	"void main()\n"
	"{\n"
	"   gl_FragColor = alpha * texture2D(tex, v_texcoord)\n;"
	"}\n";

static const char texture_fragment_shader_rgbx[] =
	"precision mediump float;\n"
	"varying vec2 v_texcoord;\n"
	"uniform sampler2D tex;\n"
	"uniform float alpha;\n"
	"void main()\n"
	"{\n"
	"   gl_FragColor.rgb = alpha * texture2D(tex, v_texcoord).rgb\n;"
	"   gl_FragColor.a = alpha;\n"
	"}\n";

static const char texture_fragment_shader_egl_external[] =
	"#extension GL_OES_EGL_image_external : require\n"
	"precision mediump float;\n"
	"varying vec2 v_texcoord;\n"
	"uniform samplerExternalOES tex;\n"
	"uniform float alpha;\n"
	"void main()\n"
	"{\n"
	"   gl_FragColor = alpha * texture2D(tex, v_texcoord)\n;"
	"}\n";

static const char texture_fragment_shader_y_uv[] =
	"precision mediump float;\n"
	"uniform sampler2D tex;\n"
	"uniform sampler2D tex1;\n"
	"varying vec2 v_texcoord;\n"
	"uniform float alpha;\n"
	"void main() {\n"
	"  float y = 1.16438356 * (texture2D(tex, v_texcoord).x - 0.0625);\n"
	"  float u = texture2D(tex1, v_texcoord).r - 0.5;\n"
	"  float v = texture2D(tex1, v_texcoord).g - 0.5;\n"
	FRAGMENT_CONVERT_YUV
	"}\n";

static const char texture_fragment_shader_y_u_v[] =
	"precision mediump float;\n"
	"uniform sampler2D tex;\n"
	"uniform sampler2D tex1;\n"
	"uniform sampler2D tex2;\n"
	"varying vec2 v_texcoord;\n"
	"uniform float alpha;\n"
	"void main() {\n"
	"  float y = 1.16438356 * (texture2D(tex, v_texcoord).x - 0.0625);\n"
	"  float u = texture2D(tex1, v_texcoord).x - 0.5;\n"
	"  float v = texture2D(tex2, v_texcoord).x - 0.5;\n"
	FRAGMENT_CONVERT_YUV
	"}\n";

static const char texture_fragment_shader_y_xuxv[] =
	"precision mediump float;\n"
	"uniform sampler2D tex;\n"
	"uniform sampler2D tex1;\n"
	"varying vec2 v_texcoord;\n"
	"uniform float alpha;\n"
	"void main() {\n"
	"  float y = 1.16438356 * (texture2D(tex, v_texcoord).x - 0.0625);\n"
	"  float u = texture2D(tex1, v_texcoord).g - 0.5;\n"
	"  float v = texture2D(tex1, v_texcoord).a - 0.5;\n"
	FRAGMENT_CONVERT_YUV
	"}\n";

static const char solid_fragment_shader[] =
	"precision mediump float;\n"
	"uniform vec4 color;\n"
	"uniform float alpha;\n"
	"void main()\n"
	"{\n"
	"   gl_FragColor = alpha * color\n;"
	"}\n";

static int
compile_shader(GLenum type, const char *source)
{
	GLuint s;
	char msg[512];
	GLint status;

	s = glCreateShader(type);
	glShaderSource(s, 1, &source, NULL);
	glCompileShader(s);
	glGetShaderiv(s, GL_COMPILE_STATUS, &status);
	if (!status) {
		glGetShaderInfoLog(s, sizeof msg, NULL, msg);
		weston_log("shader info: %s\n", msg);
		return GL_NONE;
	}

	return s;
}

static int
weston_shader_init(struct weston_shader *shader,
		   const char *vertex_source, const char *fragment_source)
{
	char msg[512];
	GLint status;

	shader->vertex_shader =
		compile_shader(GL_VERTEX_SHADER, vertex_source);
	shader->fragment_shader =
		compile_shader(GL_FRAGMENT_SHADER, fragment_source);

	shader->program = glCreateProgram();
	glAttachShader(shader->program, shader->vertex_shader);
	glAttachShader(shader->program, shader->fragment_shader);
	glBindAttribLocation(shader->program, 0, "position");
	glBindAttribLocation(shader->program, 1, "texcoord");

	glLinkProgram(shader->program);
	glGetProgramiv(shader->program, GL_LINK_STATUS, &status);
	if (!status) {
		glGetProgramInfoLog(shader->program, sizeof msg, NULL, msg);
		weston_log("link info: %s\n", msg);
		return -1;
	}

	shader->proj_uniform = glGetUniformLocation(shader->program, "proj");
	shader->tex_uniforms[0] = glGetUniformLocation(shader->program, "tex");
	shader->tex_uniforms[1] = glGetUniformLocation(shader->program, "tex1");
	shader->tex_uniforms[2] = glGetUniformLocation(shader->program, "tex2");
	shader->alpha_uniform = glGetUniformLocation(shader->program, "alpha");
	shader->color_uniform = glGetUniformLocation(shader->program, "color");

	return 0;
}

static void
log_extensions(const char *name, const char *extensions)
{
	const char *p, *end;
	int l;
	int len;

	l = weston_log("%s:", name);
	p = extensions;
	while (*p) {
		end = strchrnul(p, ' ');
		len = end - p;
		if (l + len > 78)
			l = weston_log_continue("\n" STAMP_SPACE "%.*s",
						len, p);
		else
			l += weston_log_continue(" %.*s", len, p);
		for (p = end; isspace(*p); p++)
			;
	}
	weston_log_continue("\n");
}

static void
log_egl_gl_info(EGLDisplay egldpy)
{
	const char *str;

	str = eglQueryString(egldpy, EGL_VERSION);
	weston_log("EGL version: %s\n", str ? str : "(null)");

	str = eglQueryString(egldpy, EGL_VENDOR);
	weston_log("EGL vendor: %s\n", str ? str : "(null)");

	str = eglQueryString(egldpy, EGL_CLIENT_APIS);
	weston_log("EGL client APIs: %s\n", str ? str : "(null)");

	str = eglQueryString(egldpy, EGL_EXTENSIONS);
	log_extensions("EGL extensions", str ? str : "(null)");

	str = (char *)glGetString(GL_VERSION);
	weston_log("GL version: %s\n", str ? str : "(null)");

	str = (char *)glGetString(GL_SHADING_LANGUAGE_VERSION);
	weston_log("GLSL version: %s\n", str ? str : "(null)");

	str = (char *)glGetString(GL_VENDOR);
	weston_log("GL vendor: %s\n", str ? str : "(null)");

	str = (char *)glGetString(GL_RENDERER);
	weston_log("GL renderer: %s\n", str ? str : "(null)");

	str = (char *)glGetString(GL_EXTENSIONS);
	log_extensions("GL extensions", str ? str : "(null)");
}

struct gles2_renderer {
	struct weston_renderer base;
};

WL_EXPORT void
gles2_renderer_destroy(struct weston_compositor *ec)
{
	if (ec->has_bind_display)
		ec->unbind_display(ec->egl_display, ec->wl_display);
}

WL_EXPORT int
gles2_renderer_init(struct weston_compositor *ec)
{
	struct gles2_renderer *renderer;
	const char *extensions;
	int has_egl_image_external = 0;
	struct weston_output *output;
	EGLBoolean ret;

	static const EGLint context_attribs[] = {
		EGL_CONTEXT_CLIENT_VERSION, 2,
		EGL_NONE
	};

	renderer = malloc(sizeof *renderer);
	if (renderer == NULL)
		return -1;

	if (!eglBindAPI(EGL_OPENGL_ES_API)) {
		weston_log("failed to bind EGL_OPENGL_ES_API\n");
		print_egl_error_state();
		return -1;
	}
	ec->egl_context = eglCreateContext(ec->egl_display, ec->egl_config,
					   EGL_NO_CONTEXT, context_attribs);
	if (ec->egl_context == NULL) {
		weston_log("failed to create context\n");
		print_egl_error_state();
		return -1;
	}

	output = container_of(ec->output_list.next,
			      struct weston_output, link);
	ret = eglMakeCurrent(ec->egl_display, output->egl_surface,
			     output->egl_surface, ec->egl_context);
	if (ret == EGL_FALSE) {
		weston_log("Failed to make EGL context current.\n");
		print_egl_error_state();
		return -1;
	}

	log_egl_gl_info(ec->egl_display);

	ec->image_target_texture_2d =
		(void *) eglGetProcAddress("glEGLImageTargetTexture2DOES");
	ec->image_target_renderbuffer_storage = (void *)
		eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES");
	ec->create_image = (void *) eglGetProcAddress("eglCreateImageKHR");
	ec->destroy_image = (void *) eglGetProcAddress("eglDestroyImageKHR");
	ec->bind_display =
		(void *) eglGetProcAddress("eglBindWaylandDisplayWL");
	ec->unbind_display =
		(void *) eglGetProcAddress("eglUnbindWaylandDisplayWL");
	ec->query_buffer =
		(void *) eglGetProcAddress("eglQueryWaylandBufferWL");

	extensions = (const char *) glGetString(GL_EXTENSIONS);
	if (!extensions) {
		weston_log("Retrieving GL extension string failed.\n");
		return -1;
	}

	if (!strstr(extensions, "GL_EXT_texture_format_BGRA8888")) {
		weston_log("GL_EXT_texture_format_BGRA8888 not available\n");
		return -1;
	}

	if (strstr(extensions, "GL_EXT_read_format_bgra"))
		ec->read_format = GL_BGRA_EXT;
	else
		ec->read_format = GL_RGBA;

	if (strstr(extensions, "GL_EXT_unpack_subimage"))
		ec->has_unpack_subimage = 1;

	if (strstr(extensions, "GL_OES_EGL_image_external"))
		has_egl_image_external = 1;

	extensions =
		(const char *) eglQueryString(ec->egl_display, EGL_EXTENSIONS);
	if (!extensions) {
		weston_log("Retrieving EGL extension string failed.\n");
		return -1;
	}

	if (strstr(extensions, "EGL_WL_bind_wayland_display"))
		ec->has_bind_display = 1;
	if (ec->has_bind_display)
		ec->bind_display(ec->egl_display, ec->wl_display);

	glActiveTexture(GL_TEXTURE0);

	if (weston_shader_init(&ec->texture_shader_rgba,
			     vertex_shader, texture_fragment_shader_rgba) < 0)
		return -1;
	if (weston_shader_init(&ec->texture_shader_rgbx,
			     vertex_shader, texture_fragment_shader_rgbx) < 0)
		return -1;
	if (has_egl_image_external &&
			weston_shader_init(&ec->texture_shader_egl_external,
				vertex_shader, texture_fragment_shader_egl_external) < 0)
		return -1;
	if (weston_shader_init(&ec->texture_shader_y_uv,
			       vertex_shader, texture_fragment_shader_y_uv) < 0)
		return -1;
	if (weston_shader_init(&ec->texture_shader_y_u_v,
			       vertex_shader, texture_fragment_shader_y_u_v) < 0)
		return -1;
	if (weston_shader_init(&ec->texture_shader_y_xuxv,
			       vertex_shader, texture_fragment_shader_y_xuxv) < 0)
		return -1;
	if (weston_shader_init(&ec->solid_shader,
			     vertex_shader, solid_fragment_shader) < 0)
		return -1;

	renderer->base.repaint_output = gles2_renderer_repaint_output;
	renderer->base.flush_damage = gles2_renderer_flush_damage;
	renderer->base.attach = gles2_renderer_attach;
	renderer->base.destroy_surface = gles2_renderer_destroy_surface;
	ec->renderer = &renderer->base;

	return 0;
}
