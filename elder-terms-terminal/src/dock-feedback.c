// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
#include "dock-feedback.h"

cairo_surface_t *dock_feedback_surface(DockFeedback action, int scale)
{
    scale = MAX(1, scale);
    int size = DOCK_FEEDBACK_SIZE * scale;
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
    cairo_surface_set_device_scale(surface, scale, scale);
    cairo_t *cr = cairo_create(surface);

    cairo_arc(cr, 18, 6, 5, -G_PI_2, 0);
    cairo_arc(cr, 18, 18, 5, 0, G_PI_2);
    cairo_arc(cr, 6, 18, 5, G_PI_2, G_PI);
    cairo_arc(cr, 6, 6, 5, G_PI, 3 * G_PI_2);
    cairo_close_path(cr);
    if (action == DOCK_FEEDBACK_DOCK) cairo_set_source_rgb(cr, .08, .47, .35);
    else cairo_set_source_rgb(cr, .26, .32, .44);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, .87, .90, .93);
    cairo_set_line_width(cr, 1.5);
    cairo_stroke(cr);

    // Leave the top-right corner of the window open for the arrow.
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_set_line_width(cr, 2);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_move_to(cr, 12, 7);
    cairo_line_to(cr, 6, 7);
    cairo_line_to(cr, 6, 18);
    cairo_line_to(cr, 17, 18);
    cairo_line_to(cr, 17, 13);
    cairo_stroke(cr);
    cairo_move_to(cr, 11, 13);
    cairo_line_to(cr, 18, 6);
    cairo_stroke(cr);
    if (action == DOCK_FEEDBACK_DOCK) {
        cairo_move_to(cr, 11, 9);
        cairo_line_to(cr, 11, 13);
        cairo_line_to(cr, 15, 13);
    } else {
        cairo_move_to(cr, 14, 6);
        cairo_line_to(cr, 18, 6);
        cairo_line_to(cr, 18, 10);
    }
    cairo_stroke(cr);
    cairo_destroy(cr);
    return surface;
}
