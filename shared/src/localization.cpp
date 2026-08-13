#include <elder-terms/localization.h>

#include <clocale>
#include <cstdlib>
#include <filesystem>
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

static const char *message_locale_prefix(ApplicationUiLanguage language) {
  switch (language) {
  case ApplicationUiLanguage::arabic:
    return "ar_SA";
  case ApplicationUiLanguage::spanish:
    return "es_ES";
  case ApplicationUiLanguage::french:
    return "fr_FR";
  case ApplicationUiLanguage::hindi:
    return "hi_IN";
  case ApplicationUiLanguage::japanese:
    return "ja_JP";
  case ApplicationUiLanguage::korean:
    return "ko_KR";
  case ApplicationUiLanguage::portuguese:
    return "pt_PT";
  case ApplicationUiLanguage::russian:
    return "ru_RU";
  case ApplicationUiLanguage::chinese:
    return "zh_CN";
  case ApplicationUiLanguage::system:
  case ApplicationUiLanguage::english:
    return nullptr;
  }
  return nullptr;
}

static bool select_translated_message_locale(ApplicationUiLanguage language) {
  const char *prefix = message_locale_prefix(language);
  if (prefix != nullptr) {
    const std::string utf8_locale = std::string(prefix) + ".UTF-8";
    const std::string compact_utf8_locale = std::string(prefix) + ".utf8";
    if (std::setlocale(LC_MESSAGES, utf8_locale.c_str()) != nullptr ||
        std::setlocale(LC_MESSAGES, compact_utf8_locale.c_str()) != nullptr) {
      return true;
    }
  }

  static constexpr const char *fallback_locales[] = {
      "en_US.UTF-8", "en_US.utf8", "ja_JP.UTF-8", "ja_JP.utf8"};
  for (const char *locale : fallback_locales) {
    if (std::setlocale(LC_MESSAGES, locale) != nullptr) {
      return true;
    }
  }
  return false;
}

static bool path_is_within(const std::filesystem::path &path,
                           const std::filesystem::path &directory) {
  auto path_component = path.begin();
  for (const auto &directory_component : directory) {
    if (path_component == path.end() ||
        *path_component != directory_component) {
      return false;
    }
    ++path_component;
  }
  return true;
}

static bool is_running_from_build_tree() {
  std::error_code error;
  const std::filesystem::path executable =
      std::filesystem::read_symlink("/proc/self/exe", error);
  if (error) {
    return false;
  }

  const std::filesystem::path build_root =
      std::filesystem::weakly_canonical(
          std::filesystem::path(ELDER_TERMS_BUILD_LOCALE_DIR).parent_path(),
          error);
  return !error && path_is_within(executable, build_root);
}

static const char *default_locale_directory() {
  return is_running_from_build_tree() ? ELDER_TERMS_BUILD_LOCALE_DIR
                                      : ELDER_TERMS_LOCALE_DIR;
}

LocalizationInitializationResult
initialize_localization(ApplicationUiLanguage language) {
  LocalizationInitializationResult result;
  if (std::setlocale(LC_ALL, "") == nullptr) {
    result.requested_language_applied = false;
    result.warnings.push_back(
        "Warning: unable to select the system locale for UI text");
  }

  const char *requested_language =
      language == ApplicationUiLanguage::system
          ? nullptr
          : application_ui_language_to_string(language);
  if (requested_language != nullptr &&
      !g_setenv(language_environment, requested_language, TRUE)) {
    result.requested_language_applied = false;
    result.warnings.push_back(
        "Warning: unable to apply the configured UI language");
  }

  if (language != ApplicationUiLanguage::system &&
      language != ApplicationUiLanguage::english &&
      is_c_message_locale(std::setlocale(LC_MESSAGES, nullptr)) &&
      !select_translated_message_locale(language)) {
    result.requested_language_applied = false;
    result.warnings.push_back(
        "Warning: translated UI text requires an installed non-C UTF-8 "
        "locale");
  }

  const char *override_directory =
      g_getenv(locale_directory_environment);
  const char *locale_directory =
      override_directory != nullptr && override_directory[0] != '\0'
          ? override_directory
          : default_locale_directory();
  bindtextdomain(gettext_package, locale_directory);
  bind_textdomain_codeset(gettext_package, "UTF-8");
  return result;
}

} // namespace elder_terms
