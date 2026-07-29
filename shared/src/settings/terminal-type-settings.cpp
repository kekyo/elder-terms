#include "terminal-type-settings.h"

#include <elder-terms/settings/general-settings.h>

namespace elder_terms {

std::string resolve_terminal_type_setting(
    const SettingsStore &store, const SettingKey &key,
    const std::string &default_without_background) {
  if (setting_has_configured_value(store, key)) {
    return setting_string_value_or_default(
        store, key, default_without_background);
  }

  // VTE only honors the configured background for the xterm terminal type.
  return general_color_settings(store).background.has_value()
             ? std::string("xterm")
             : default_without_background;
}

} // namespace elder_terms
