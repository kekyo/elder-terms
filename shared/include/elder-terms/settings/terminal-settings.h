#pragma once

#include <string>
#include <vector>

#include <elder-terms/key-binding.h>
#include <elder-terms/settings/settings-store.h>

namespace elder_terms {

/**
 * Terminal display settings used to initialize VTE.
 */
struct TerminalDisplaySettings {
  /** Initial terminal columns. */
  glong width;
  /** Initial terminal rows. */
  glong height;
  /** Initial VTE font scale. */
  gdouble zoom;
};

/**
 * Keyboard bindings for terminal display actions.
 */
struct TerminalKeyBindings {
  /** Zoom-in binding, or no value when the action is disabled. */
  std::optional<KeyBinding> zoom_in;
  /** Zoom-out binding, or no value when the action is disabled. */
  std::optional<KeyBinding> zoom_out;
};

/**
 * Builds the default terminal display settings.
 *
 * @param default_zoom VTE's runtime default font scale.
 * @returns Terminal display settings matching the current built-in behavior.
 */
TerminalDisplaySettings default_terminal_display_settings(gdouble default_zoom);

/**
 * Returns the setting key for [terminal] width.
 *
 * @returns Setting key for terminal columns.
 */
SettingKey terminal_width_setting_key();

/**
 * Returns the setting key for [terminal] height.
 *
 * @returns Setting key for terminal rows.
 */
SettingKey terminal_height_setting_key();

/**
 * Returns the setting key for [terminal] zoom.
 *
 * @returns Setting key for terminal font scale.
 */
SettingKey terminal_zoom_setting_key();

/**
 * Returns the setting key for [terminal] auto_close.
 *
 * @returns Setting key for terminal auto-close behavior.
 */
SettingKey terminal_auto_close_setting_key();

/**
 * Returns the setting key for [terminal] zoom_in_key.
 *
 * @returns Setting key for the terminal zoom-in binding.
 */
SettingKey terminal_zoom_in_key_setting_key();

/**
 * Returns the setting key for [terminal] zoom_out_key.
 *
 * @returns Setting key for the terminal zoom-out binding.
 */
SettingKey terminal_zoom_out_key_setting_key();

/**
 * Returns the setting key for [terminal] encoding.
 *
 * @returns Setting key for the terminal wire character encoding.
 */
SettingKey terminal_encoding_setting_key();

/**
 * Returns the setting key for [terminal] backspace_code.
 *
 * @returns Setting key for the Backspace send code.
 */
SettingKey terminal_backspace_code_setting_key();

/**
 * Returns the setting key for [terminal] cursor_key_mode.
 *
 * @returns Setting key for cursor-key sequence handling.
 */
SettingKey terminal_cursor_key_mode_setting_key();

/**
 * Checks whether iconv can convert between one encoding and UTF-8 in both
 * directions.
 *
 * @param encoding Candidate iconv encoding name. Surrounding whitespace is
 * ignored.
 * @param reason Receives a human-readable validation failure reason.
 * @returns True when both conversion directions can be opened.
 */
bool terminal_encoding_name_is_valid(const std::string &encoding,
                                     std::string *reason);

/**
 * Returns supported representative terminal encoding choices.
 *
 * @returns Process-local choices filtered through bidirectional iconv checks.
 */
std::vector<std::string> terminal_encoding_choices();

/**
 * Returns terminal setting definitions.
 *
 * @param terminal_defaults Default terminal display settings.
 * @returns Setting definitions for the terminal INI section.
 */
std::vector<SettingDefinition>
terminal_setting_definitions(TerminalDisplaySettings terminal_defaults);

/**
 * Extracts terminal display settings from a store.
 *
 * @param store Source settings store.
 * @returns Typed terminal display settings.
 */
TerminalDisplaySettings terminal_display_settings(const SettingsStore &store);

/**
 * Extracts the terminal auto-close behavior from a store.
 *
 * @param store Source settings store.
 * @returns True when the app should exit after the active session ends.
 */
bool terminal_auto_close(const SettingsStore &store);

/**
 * Extracts the raw terminal zoom-in binding text from a store.
 *
 * @param store Source settings store.
 * @returns Configured binding text, or the built-in default.
 */
std::string terminal_zoom_in_key(const SettingsStore &store);

/**
 * Extracts the raw terminal zoom-out binding text from a store.
 *
 * @param store Source settings store.
 * @returns Configured binding text, or the built-in default.
 */
std::string terminal_zoom_out_key(const SettingsStore &store);

/**
 * Extracts parsed terminal keyboard bindings from a store.
 *
 * @param store Source settings store containing validated values.
 * @returns Parsed zoom-in and zoom-out bindings.
 */
TerminalKeyBindings terminal_key_bindings(const SettingsStore &store);

/**
 * Checks whether both terminal zoom actions use the same enabled binding.
 *
 * @param store Source settings store containing individually valid values.
 * @returns True when the two enabled bindings conflict.
 */
bool terminal_key_bindings_conflict(const SettingsStore &store);

} // namespace elder_terms
