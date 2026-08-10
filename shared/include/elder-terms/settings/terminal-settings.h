#pragma once

#include <optional>
#include <string>
#include <vector>

#include <elder-terms/export.h>
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
  /** Maximum normal-screen scrollback history in lines. */
  glong scrollback_lines;
  /** Initial VTE font scale. */
  gdouble zoom;
};

/**
 * Ordered terminal font families.
 *
 * The fallback family is used after the primary family when the primary font
 * does not contain a requested glyph. Neither value contains a font size.
 */
struct TerminalFontFamilies {
  /** Primary font family, or no value to retain VTE's default family. */
  std::optional<std::string> primary_family;
  /** Secondary fallback family, or no value to use normal system fallback. */
  std::optional<std::string> fallback_family;
};

/**
 * Keyboard bindings for terminal display actions.
 */
struct TerminalKeyBindings {
  /** Zoom-in binding, or no value when the action is disabled. */
  std::optional<KeyBinding> zoom_in;
  /** Zoom-out binding, or no value when the action is disabled. */
  std::optional<KeyBinding> zoom_out;
  /** BREAK binding, or no value when the action is disabled. */
  std::optional<KeyBinding> send_break;
};

/**
 * Builds the default terminal display settings.
 *
 * @param default_zoom VTE's runtime default font scale.
 * @returns Terminal display settings matching the current built-in behavior.
 */
ELDER_TERMS_API TerminalDisplaySettings
default_terminal_display_settings(gdouble default_zoom);

/**
 * Returns the setting key for [terminal] width.
 *
 * @returns Setting key for terminal columns.
 */
ELDER_TERMS_API SettingKey terminal_width_setting_key();

/**
 * Returns the setting key for [terminal] height.
 *
 * @returns Setting key for terminal rows.
 */
ELDER_TERMS_API SettingKey terminal_height_setting_key();

/**
 * Returns the setting key for [terminal] scrollback_lines.
 *
 * @returns Setting key for the VTE scrollback buffer size.
 */
ELDER_TERMS_API SettingKey terminal_scrollback_lines_setting_key();

/**
 * Returns the setting key for [terminal] zoom.
 *
 * @returns Setting key for terminal font scale.
 */
ELDER_TERMS_API SettingKey terminal_zoom_setting_key();

/**
 * Returns the setting key for [terminal] font_primary_family.
 *
 * @returns Setting key for the primary terminal font family.
 */
ELDER_TERMS_API SettingKey terminal_font_primary_family_setting_key();

/**
 * Returns the setting key for [terminal] font_fallback_family.
 *
 * @returns Setting key for the secondary terminal font family.
 */
ELDER_TERMS_API SettingKey terminal_font_fallback_family_setting_key();

/**
 * Returns the setting key for [terminal] auto_close.
 *
 * @returns Setting key for terminal auto-close behavior.
 */
ELDER_TERMS_API SettingKey terminal_auto_close_setting_key();

/**
 * Returns the setting key for [terminal] zoom_in_key.
 *
 * @returns Setting key for the terminal zoom-in binding.
 */
ELDER_TERMS_API SettingKey terminal_zoom_in_key_setting_key();

/**
 * Returns the setting key for [terminal] zoom_out_key.
 *
 * @returns Setting key for the terminal zoom-out binding.
 */
ELDER_TERMS_API SettingKey terminal_zoom_out_key_setting_key();

/**
 * Returns the setting key for [terminal] send_break_key.
 *
 * @returns Setting key for the terminal BREAK binding.
 */
ELDER_TERMS_API SettingKey terminal_send_break_key_setting_key();

/**
 * Returns the setting key for [terminal] encoding.
 *
 * @returns Setting key for the terminal wire character encoding.
 */
ELDER_TERMS_API SettingKey terminal_encoding_setting_key();

/**
 * Returns the setting key for [terminal] backspace_code.
 *
 * @returns Setting key for the Backspace send code.
 */
ELDER_TERMS_API SettingKey terminal_backspace_code_setting_key();

/**
 * Returns the setting key for [terminal] cursor_key_mode.
 *
 * @returns Setting key for cursor-key sequence handling.
 */
ELDER_TERMS_API SettingKey terminal_cursor_key_mode_setting_key();

/**
 * Returns the setting key for [terminal] return_code.
 *
 * @returns Setting key for the Enter/Return send code.
 */
ELDER_TERMS_API SettingKey terminal_return_code_setting_key();

/**
 * Checks whether iconv can convert between one encoding and UTF-8 in both
 * directions.
 *
 * @param encoding Candidate iconv encoding name. Surrounding whitespace is
 * ignored.
 * @param reason Receives a human-readable validation failure reason.
 * @returns True when both conversion directions can be opened.
 */
ELDER_TERMS_API bool terminal_encoding_name_is_valid(
    const std::string &encoding, std::string *reason);

/**
 * Returns supported representative terminal encoding choices.
 *
 * @returns Process-local choices filtered through bidirectional iconv checks.
 */
ELDER_TERMS_API std::vector<std::string> terminal_encoding_choices();

/**
 * Returns terminal setting definitions.
 *
 * @param terminal_defaults Default terminal display settings.
 * @returns Setting definitions for the terminal INI section.
 */
ELDER_TERMS_API std::vector<SettingDefinition>
terminal_setting_definitions(TerminalDisplaySettings terminal_defaults);

/**
 * Extracts terminal display settings from a store.
 *
 * @param store Source settings store.
 * @returns Typed terminal display settings.
 */
ELDER_TERMS_API TerminalDisplaySettings
terminal_display_settings(const SettingsStore &store);

/**
 * Extracts the resolved ordered terminal font families from a store.
 *
 * @param store Source settings store.
 * @returns Primary and secondary families without a font size.
 */
ELDER_TERMS_API TerminalFontFamilies
terminal_font_families(const SettingsStore &store);

/**
 * Extracts the terminal auto-close behavior from a store.
 *
 * @param store Source settings store.
 * @returns True when the app should exit after the active session ends.
 */
ELDER_TERMS_API bool terminal_auto_close(const SettingsStore &store);

/**
 * Extracts the raw terminal zoom-in binding text from a store.
 *
 * @param store Source settings store.
 * @returns Configured binding text, or the built-in default.
 */
ELDER_TERMS_API std::string
terminal_zoom_in_key(const SettingsStore &store);

/**
 * Extracts the raw terminal zoom-out binding text from a store.
 *
 * @param store Source settings store.
 * @returns Configured binding text, or the built-in default.
 */
ELDER_TERMS_API std::string
terminal_zoom_out_key(const SettingsStore &store);

/**
 * Extracts the raw terminal BREAK binding text from a store.
 *
 * @param store Source settings store.
 * @returns Configured binding text, or an empty string when disabled.
 */
ELDER_TERMS_API std::string
terminal_send_break_key(const SettingsStore &store);

/**
 * Extracts parsed terminal keyboard bindings from a store.
 *
 * @param store Source settings store containing validated values.
 * @returns Parsed zoom-in and zoom-out bindings.
 */
ELDER_TERMS_API TerminalKeyBindings
terminal_key_bindings(const SettingsStore &store);

/**
 * Checks whether terminal actions share an enabled binding.
 *
 * @param store Source settings store containing individually valid values.
 * @returns True when any two enabled bindings conflict.
 */
ELDER_TERMS_API bool
terminal_key_bindings_conflict(const SettingsStore &store);

} // namespace elder_terms
