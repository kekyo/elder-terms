#pragma once

#include <elder-terms/export.h>

namespace elder_terms {

/**
 * Registers the bundled application icons for subsequently created windows.
 *
 * @remarks GTK must be initialized before this function is called. When a
 * bundled icon cannot be loaded, the current GTK default icon list is left
 * unchanged.
 * @returns True when every bundled icon size was registered.
 */
ELDER_TERMS_API bool initialize_application_window_icon();

} // namespace elder_terms
