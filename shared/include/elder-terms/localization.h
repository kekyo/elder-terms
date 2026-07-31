#pragma once

#include <string>
#include <vector>

#include <elder-terms/export.h>
#include <elder-terms/settings/application-settings.h>

namespace elder_terms {

/**
 * Reports the outcome of process-wide localization initialization.
 */
struct LocalizationInitializationResult {
  /** Whether the requested language could be applied. */
  bool requested_language_applied = true;
  /** Non-fatal warnings encountered while selecting the locale. */
  std::vector<std::string> warnings;
};

/**
 * Selects the process locale and binds the elder-terms gettext domain.
 *
 * @param language Configured UI language.
 * @returns Whether the language was applied and any non-fatal warnings.
 *
 * @remarks Call this before GTK initialization or any translated text lookup.
 * The ELDER_TERMS_LOCALE_DIR environment variable may override the installed
 * or automatically detected build-tree locale directory. Call
 * gtk_disable_setlocale() before gtk_init() so GTK preserves the selected
 * locale.
 */
ELDER_TERMS_API LocalizationInitializationResult
initialize_localization(ApplicationUiLanguage language);

} // namespace elder_terms
