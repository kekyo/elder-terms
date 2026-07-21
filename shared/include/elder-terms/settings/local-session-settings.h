#pragma once

#include <vector>

#include <elder-terms/export.h>
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
ELDER_TERMS_API std::vector<SettingDefinition>
local_shell_connection_setting_definitions();

} // namespace elder_terms
