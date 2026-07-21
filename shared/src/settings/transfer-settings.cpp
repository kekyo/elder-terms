#include <elder-terms/settings/transfer-settings.h>

#include <elder-terms/settings/general-settings.h>

namespace elder_terms {

static constexpr char transfer_section[] = "transfer";
static constexpr char transfer_base_path_key[] = "base_path";
static constexpr char transfer_text_send_bytes_per_second_key[] =
    "text_send_bytes_per_second";
static constexpr char transfer_zmodem_autostart_key[] = "zmodem_autostart";
static constexpr gint64 default_text_send_bytes_per_second = 1024;

static bool validate_text_send_bytes_per_second(const SettingValue &value,
                                                std::string *reason) {
  const auto *integer = std::get_if<gint64>(&value);
  if (integer == nullptr || *integer < 1 || *integer > 8000000) {
    *reason = "must be an integer between 1 and 8000000";
    return false;
  }
  return true;
}

static SettingKey transfer_key(const char *name) {
  return make_setting_key(transfer_section, name);
}

SettingKey transfer_base_path_setting_key() {
  return transfer_key(transfer_base_path_key);
}

SettingKey transfer_text_send_bytes_per_second_setting_key() {
  return transfer_key(transfer_text_send_bytes_per_second_key);
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
          .key = transfer_text_send_bytes_per_second_setting_key(),
          .default_value = SettingValue{default_text_send_bytes_per_second},
          .validate = validate_text_send_bytes_per_second,
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

gint64 transfer_text_send_bytes_per_second(const SettingsStore &store) {
  return setting_integer_value_or_default(
      store, transfer_text_send_bytes_per_second_setting_key(),
      default_text_send_bytes_per_second);
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
