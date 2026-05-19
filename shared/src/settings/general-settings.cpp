#include <elder-terms/settings/general-settings.h>

namespace elder_terms {

static constexpr char general_section[] = "general";
static constexpr char general_type_key[] = "type";
static constexpr char local_connection_type[] = "local";
static constexpr char telnet_connection_type[] = "telnet";
static constexpr char serial_connection_type[] = "serial";

static bool validate_connection_type(const SettingValue &value,
                                     std::string *reason) {
  const auto *text = std::get_if<std::string>(&value);
  if (text == nullptr ||
      (*text != local_connection_type && *text != telnet_connection_type &&
       *text != serial_connection_type)) {
    *reason = "must be local, telnet, or serial";
    return false;
  }
  return true;
}

SettingKey general_type_setting_key() {
  return make_setting_key(general_section, general_type_key);
}

std::vector<SettingDefinition> general_setting_definitions() {
  return {
      {
          .key = general_type_setting_key(),
          .default_value = SettingValue{std::string(local_connection_type)},
          .validate = validate_connection_type,
      },
  };
}

bool general_settings_select_telnet_connection(const SettingsStore &store) {
  return setting_string_value_or_default(store, general_type_setting_key(),
                                         local_connection_type) ==
         telnet_connection_type;
}

bool general_settings_select_serial_connection(const SettingsStore &store) {
  return setting_string_value_or_default(store, general_type_setting_key(),
                                         local_connection_type) ==
         serial_connection_type;
}

} // namespace elder_terms
