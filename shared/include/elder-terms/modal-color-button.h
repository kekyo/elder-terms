#pragma once

#include <elder-terms/export.h>
#include <gtk/gtk.h>

namespace elder_terms {

/**
 * Creates a color button whose chooser uses the shared modal lifecycle.
 * @return New floating widget, to be owned by its container or the caller.
 * @remarks Call on the GTK thread. The button supports GtkColorChooser and
 * the color-set signal, including the standard color drag-and-drop behavior.
 * Its chooser inherits the button's title, alpha and editor preferences.
 */
ELDER_TERMS_API GtkWidget *create_modal_color_button();

} // namespace elder_terms
