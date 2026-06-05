#pragma once

#include <string>
#include <vector>

#include <elder-terms/settings/settings-store.h>

namespace elder_terms {

/**
 * Returns the transfer base path setting key.
 *
 * @returns Setting key for [transfer] base_path.
 */
SettingKey transfer_base_path_setting_key();

/**
 * Returns transfer setting definitions.
 *
 * @returns Setting definitions for the transfer section.
 */
std::vector<SettingDefinition> transfer_setting_definitions();

/**
 * Reads the configured transfer base path.
 *
 * @param store Source settings store.
 * @returns Configured base path or an empty string when the default should be
 * used.
 */
std::string transfer_base_path(const SettingsStore &store);

} // namespace elder_terms
