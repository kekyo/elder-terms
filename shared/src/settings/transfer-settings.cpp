#include <elder-terms/settings/transfer-settings.h>

#include <elder-terms/settings/general-settings.h>

namespace elder_terms {

static constexpr char transfer_section[] = "transfer";
static constexpr char transfer_base_path_key[] = "base_path";
static constexpr char transfer_zmodem_autostart_key[] = "zmodem_autostart";

static SettingKey transfer_key(const char *name) {
  return make_setting_key(transfer_section, name);
}

SettingKey transfer_base_path_setting_key() {
  return transfer_key(transfer_base_path_key);
}

SettingKey transfer_zmodem_autostart_setting_key() {
  return transfer_key(transfer_zmodem_autostart_key);
}

std::vector<SettingDefinition> transfer_setting_definitions() {
  return {
      {
          .key = transfer_base_path_setting_key(),
          .default_value = SettingValue{std::string()},
      },
      {
          .key = transfer_zmodem_autostart_setting_key(),
          .default_value = SettingValue{false},
          .validate = nullptr,
          .save_when_loaded = true,
      },
  };
}

std::string transfer_base_path(const SettingsStore &store) {
  return setting_string_value_or_default(
      store, transfer_base_path_setting_key(), std::string());
}

bool transfer_zmodem_autostart(const SettingsStore &store) {
  const bool configured = setting_boolean_value_or_default(
      store, transfer_zmodem_autostart_setting_key(), false);
  if (setting_has_explicit_value(store,
                                 transfer_zmodem_autostart_setting_key())) {
    return configured;
  }
  if (configured) {
    return true;
  }
  return general_settings_select_serial_connection(store);
}

} // namespace elder_terms
