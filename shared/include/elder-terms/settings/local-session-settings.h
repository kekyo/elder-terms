#pragma once

#include <vector>

#include <elder-terms/settings/settings-store.h>

namespace elder_terms {

/**
 * Settings for the local shell connection backend.
 */
struct LocalShellConnectionSettings {};

/**
 * Returns local shell setting definitions.
 *
 * @returns Empty definitions because local shell currently has no INI keys.
 */
std::vector<SettingDefinition> local_shell_connection_setting_definitions();

} // namespace elder_terms
