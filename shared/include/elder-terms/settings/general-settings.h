#pragma once

#include <string>
#include <vector>

#include <elder-terms/export.h>
#include <elder-terms/settings/settings-store.h>

namespace elder_terms {

/**
 * Returns application-wide setting definitions.
 *
 * @param default_connection_name Connection name used when [general] name is
 * absent or blank.
 * @returns Setting definitions for the general INI section.
 */
ELDER_TERMS_API std::vector<SettingDefinition>
general_setting_definitions(std::string default_connection_name);

/**
 * Returns the setting key for [general] name.
 *
 * @returns Setting key for the configured connection name.
 */
ELDER_TERMS_API SettingKey general_name_setting_key();

/**
 * Returns the setting key for [general] type.
 *
 * @returns Setting key for the selected connection type.
 */
ELDER_TERMS_API SettingKey general_type_setting_key();

/**
 * Returns the effective connection name.
 *
 * @param store Source settings store.
 * @returns Explicit [general] name, or the store's path-derived default when
 * the configured value is absent or blank.
 */
ELDER_TERMS_API std::string
general_connection_name(const SettingsStore &store);

/**
 * Checks whether the loaded general settings select the TELNET backend.
 *
 * @param store Source settings store.
 * @returns True when [general] type is telnet.
 */
ELDER_TERMS_API bool
general_settings_select_telnet_connection(const SettingsStore &store);

/**
 * Checks whether the loaded general settings select the serial backend.
 *
 * @param store Source settings store.
 * @returns True when [general] type is serial.
 */
ELDER_TERMS_API bool
general_settings_select_serial_connection(const SettingsStore &store);

} // namespace elder_terms
