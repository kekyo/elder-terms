#pragma once

#include <vector>

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

} // namespace elder_terms
