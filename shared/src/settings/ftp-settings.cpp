#include <elder-terms/settings/ftp-settings.h>

#include <algorithm>
#include <cctype>
#include <utility>

namespace elder_terms {

static constexpr gint64 default_ftp_port = 21;
static constexpr char default_ftp_remote_directory[] = ".";
static constexpr char ftp_section[] = "ftp";
static constexpr char ftp_address_key[] = "address";
static constexpr char ftp_port_key[] = "port";
static constexpr char ftp_username_key[] = "username";
static constexpr char ftp_data_connection_mode_key[] =
    "data_connection_mode";
static constexpr char ftp_local_directory_key[] = "local_directory";
static constexpr char ftp_remote_directory_key[] = "remote_directory";
static constexpr char passive_data_connection_mode[] = "passive";
static constexpr char active_data_connection_mode[] = "active";

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

static bool validate_data_connection_mode(const SettingValue &value,
                                          std::string *reason) {
  const auto *text = std::get_if<std::string>(&value);
  if (text == nullptr ||
      (*text != passive_data_connection_mode &&
       *text != active_data_connection_mode)) {
    *reason = "must be passive or active";
    return false;
  }
  return true;
}

static bool validate_tls_mode(const SettingValue &value, std::string *reason) {
  const auto &text = std::get<std::string>(value);
  if (text == "none" || text == "explicit" || text == "implicit") return true;
  *reason = "must be none, explicit, or implicit";
  return false;
}

static bool validate_ca_file(const SettingValue &value, std::string *reason) {
  const auto &text = std::get<std::string>(value);
  if (text.empty() || (text.front() == '/' && text.find('\0') == std::string::npos)) return true;
  *reason = "must be an absolute PEM CA bundle path or empty";
  return false;
}

static SettingKey ftp_key(const char *name) {
  return make_setting_key(ftp_section, name);
}

SettingKey ftp_tls_mode_setting_key() { return ftp_key("tls_mode"); }
SettingKey ftp_ca_file_setting_key() { return ftp_key("ca_file"); }

const char *ftp_tls_mode_to_string(FtpTlsMode mode) {
  switch (mode) {
  case FtpTlsMode::none: return "none";
  case FtpTlsMode::explicit_tls: return "explicit";
  case FtpTlsMode::implicit_tls: return "implicit";
  }
  return "invalid";
}

SettingKey ftp_address_setting_key() {
  return ftp_key(ftp_address_key);
}

SettingKey ftp_port_setting_key() {
  return ftp_key(ftp_port_key);
}

SettingKey ftp_username_setting_key() {
  return ftp_key(ftp_username_key);
}

SettingKey ftp_data_connection_mode_setting_key() {
  return ftp_key(ftp_data_connection_mode_key);
}

SettingKey ftp_local_directory_setting_key() {
  return ftp_key(ftp_local_directory_key);
}

SettingKey ftp_remote_directory_setting_key() {
  return ftp_key(ftp_remote_directory_key);
}

const char *ftp_data_connection_mode_to_string(
    FtpDataConnectionMode mode) {
  return mode == FtpDataConnectionMode::active
             ? active_data_connection_mode
             : passive_data_connection_mode;
}

std::vector<SettingDefinition> ftp_connection_setting_definitions() {
  return {
      {.key = ftp_tls_mode_setting_key(), .default_value = std::string("none"),
       .validate = validate_tls_mode, .retain_invalid = true},
      {.key = ftp_ca_file_setting_key(), .default_value = std::string(),
       .validate = validate_ca_file, .retain_invalid = true},
      {.key = ftp_key("tls_min_version"), .default_value = std::string("1.2"), .retain_invalid = true},
      {.key = ftp_key("tls_max_version"), .default_value = std::string("default"), .retain_invalid = true},
      {.key = ftp_key("tls_auth_order"), .default_value = std::string("tls"), .retain_invalid = true},
      {.key = ftp_key("tls_compatibility"), .default_value = std::string("standard"), .retain_invalid = true},
      {.key = ftp_key("tls_cipher_list"), .default_value = std::string(), .retain_invalid = true},
      {.key = ftp_key("tls13_cipher_list"), .default_value = std::string(), .retain_invalid = true},
      {.key = ftp_key("certificate_error_action"), .default_value = std::string("reject"), .retain_invalid = true},
      {
          .key = ftp_address_setting_key(),
          .default_value = SettingValue{std::string()},
          .validate = nullptr,
      },
      {
          .key = ftp_port_setting_key(),
          .default_value = SettingValue{default_ftp_port},
          .validate = validate_port,
      },
      {
          .key = ftp_username_setting_key(),
          .default_value = SettingValue{std::string()},
          .validate = nullptr,
      },
      {
          .key = ftp_data_connection_mode_setting_key(),
          .default_value =
              SettingValue{std::string(passive_data_connection_mode)},
          .validate = validate_data_connection_mode,
      },
      {
          .key = ftp_local_directory_setting_key(),
          .default_value = SettingValue{std::string()},
          .validate = nullptr,
      },
      {
          .key = ftp_remote_directory_setting_key(),
          .default_value =
              SettingValue{std::string(default_ftp_remote_directory)},
          .validate = nullptr,
      },
  };
}

FtpConnectionSettings ftp_connection_settings(const SettingsStore &store) {
  std::string address = setting_string_value_or_default(
      store, ftp_address_setting_key(), std::string());
  if (string_is_blank(address)) {
    address.clear();
  }
  std::string username = setting_string_value_or_default(
      store, ftp_username_setting_key(), std::string());
  if (string_is_blank(username)) {
    username.clear();
  }
  const std::string data_connection_mode = setting_string_value_or_default(
      store, ftp_data_connection_mode_setting_key(),
      passive_data_connection_mode);

  const auto tls = setting_string_value_or_default(store, ftp_tls_mode_setting_key(), "none");
  std::vector<std::string> errors;
  for (const auto &entry : store.entries) {
    const auto &key = entry.definition.key;
    if (key.section == "ftp" && key.name != "tls_mode" &&
        (key.name.starts_with("tls_") || key.name == "tls13_cipher_list" || key.name == "certificate_error_action") &&
        entry.value != entry.definition.default_value) {
      errors.push_back("[ftp] " + key.name + ": non-default values are not implemented yet");
    }
    if (entry.definition.key.section == "ftp" && !entry.validation_error.empty()) {
      const auto source = setting_value_source(store, entry.definition.key);
      errors.push_back("[ftp] " + entry.definition.key.name + " (" +
          (source == SettingValueSource::global ? "global" : "connection") + "): " + entry.validation_error);
    }
  }
  return {
      .address = std::move(address),
      .port = setting_integer_value_or_default(
          store, ftp_port_setting_key(), default_ftp_port),
      .username = std::move(username),
      .data_connection_mode =
          data_connection_mode == active_data_connection_mode
              ? FtpDataConnectionMode::active
              : FtpDataConnectionMode::passive,
      .local_directory = setting_string_value_or_default(
          store, ftp_local_directory_setting_key(), std::string()),
      .remote_directory = setting_string_value_or_default(
          store, ftp_remote_directory_setting_key(),
          default_ftp_remote_directory),
      .tls_mode = tls == "explicit" ? FtpTlsMode::explicit_tls :
          tls == "implicit" ? FtpTlsMode::implicit_tls : FtpTlsMode::none,
      .ca_file = setting_string_value_or_default(store, ftp_ca_file_setting_key(), ""),
      .validation_errors = std::move(errors),
  };
}

void append_ftp_connection_warnings(const SettingsStore &store,
                                    std::vector<std::string> *warnings) {
  const std::string address = setting_string_value_or_default(
      store, ftp_address_setting_key(), std::string());
  if (string_is_blank(address)) {
    warnings->push_back(
        "Warning: missing required configuration value [ftp] address; "
        "FTP connection will not open");
  }
}

} // namespace elder_terms
