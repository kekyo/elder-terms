#include <elder-terms/settings/telnet-settings.h>

#include <algorithm>
#include <cctype>
#include <utility>

namespace elder_terms {

static constexpr gint64 default_telnet_port = 23;
static constexpr char telnet_section[] = "telnet";
static constexpr char telnet_address_key[] = "address";
static constexpr char telnet_port_key[] = "port";

static bool string_is_blank(const std::string &value) {
  return std::all_of(value.begin(), value.end(), [](unsigned char character) {
    return std::isspace(character) != 0;
  });
}

static bool validate_port(const SettingValue &value, std::string *reason) {
  const auto *integer = std::get_if<gint64>(&value);
  if (integer == nullptr || *integer <= 0 || *integer > 65535) {
    *reason = "must be an integer between 1 and 65535";
    return false;
  }
  return true;
}

static SettingKey telnet_key(const char *name) {
  return make_setting_key(telnet_section, name);
}

SettingKey telnet_address_setting_key() {
  return telnet_key(telnet_address_key);
}

SettingKey telnet_port_setting_key() {
  return telnet_key(telnet_port_key);
}

std::vector<SettingDefinition> telnet_connection_setting_definitions() {
  return {
      {
          .key = telnet_address_setting_key(),
          .default_value = SettingValue{std::string()},
          .validate = nullptr,
      },
      {
          .key = telnet_port_setting_key(),
          .default_value = SettingValue{default_telnet_port},
          .validate = validate_port,
      },
  };
}

TelnetConnectionSettings telnet_connection_settings(const SettingsStore &store) {
  std::string address = setting_string_value_or_default(
      store, telnet_address_setting_key(), std::string());
  if (string_is_blank(address)) {
    address.clear();
  }

  return {
      .address = std::move(address),
      .port = setting_integer_value_or_default(
          store, telnet_port_setting_key(), default_telnet_port),
  };
}

void append_telnet_connection_warnings(const SettingsStore &store,
                                       std::vector<std::string> *warnings) {
  const std::string address = setting_string_value_or_default(
      store, telnet_address_setting_key(), std::string());
  if (string_is_blank(address)) {
    warnings->push_back(
        "Warning: missing required configuration value [telnet] address; "
        "TELNET session will not connect");
  }
}

} // namespace elder_terms
