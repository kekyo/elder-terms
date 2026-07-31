#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <elder-terms/export.h>
#include <elder-terms/key-binding.h>
#include <elder-terms/settings/settings-store.h>

namespace elder_terms {

/**
 * Selects the language used for application UI text.
 */
enum class ApplicationUiLanguage {
  /** Follows the process environment configured by the operating system. */
  system,
  /** Uses English source messages. */
  english,
  /** Uses the Japanese message catalog. */
  japanese,
};

/**
 * Selects the launcher presentation used at application startup.
 */
enum class StartupMode {
  /** Shows the launcher without creating a tray item. */
  window,
  /** Creates a tray item without initially showing the launcher. */
  tray,
  /** Creates a tray item and initially shows the launcher. */
  window_and_tray,
};

/**
 * Returns global-only application setting definitions.
 *
 * @returns Setting definitions for launcher startup and activation.
 *
 * @remarks These definitions must only be added to the global settings store.
 */
ELDER_TERMS_API std::vector<SettingDefinition>
application_setting_definitions();

/**
 * Returns the setting key for [general] startup_mode.
 *
 * @returns Setting key for the launcher startup presentation.
 */
ELDER_TERMS_API SettingKey application_startup_mode_setting_key();

/**
 * Returns the setting key for [general] ui_language.
 *
 * @returns Setting key for the application UI language.
 */
ELDER_TERMS_API SettingKey application_ui_language_setting_key();

/**
 * Returns the setting key for [general] open_application.
 *
 * @returns Setting key for the global launcher activation hotkey.
 */
ELDER_TERMS_API SettingKey application_open_hotkey_setting_key();

/**
 * Returns the stable INI value for a startup mode.
 *
 * @param mode Startup mode to serialize.
 * @returns Stable lowercase INI value.
 */
ELDER_TERMS_API const char *startup_mode_to_string(StartupMode mode);

/**
 * Returns the stable INI value for an application UI language.
 *
 * @param language UI language to serialize.
 * @returns `system`, `en`, or `ja`.
 */
ELDER_TERMS_API const char *
application_ui_language_to_string(ApplicationUiLanguage language);

/**
 * Extracts the configured application UI language.
 *
 * @param store Global settings store.
 * @returns Configured language, or the system language by default.
 */
ELDER_TERMS_API ApplicationUiLanguage
application_ui_language(const SettingsStore &store);

/**
 * Reads the application UI language needed before the full settings load.
 *
 * @param global_config_path Global defaults INI path.
 * @returns A supported configured language, or the system language when the
 * file, key, or value cannot be read.
 *
 * @remarks This best-effort read intentionally does not report configuration
 * warnings. The later full settings load remains responsible for diagnostics.
 */
ELDER_TERMS_API ApplicationUiLanguage
load_application_ui_language_preference(
    const std::filesystem::path &global_config_path);

/**
 * Extracts the effective launcher startup mode.
 *
 * @param store Global settings store.
 * @returns Configured mode, or simple window startup by default.
 */
ELDER_TERMS_API StartupMode
application_startup_mode(const SettingsStore &store);

/**
 * Extracts the effective application hotkey text.
 *
 * @param store Global settings store.
 * @returns Configured key binding, the built-in default, or an empty string
 * when explicitly disabled.
 */
ELDER_TERMS_API std::string
application_open_hotkey_text(const SettingsStore &store);

/**
 * Extracts the parsed application activation hotkey.
 *
 * @param store Global settings store.
 * @returns Parsed hotkey, or no value when explicitly disabled.
 */
ELDER_TERMS_API std::optional<KeyBinding>
application_open_hotkey(const SettingsStore &store);

/**
 * Validates application hotkey text.
 *
 * @param text Candidate binding text. An empty value disables the hotkey.
 * @param reason Receives a human-readable reason when validation fails.
 * @returns True for an empty value or a key binding with at least one
 * supported modifier.
 */
ELDER_TERMS_API bool
application_hotkey_text_is_valid(const std::string &text,
                                 std::string *reason);

} // namespace elder_terms
