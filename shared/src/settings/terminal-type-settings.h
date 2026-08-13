#pragma once

#include <string>

#include <elder-terms/settings/settings-store.h>

namespace elder_terms {

std::string resolve_terminal_type_setting(
    const SettingsStore &store, const SettingKey &key,
    const std::string &default_without_background);

} // namespace elder_terms
