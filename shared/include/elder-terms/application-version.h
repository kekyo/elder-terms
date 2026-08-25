#pragma once

#include <elder-terms/export.h>

namespace elder_terms {

/**
 * Returns the version of the running elder-terms build.
 *
 * @returns NUL-terminated version string embedded at build time.
 */
ELDER_TERMS_API const char *application_version();

} // namespace elder_terms
