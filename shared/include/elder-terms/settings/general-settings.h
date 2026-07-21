#pragma once

#include <string>
#include <vector>

#include <elder-terms/export.h>
#include <elder-terms/settings/settings-store.h>

namespace elder_terms {

/**
 * Returns application-wide setting definitions.
 *
 * @returns Setting definitions for the general INI section.
 */
ELDER_TERMS_API std::vector<SettingDefinition> general_setting_definitions();

/**
 * Returns the setting key for [general] type.
 *
 * @returns Setting key for the selected connection type.
 */
ELDER_TERMS_API SettingKey general_type_setting_key();

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
