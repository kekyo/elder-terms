#include <elder-terms/settings/ssh-proxy-settings.h>
#include <elder-terms/settings/general-settings.h>
#include <elder-terms/settings/ftp-settings.h>

#include <algorithm>
#include <utility>

#define GETTEXT_PACKAGE "elder-terms"
#include <glib/gi18n-lib.h>

namespace elder_terms {

static bool validate_text(const SettingValue &value, std::string *reason) {
  const auto &text = std::get<std::string>(value);
  if (g_utf8_validate(text.data(), static_cast<gssize>(text.size()), nullptr) &&
      std::none_of(text.begin(), text.end(), [](unsigned char c) { return c < 32 || c == 127; })) return true;
  *reason = "must contain valid UTF-8 without control characters";
  return false;
}

static bool validate_address(const SettingValue &value, std::string *reason) {
  if (!validate_text(value, reason)) return false;
  const auto &text = std::get<std::string>(value);
  if (text.find_first_of(" /\\@?#[]") == std::string::npos) return true;
  *reason = "must be a hostname or IP address without a scheme, path, or credentials";
  return false;
}

static bool validate_port(const SettingValue &value, std::string *reason) {
  const auto port = std::get<gint64>(value);
  if (port >= 1 && port <= 65535) return true;
  *reason = "must be an integer between 1 and 65535";
  return false;
}

SettingKey ssh_proxy_setting_key(std::string name) {
  return make_setting_key("ssh_proxy", std::move(name));
}

std::vector<SettingDefinition> ssh_proxy_setting_definitions() {
  return {
      {.key = ssh_proxy_setting_key("enabled"), .default_value = false, .retain_invalid = true},
      {.key = ssh_proxy_setting_key("address"), .default_value = std::string(), .validate = validate_address, .retain_invalid = true},
      {.key = ssh_proxy_setting_key("port"), .default_value = gint64{22}, .validate = validate_port, .retain_invalid = true},
      {.key = ssh_proxy_setting_key("username"), .default_value = std::string(), .validate = validate_text, .retain_invalid = true},
      {.key = ssh_proxy_setting_key("identity_file"), .default_value = std::string(), .validate = validate_text, .retain_invalid = true},
  };
}

SshProxySettings ssh_proxy_settings(const SettingsStore &store) {
  SshProxySettings result{
      .enabled = setting_boolean_value_or_default(store, ssh_proxy_setting_key("enabled"), false),
      .endpoint = {
          .address = setting_string_value_or_default(store, ssh_proxy_setting_key("address"), ""),
          .port = setting_integer_value_or_default(store, ssh_proxy_setting_key("port"), 22),
          .username = setting_string_value_or_default(store, ssh_proxy_setting_key("username"), ""),
          .identity_file = setting_string_value_or_default(store, ssh_proxy_setting_key("identity_file"), "")},
  };
  for (const auto &entry : store.entries) {
    if (entry.definition.key.section != "ssh_proxy" || entry.validation_error.empty()) continue;
    // An invalid enable flag must never silently choose the direct route.
    if (!result.enabled && entry.definition.key.name != "enabled") continue;
    const auto source = setting_value_source(store, entry.definition.key);
    result.validation_errors.push_back("[ssh_proxy] " + entry.definition.key.name + " (" +
        (source == SettingValueSource::global ? "global" : "connection") + "): " + entry.validation_error);
  }
  if (result.enabled && result.endpoint.address.empty())
    result.validation_errors.emplace_back("[ssh_proxy] address: SSH proxy server address is required");
  return result;
}

SshProxySettings ssh_proxy_connection_settings(const SettingsStore &store) {
  const auto kind = general_connection_kind(store);
  if (kind == ConnectionKind::local_shell || kind == ConnectionKind::serial) return {};
  auto result = ssh_proxy_settings(store);
  if (result.enabled && kind == ConnectionKind::ftp &&
      ftp_connection_settings(store).data_connection_mode == FtpDataConnectionMode::active)
    result.validation_errors.emplace_back(_("SSH proxy requires passive FTP/FTPS; select Passive or disable SSH proxy"));
  return result;
}

} // namespace elder_terms
