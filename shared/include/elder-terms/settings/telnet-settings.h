#pragma once

#include <string>
#include <vector>

#include <glib.h>

#include <elder-terms/settings/settings-store.h>

namespace elder_terms {

/**
 * Settings for the TELNET connection backend.
 */
struct TelnetConnectionSettings {
  /** TELNET server address or hostname. */
  std::string address;
  /** TELNET server TCP port. */
  gint64 port;
};

/**
 * Returns TELNET setting definitions.
 *
 * @returns Setting definitions for the telnet INI section.
 */
std::vector<SettingDefinition> telnet_connection_setting_definitions();

/**
 * Returns the setting key for [telnet] address.
 *
 * @returns Setting key for TELNET server address.
 */
SettingKey telnet_address_setting_key();

/**
 * Returns the setting key for [telnet] port.
 *
 * @returns Setting key for TELNET server port.
 */
SettingKey telnet_port_setting_key();

/**
 * Extracts TELNET connection settings from a store.
 *
 * @param store Source settings store.
 * @returns Typed TELNET connection settings.
 */
TelnetConnectionSettings telnet_connection_settings(const SettingsStore &store);

/**
 * Appends TELNET-specific non-fatal warnings.
 *
 * @param store Source settings store.
 * @param warnings Warning sink.
 */
void append_telnet_connection_warnings(const SettingsStore &store,
                                       std::vector<std::string> *warnings);

} // namespace elder_terms
