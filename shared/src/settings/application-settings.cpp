#include <elder-terms/settings/application-settings.h>

#include <string>

#include <glib.h>

namespace elder_terms {

static constexpr char general_section[] = "general";
static constexpr char ui_language_key[] = "ui_language";
static constexpr char startup_mode_key[] = "startup_mode";
static constexpr char open_application_key[] = "open_application";
static constexpr char window_mode[] = "window";
static constexpr char tray_mode[] = "tray";
static constexpr char window_and_tray_mode[] = "window_and_tray";
static constexpr char system_language[] = "system";
static constexpr char english_language[] = "en";
static constexpr char japanese_language[] = "ja";
static constexpr char default_open_application[] = "ctrl+alt+t";

static ApplicationUiLanguage parse_ui_language(const char *value) {
  if (value != nullptr && std::string(value) == english_language) {
    return ApplicationUiLanguage::english;
  }
  if (value != nullptr && std::string(value) == japanese_language) {
    return ApplicationUiLanguage::japanese;
  }
  return ApplicationUiLanguage::system;
}

static bool validate_ui_language(const SettingValue &value,
                                 std::string *reason) {
  const auto *text = std::get_if<std::string>(&value);
  if (text == nullptr ||
      (*text != system_language && *text != english_language &&
       *text != japanese_language)) {
    *reason = "must be system, en, or ja";
    return false;
  }
  return true;
}

static bool validate_startup_mode(const SettingValue &value,
                                  std::string *reason) {
  const auto *text = std::get_if<std::string>(&value);
  if (text == nullptr ||
      (*text != window_mode && *text != tray_mode &&
       *text != window_and_tray_mode)) {
    *reason = "must be window, tray, or window_and_tray";
    return false;
  }
  return true;
}

static bool validate_application_hotkey(const SettingValue &value,
                                        std::string *reason) {
  const auto *text = std::get_if<std::string>(&value);
  if (text == nullptr) {
    *reason = "must be a string";
    return false;
  }
  return application_hotkey_text_is_valid(*text, reason);
}

SettingKey application_startup_mode_setting_key() {
  return make_setting_key(general_section, startup_mode_key);
}

SettingKey application_ui_language_setting_key() {
  return make_setting_key(general_section, ui_language_key);
}

SettingKey application_open_hotkey_setting_key() {
  return make_setting_key(general_section, open_application_key);
}

std::vector<SettingDefinition> application_setting_definitions() {
  return {
      {
          .key = application_ui_language_setting_key(),
          .default_value = SettingValue{std::string(system_language)},
          .validate = validate_ui_language,
      },
      {
          .key = application_startup_mode_setting_key(),
          .default_value = SettingValue{std::string(window_mode)},
          .validate = validate_startup_mode,
      },
      {
          .key = application_open_hotkey_setting_key(),
          .default_value =
              SettingValue{std::string(default_open_application)},
          .validate = validate_application_hotkey,
      },
  };
}

const char *
application_ui_language_to_string(ApplicationUiLanguage language) {
  if (language == ApplicationUiLanguage::english) {
    return english_language;
  }
  if (language == ApplicationUiLanguage::japanese) {
    return japanese_language;
  }
  return system_language;
}

ApplicationUiLanguage application_ui_language(const SettingsStore &store) {
  const std::string value = setting_string_value_or_default(
      store, application_ui_language_setting_key(), system_language);
  return parse_ui_language(value.c_str());
}

ApplicationUiLanguage load_application_ui_language_preference(
    const std::filesystem::path &global_config_path) {
  GKeyFile *key_file = g_key_file_new();
  GError *error = nullptr;
  if (!g_key_file_load_from_file(key_file, global_config_path.c_str(),
                                 G_KEY_FILE_NONE, &error)) {
    g_clear_error(&error);
    g_key_file_unref(key_file);
    return ApplicationUiLanguage::system;
  }

  gchar *value = g_key_file_get_string(
      key_file, general_section, ui_language_key, &error);
  if (error != nullptr) {
    g_clear_error(&error);
    g_key_file_unref(key_file);
    return ApplicationUiLanguage::system;
  }
  const ApplicationUiLanguage language = parse_ui_language(value);
  g_free(value);
  g_key_file_unref(key_file);
  return language;
}

const char *startup_mode_to_string(StartupMode mode) {
  if (mode == StartupMode::tray) {
    return tray_mode;
  }
  if (mode == StartupMode::window_and_tray) {
    return window_and_tray_mode;
  }
  return window_mode;
}

StartupMode application_startup_mode(const SettingsStore &store) {
  const std::string value = setting_string_value_or_default(
      store, application_startup_mode_setting_key(), window_mode);
  if (value == tray_mode) {
    return StartupMode::tray;
  }
  if (value == window_and_tray_mode) {
    return StartupMode::window_and_tray;
  }
  return StartupMode::window;
}

std::string application_open_hotkey_text(const SettingsStore &store) {
  return setting_string_value_or_default(
      store, application_open_hotkey_setting_key(),
      default_open_application);
}

std::optional<KeyBinding>
application_open_hotkey(const SettingsStore &store) {
  return parse_key_binding(application_open_hotkey_text(store)).binding;
}

bool application_hotkey_text_is_valid(const std::string &text,
                                      std::string *reason) {
  return global_hotkey_text_is_valid(text, reason);
}

} // namespace elder_terms
