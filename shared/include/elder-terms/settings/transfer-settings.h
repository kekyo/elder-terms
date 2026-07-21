#pragma once

#include <string>
#include <vector>

#include <elder-terms/export.h>
#include <elder-terms/settings/settings-store.h>

namespace elder_terms {

/**
 * Returns the transfer base path setting key.
 *
 * @returns Setting key for [transfer] base_path.
 */
ELDER_TERMS_API SettingKey transfer_base_path_setting_key();

/**
 * Returns the ZMODEM auto-start setting key.
 *
 * @returns Setting key for [transfer] zmodem_autostart.
 */
ELDER_TERMS_API SettingKey transfer_zmodem_autostart_setting_key();

/**
 * Returns transfer setting definitions.
 *
 * @returns Setting definitions for the transfer section.
 */
ELDER_TERMS_API std::vector<SettingDefinition>
transfer_setting_definitions();

/**
 * Reads the configured transfer base path.
 *
 * @param store Source settings store.
 * @returns Configured base path or an empty string when the default should be
 * used.
 */
ELDER_TERMS_API std::string transfer_base_path(const SettingsStore &store);

/**
 * Reads the effective ZMODEM auto-start setting.
 *
 * @param store Source settings store.
 * @returns Explicit configured value, or true only for Serial connections when
 * [transfer] zmodem_autostart is omitted.
 */
ELDER_TERMS_API bool
transfer_zmodem_autostart(const SettingsStore &store);

} // namespace elder_terms
