#pragma once

#include <elder-terms/settings/ssh-settings.h>

namespace elder_terms {

/** Immutable gateway settings shared by network connection backends. */
struct SshProxySettings {
  /** Whether connections must pass through the SSH gateway. */
  bool enabled = false;
  /** Gateway endpoint and login, independent of the final destination. */
  SshEndpointSettings endpoint{.address = {}, .port = 22, .username = {}, .identity_file = {}};
  /** Errors that must be rejected before any network connection is opened. */
  std::vector<std::string> validation_errors{};
};

/** @param name Key name. @returns Key in the ssh_proxy section. */
ELDER_TERMS_API SettingKey ssh_proxy_setting_key(std::string name);

/** @returns Independently inherited SSH proxy setting definitions. */
ELDER_TERMS_API std::vector<SettingDefinition> ssh_proxy_setting_definitions();

/**
 * Reads gateway settings, retaining failures instead of falling back to direct TCP.
 * @param store Source settings, including inherited values.
 * @returns Gateway settings and validation failures; also suitable for global defaults.
 */
ELDER_TERMS_API SshProxySettings ssh_proxy_settings(const SettingsStore &store);

/**
 * Resolves the gateway for the selected connection type.
 * @param store Source connection settings.
 * @returns Disabled gateway for local/serial; validated route for network connections.
 */
ELDER_TERMS_API SshProxySettings ssh_proxy_connection_settings(const SettingsStore &store);

} // namespace elder_terms
