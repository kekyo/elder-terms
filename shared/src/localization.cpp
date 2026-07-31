#include <elder-terms/localization.h>

#include <clocale>
#include <cstdlib>
#include <string>

#include <glib.h>
#include <libintl.h>

#include "localization-config.h"

namespace elder_terms {

static constexpr char gettext_package[] = "elder-terms";
static constexpr char locale_directory_environment[] =
    "ELDER_TERMS_LOCALE_DIR";
static constexpr char language_environment[] = "LANGUAGE";

static bool is_c_message_locale(const char *locale) {
  if (locale == nullptr) {
    return true;
  }
  const std::string value(locale);
  return value == "C" || value == "POSIX" || value == "C.UTF-8" ||
         value == "C.utf8";
}

static bool select_japanese_message_locale() {
  return std::setlocale(LC_MESSAGES, "ja_JP.UTF-8") != nullptr ||
         std::setlocale(LC_MESSAGES, "ja_JP.utf8") != nullptr;
}

LocalizationInitializationResult
initialize_localization(ApplicationUiLanguage language) {
  LocalizationInitializationResult result;
  if (std::setlocale(LC_ALL, "") == nullptr) {
    result.requested_language_applied = false;
    result.warnings.push_back(
        "Warning: unable to select the system locale for UI text");
  }

  const char *requested_language = nullptr;
  if (language == ApplicationUiLanguage::english) {
    requested_language = "en";
  } else if (language == ApplicationUiLanguage::japanese) {
    requested_language = "ja";
  }
  if (requested_language != nullptr &&
      !g_setenv(language_environment, requested_language, TRUE)) {
    result.requested_language_applied = false;
    result.warnings.push_back(
        "Warning: unable to apply the configured UI language");
  }

  if (language == ApplicationUiLanguage::japanese &&
      is_c_message_locale(std::setlocale(LC_MESSAGES, nullptr)) &&
      !select_japanese_message_locale()) {
    result.requested_language_applied = false;
    result.warnings.push_back(
        "Warning: Japanese UI text requires an installed ja_JP UTF-8 locale");
  }

  const char *override_directory =
      g_getenv(locale_directory_environment);
  const char *locale_directory =
      override_directory != nullptr && override_directory[0] != '\0'
          ? override_directory
          : ELDER_TERMS_LOCALE_DIR;
  bindtextdomain(gettext_package, locale_directory);
  bind_textdomain_codeset(gettext_package, "UTF-8");
  return result;
}

} // namespace elder_terms
