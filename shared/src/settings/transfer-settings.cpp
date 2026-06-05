#include <elder-terms/settings/transfer-settings.h>

namespace elder_terms {

static constexpr char transfer_section[] = "transfer";
static constexpr char transfer_base_path_key[] = "base_path";

static SettingKey transfer_key(const char *name) {
  return make_setting_key(transfer_section, name);
}

SettingKey transfer_base_path_setting_key() {
  return transfer_key(transfer_base_path_key);
}

std::vector<SettingDefinition> transfer_setting_definitions() {
  return {
      {
          .key = transfer_base_path_setting_key(),
          .default_value = SettingValue{std::string()},
      },
  };
}

std::string transfer_base_path(const SettingsStore &store) {
  return setting_string_value_or_default(
      store, transfer_base_path_setting_key(), std::string());
}

} // namespace elder_terms
