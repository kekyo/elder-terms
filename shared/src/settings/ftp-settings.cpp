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

static std::optional<FtpTlsVersion> parse_tls_version(const std::string &text) {
  if (text == "1.0") return FtpTlsVersion::tls10;
  if (text == "1.1") return FtpTlsVersion::tls11;
  if (text == "1.2") return FtpTlsVersion::tls12;
  if (text == "1.3") return FtpTlsVersion::tls13;
  return std::nullopt;
}

static bool validate_tls_min_version(const SettingValue &value, std::string *reason) {
  if (parse_tls_version(std::get<std::string>(value))) return true;
  *reason = "must be 1.0, 1.1, 1.2, or 1.3";
  return false;
}

static bool validate_tls_max_version(const SettingValue &value, std::string *reason) {
  if (std::get<std::string>(value) == "default" || parse_tls_version(std::get<std::string>(value))) return true;
  *reason = "must be default, 1.0, 1.1, 1.2, or 1.3";
  return false;
}

static bool validate_tls_auth_order(const SettingValue &value, std::string *reason) {
  const auto &text = std::get<std::string>(value);
  if (text == "tls" || text == "ssl" || text == "default") return true;
  *reason = "must be tls, ssl, or default";
  return false;
}

static bool validate_tls_compatibility(const SettingValue &value, std::string *reason) {
  const auto &text = std::get<std::string>(value);
  if (text == "standard" || text == "openssl_legacy") return true;
  *reason = "must be standard or openssl_legacy";
  return false;
}

static bool validate_tls_cipher_list(const SettingValue &value, std::string *reason) {
  auto text = std::get<std::string>(value);
  if (std::any_of(text.begin(), text.end(), [](unsigned char c) { return c < 32 || c == 127; })) {
    *reason = "must not contain control characters";
    return false;
  }
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::toupper(c); });
  if (text.find("@SECLEVEL") != std::string::npos) {
    *reason = "security levels must be selected with tls_compatibility";
    return false;
  }
  return true;
}

static bool validate_tls13_cipher_list(const SettingValue &value, std::string *reason) {
  if (!validate_tls_cipher_list(value, reason)) return false;
  const auto &text = std::get<std::string>(value);
  // OpenSSL 3.5 can negotiate these integrity-only suites at security level 0.
  // FTPS requires encryption even when legacy compatibility is explicit.
  if (text.find("TLS_SHA256_SHA256") != std::string::npos ||
      text.find("TLS_SHA384_SHA384") != std::string::npos) {
    *reason = "TLS 1.3 cipher suites must encrypt data";
    return false;
  }
  return true;
}

static bool validate_certificate_error_action(const SettingValue &value, std::string *reason) {
  const auto &text = std::get<std::string>(value);
  if (text == "reject" || text == "prompt") return true;
  *reason = "must be reject or prompt";
  return false;
}

static SettingKey ftp_key(const char *name) {
  return make_setting_key(ftp_section, name);
}

SettingKey ftp_tls_mode_setting_key() { return ftp_key("tls_mode"); }
SettingKey ftp_tls_min_version_setting_key() { return ftp_key("tls_min_version"); }
SettingKey ftp_tls_max_version_setting_key() { return ftp_key("tls_max_version"); }
SettingKey ftp_tls_auth_order_setting_key() { return ftp_key("tls_auth_order"); }
SettingKey ftp_tls_compatibility_setting_key() { return ftp_key("tls_compatibility"); }
SettingKey ftp_tls_cipher_list_setting_key() { return ftp_key("tls_cipher_list"); }
SettingKey ftp_tls13_cipher_list_setting_key() { return ftp_key("tls13_cipher_list"); }
SettingKey ftp_certificate_error_action_setting_key() { return ftp_key("certificate_error_action"); }
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
      {.key = ftp_key("tls_min_version"), .default_value = std::string("1.2"), .validate = validate_tls_min_version, .retain_invalid = true},
      {.key = ftp_key("tls_max_version"), .default_value = std::string("default"), .validate = validate_tls_max_version, .retain_invalid = true},
      {.key = ftp_key("tls_auth_order"), .default_value = std::string("tls"), .validate = validate_tls_auth_order, .retain_invalid = true},
      {.key = ftp_key("tls_compatibility"), .default_value = std::string("standard"), .validate = validate_tls_compatibility, .retain_invalid = true},
      {.key = ftp_key("tls_cipher_list"), .default_value = std::string(), .validate = validate_tls_cipher_list, .retain_invalid = true},
      {.key = ftp_key("tls13_cipher_list"), .default_value = std::string(), .validate = validate_tls13_cipher_list, .retain_invalid = true},
      {.key = ftp_key("certificate_error_action"), .default_value = std::string("reject"), .validate = validate_certificate_error_action, .retain_invalid = true},
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
    if (entry.definition.key.section == "ftp" && !entry.validation_error.empty()) {
      const auto source = setting_value_source(store, entry.definition.key);
      errors.push_back("[ftp] " + entry.definition.key.name + " (" +
          (source == SettingValueSource::global ? "global" : "connection") + "): " + entry.validation_error);
    }
  }
  const auto minimum = parse_tls_version(setting_string_value_or_default(store, ftp_tls_min_version_setting_key(), "1.2"));
  const auto maximum = parse_tls_version(setting_string_value_or_default(store, ftp_tls_max_version_setting_key(), "default"));
  if (minimum && maximum && *minimum > *maximum)
    errors.push_back("[ftp] tls_min_version must not exceed tls_max_version");
  const auto auth = setting_string_value_or_default(store, ftp_tls_auth_order_setting_key(), "tls");
  const auto compatibility = setting_string_value_or_default(store, ftp_tls_compatibility_setting_key(), "standard");
  return {
      .address = std::move(address),
      .port = tls == "implicit" && setting_value_source(store, ftp_port_setting_key()) == SettingValueSource::built_in
          ? 990 : setting_integer_value_or_default(store, ftp_port_setting_key(), default_ftp_port),
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
      .tls_min_version = minimum.value_or(FtpTlsVersion::tls12),
      .tls_max_version = maximum,
      .tls_auth_order = auth == "ssl" ? FtpTlsAuthOrder::ssl : auth == "default" ? FtpTlsAuthOrder::automatic : FtpTlsAuthOrder::tls,
      .tls_compatibility = compatibility == "openssl_legacy" ? FtpTlsCompatibility::openssl_legacy : FtpTlsCompatibility::standard,
      .tls_cipher_list = setting_string_value_or_default(store, ftp_tls_cipher_list_setting_key(), ""),
      .tls13_cipher_list = setting_string_value_or_default(store, ftp_tls13_cipher_list_setting_key(), ""),
      .certificate_error_action = setting_string_value_or_default(store, ftp_certificate_error_action_setting_key(), "reject") == "prompt"
          ? FtpCertificateErrorAction::prompt : FtpCertificateErrorAction::reject,
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
