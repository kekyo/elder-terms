// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include <elder-terms/settings/ssh-proxy-settings.h>
#include "validation-issues.h"
#include <elder-terms/settings/general-settings.h>
#include <elder-terms/settings/ftp-settings.h>
#include <elder-terms/settings/nfs-settings.h>
#include <elder-terms/settings/smb-settings.h>

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
    result.validation_issues.push_back({{entry.definition.key}, entry.validation_error});
    const auto source = setting_value_source(store, entry.definition.key);
    result.validation_errors.push_back("[ssh_proxy] " + entry.definition.key.name + " (" +
        (source == SettingValueSource::global ? "global" : "connection") + "): " + entry.validation_error);
  }
  if (result.enabled && result.endpoint.address.empty())
    add_validation_issue(result, {ssh_proxy_setting_key("address")}, "[ssh_proxy] address: SSH proxy server address is required");
  return result;
}

SshProxySettings ssh_proxy_connection_settings(const SettingsStore &store) {
  const auto kind = general_connection_kind(store);
  if (kind == ConnectionKind::local_shell || kind == ConnectionKind::serial) return {};
  auto result = ssh_proxy_settings(store);
  if (result.enabled && kind == ConnectionKind::ftp &&
      ftp_connection_settings(store).data_connection_mode == FtpDataConnectionMode::active)
    add_validation_issue(result, {ssh_proxy_setting_key("enabled"), ftp_data_connection_mode_setting_key()},
        _("SSH proxy requires passive FTP/FTPS; select Passive or disable SSH proxy"));
  if (result.enabled && kind == ConnectionKind::nfs && nfs_connection_settings(store).require_reserved_port)
    add_validation_issue(result, {ssh_proxy_setting_key("enabled"), nfs_setting_key("require_reserved_port")},
        _("SSH proxy cannot require reserved NFS source ports; disable the reserved-port requirement or SSH proxy"));
  if (result.enabled && kind == ConnectionKind::smb && smb_connection_settings(store).version == "1")
    add_validation_issue(result, {ssh_proxy_setting_key("enabled"), smb_setting_key("version")},
        _("SSH proxy is unavailable with SMB 1; select SMB 2/3 or disable SSH proxy"));
  return result;
}

} // namespace elder_terms
