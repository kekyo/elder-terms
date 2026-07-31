#pragma once

#include <elder-terms/export.h>

namespace elder_terms {

/**
 * Binds the elder-terms gettext domain to its locale directory.
 *
 * @remarks Call this after setting the process locale and before creating
 * translated widgets. The ELDER_TERMS_LOCALE_DIR environment variable may
 * override the installed locale directory for development and tests.
 */
ELDER_TERMS_API void initialize_localization();

} // namespace elder_terms
