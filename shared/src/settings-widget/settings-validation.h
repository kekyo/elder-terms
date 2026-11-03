// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once

#include <gtk/gtk.h>
#include <string>

namespace elder_terms {

/**
 * Presents an input error on a control, preserving its ordinary help text.
 * @param widget Control owned by its GTK parent; nullptr is ignored.
 * @param reason Validation reason, or empty to restore ordinary presentation.
 * @remarks State is owned by the widget. Entry error icons are not activatable.
 */
void set_settings_input_error(GtkWidget *widget, const std::string &reason);

/**
 * Reads the current explanation attached to a control.
 * @param widget Control, or nullptr.
 * @returns Localized error message, or empty when the control is valid.
 */
std::string settings_input_error(GtkWidget *widget);

} // namespace elder_terms
