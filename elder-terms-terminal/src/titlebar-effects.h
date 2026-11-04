// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once
#include "titlebar-effect-state.h"
#include <gtk/gtk.h>

namespace elder_terms {

/**
 * Installs background effects on an existing header without changing its layout.
 * @param header GtkHeaderBar owning the state and frame callback until destroyed.
 * @remarks Calling twice is harmless. GTK animation preferences are respected.
 */
void install_titlebar_effects(GtkWidget *header);

/**
 * Applies effective preferences, immediately cancelling disabled effects.
 * @param header Header previously installed with install_titlebar_effects().
 * @param bell Visual BEL intensity.
 * @param title Title-change intensity.
 */
void set_titlebar_effect_strengths(GtkWidget *header, TitlebarEffectStrength bell, TitlebarEffectStrength title);

/**
 * Records a final title after synchronously updating the GTK window and label.
 * @param header Installed header.
 * @param title Final visible string.
 */
void notify_titlebar_title(GtkWidget *header, const std::string &title);

/**
 * Notifies the header of BEL independently of audio selection and playback.
 * @param header Installed header.
 */
void notify_titlebar_bell(GtkWidget *header);

/**
 * Reports whether the header owns a frame update callback.
 * @param header Installed header.
 * @returns False when idle, disabled, hidden, minimized or destroyed.
 */
bool titlebar_effects_running(GtkWidget *header);

/**
 * Draws the actual theme background, effect, frame and children in that order.
 * @param header Header whose style, allocation and child widgets are used.
 * @param cr GTK draw context in header coordinates.
 * @param frame Time sample; a zero sample paints the unmodified background.
 * @remarks No child opacity or allocation changes. Also used by the comparison driver.
 */
void draw_titlebar_effect_frame(GtkWidget *header, cairo_t *cr, const TitlebarEffectFrame &frame);

} // namespace elder_terms
