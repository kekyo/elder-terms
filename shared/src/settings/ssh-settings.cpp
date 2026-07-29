#include <elder-terms/settings/ssh-settings.h>

#include <algorithm>
#include <cctype>
#include <utility>

#include "terminal-type-settings.h"

namespace elder_terms {

static constexpr gint64 default_ssh_port = 22;
static constexpr char default_ssh_terminal_type[] = "xterm-256color";
static constexpr char ssh_section[] = "ssh";
static constexpr char ssh_address_key[] = "address";
static constexpr char ssh_port_key[] = "port";
static constexpr char ssh_username_key[] = "username";
static constexpr char ssh_identity_file_key[] = "identity_file";
static constexpr char ssh_terminal_type_key[] = "terminal_type";

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

static bool validate_terminal_type(const SettingValue &value,
                                   std::string *reason) {
  const auto *text = std::get_if<std::string>(&value);
  if (text == nullptr || string_is_blank(*text)) {
    *reason = "must not be blank";
    return false;
  }
  return true;
}

static SettingKey ssh_key(const char *name) {
  return make_setting_key(ssh_section, name);
}

SettingKey ssh_address_setting_key() {
  return ssh_key(ssh_address_key);
}

SettingKey ssh_port_setting_key() {
  return ssh_key(ssh_port_key);
}

SettingKey ssh_username_setting_key() {
  return ssh_key(ssh_username_key);
}

SettingKey ssh_identity_file_setting_key() {
  return ssh_key(ssh_identity_file_key);
}

SettingKey ssh_terminal_type_setting_key() {
  return ssh_key(ssh_terminal_type_key);
}

std::vector<SettingDefinition> ssh_connection_setting_definitions() {
  return {
      {
          .key = ssh_address_setting_key(),
          .default_value = SettingValue{std::string()},
          .validate = nullptr,
      },
      {
          .key = ssh_port_setting_key(),
          .default_value = SettingValue{default_ssh_port},
          .validate = validate_port,
      },
      {
          .key = ssh_username_setting_key(),
          .default_value = SettingValue{std::string()},
          .validate = nullptr,
      },
      {
          .key = ssh_identity_file_setting_key(),
          .default_value = SettingValue{std::string()},
          .validate = nullptr,
      },
      {
          .key = ssh_terminal_type_setting_key(),
          .default_value =
              SettingValue{std::string(default_ssh_terminal_type)},
          .validate = validate_terminal_type,
      },
  };
}

SshEndpointSettings ssh_endpoint_settings(const SettingsStore &store) {
  std::string address = setting_string_value_or_default(
      store, ssh_address_setting_key(), std::string());
  if (string_is_blank(address)) {
    address.clear();
  }
  std::string username = setting_string_value_or_default(
      store, ssh_username_setting_key(), std::string());
  if (string_is_blank(username)) {
    username.clear();
  }
  std::string identity_file = setting_string_value_or_default(
      store, ssh_identity_file_setting_key(), std::string());
  if (string_is_blank(identity_file)) {
    identity_file.clear();
  }

  return {
      .address = std::move(address),
      .port = setting_integer_value_or_default(
          store, ssh_port_setting_key(), default_ssh_port),
      .username = std::move(username),
      .identity_file = std::move(identity_file),
  };
}

SshConnectionSettings ssh_connection_settings(const SettingsStore &store) {
  return {
      .endpoint = ssh_endpoint_settings(store),
      .terminal_type = resolve_terminal_type_setting(
          store, ssh_terminal_type_setting_key(),
          default_ssh_terminal_type),
  };
}

void append_ssh_connection_warnings(const SettingsStore &store,
                                    std::vector<std::string> *warnings) {
  const std::string address = setting_string_value_or_default(
      store, ssh_address_setting_key(), std::string());
  if (string_is_blank(address)) {
    warnings->push_back(
        "Warning: missing required configuration value [ssh] address; "
        "SSH session will not connect");
  }
}

} // namespace elder_terms
