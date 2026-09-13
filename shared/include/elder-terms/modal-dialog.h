#pragma once

#include <gtk/gtk.h>
#include <elder-terms/export.h>

namespace elder_terms {

/**
 * Shows a dialog using parent-scoped input blocking instead of GTK modality.
 * @param dialog Dialog to show. Its contents and response handling belong to the caller.
 * @param parent Parent window, or null for an independent dialog.
 * @pre Parent must not be the dialog itself or one of its managed descendants.
 * @remarks Call on the GTK thread after configuring the dialog. Repeated calls
 * only present an already open dialog. Hiding or destroying it releases its
 * parent's input block; a response alone does not. Parent destruction closes
 * its descendants. The helper owns its bookkeeping, not the caller's state.
 */
ELDER_TERMS_API void show_modal_dialog(GtkWidget *dialog, GtkWindow *parent);

/**
 * Presents a window's most recently opened, deepest modal descendant.
 * @param window Window or dialog to present; null is ignored.
 * @remarks Presents the window itself when it has no modal descendants.
 * Call on the GTK thread, including when presenting an already open dialog.
 */
ELDER_TERMS_API void present_modal_dialog(GtkWidget *window);

/**
 * Reports whether a window is blocked by an open modal child.
 * @param window Window to query; null has no modal children.
 * @return True while at least one managed child remains open.
 * @remarks Call on the GTK thread. Use this for application-specific input
 * eligibility in addition to connection, transfer, or other domain state.
 */
ELDER_TERMS_API bool has_modal_dialog(GtkWidget *window);

} // namespace elder_terms
