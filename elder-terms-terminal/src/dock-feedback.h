// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
#pragma once
#include <cairo.h>
#include <glib.h>

G_BEGIN_DECLS

/** Action indicated beside the pointer during a terminal drag. */
typedef enum {
    DOCK_FEEDBACK_UNDOCK, /**< Arrow leaving a window. */
    DOCK_FEEDBACK_DOCK    /**< Arrow entering a window. */
} DockFeedback;

/** Width and height of a pointer badge in logical pixels. */
enum { DOCK_FEEDBACK_SIZE = 24 };

/** Draws the badge shared by tab drags and native titlebar moves.
 * @param action Action available at the current pointer location.
 * @param scale Display scale factor in device pixels per logical pixel.
 * @return A new image surface owned by the caller.
 */
cairo_surface_t *dock_feedback_surface(DockFeedback action, int scale);

G_END_DECLS
