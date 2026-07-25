#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <elder-terms/export.h>
#include <elder-terms/settings/application-settings.h>
#include <elder-terms/settings/general-settings.h>
#include <elder-terms/settings/local-session-settings.h>
#include <elder-terms/settings/log-settings.h>
#include <elder-terms/settings/serial-settings.h>
#include <elder-terms/settings/settings-store.h>
#include <elder-terms/settings/sftp-settings.h>
#include <elder-terms/settings/ssh-settings.h>
#include <elder-terms/settings/telnet-settings.h>
#include <elder-terms/settings/terminal-settings.h>
#include <elder-terms/settings/transfer-settings.h>

namespace elder_terms {

/**
 * Identifies the terminal connection backend.
 */
enum class TerminalConnectionKind {
  /** Spawns the user's local shell in VTE. */
  local_shell,
  /** Connects to a remote TELNET server. */
  telnet,
  /** Connects to a remote SSH server. */
  ssh,
  /** Connects to a serial device. */
  serial,
};

/**
 * Code emitted when the user presses Backspace.
 */
enum class TerminalBackspaceCode {
  /** ASCII BS (0x08). */
  bs,
  /** ASCII DEL (0x7f). */
  del,
};

/**
 * Controls outbound cursor-key sequence handling.
 */
enum class TerminalCursorKeyMode {
  /** Preserve VTE cursor-key escape sequences. */
  normal,
  /** Rewrite cursor-key sequences to ADM3 one-byte codes. */
  adm3,
};

/**
 * Effective character encoding and special-code settings for a terminal
 * session.
 */
struct TerminalTextSettings {
  /** iconv encoding used on the backend byte stream. */
  std::string encoding = "UTF-8";
  /** Code emitted by the Backspace key. */
  TerminalBackspaceCode backspace_code = TerminalBackspaceCode::del;
  /** Outbound cursor-key handling mode. */
  TerminalCursorKeyMode cursor_key_mode = TerminalCursorKeyMode::normal;
};

/**
 * Backend-specific terminal connection settings.
 */
using TerminalConnectionSettings =
    std::variant<LocalShellConnectionSettings, TelnetConnectionSettings,
                 SshConnectionSettings, SerialConnectionSettings>;

/**
 * Describes how the terminal session should connect to a backend.
 */
struct TerminalConnectionProfile {
  /** Effective user-visible connection name. */
  std::string name = "elder-terms";
  /** Selected connection backend. */
  TerminalConnectionKind kind = TerminalConnectionKind::local_shell;
  /** Backend-specific settings. */
  TerminalConnectionSettings settings = LocalShellConnectionSettings{};
  /** Effective terminal character encoding and special-code settings. */
  TerminalTextSettings text_settings{};
};

/**
 * Returns the INI value for a Backspace code.
 *
 * @param code Backspace code.
 * @returns Stable setting value.
 */
ELDER_TERMS_API const char *
terminal_backspace_code_to_string(TerminalBackspaceCode code);

/**
 * Returns the INI value for a cursor-key mode.
 *
 * @param mode Cursor-key mode.
 * @returns Stable setting value.
 */
ELDER_TERMS_API const char *
terminal_cursor_key_mode_to_string(TerminalCursorKeyMode mode);

/**
 * Returns built-in terminal text defaults for a connection kind.
 *
 * @param kind Connection backend kind.
 * @returns Connection-specific terminal text defaults.
 */
ELDER_TERMS_API TerminalTextSettings
default_terminal_text_settings(TerminalConnectionKind kind);

/**
 * Resolves terminal text settings for a connection kind.
 *
 * @param store Source settings store.
 * @param kind Connection backend kind used for implicit defaults.
 * @returns Effective terminal text settings.
 */
ELDER_TERMS_API TerminalTextSettings
terminal_text_settings(const SettingsStore &store,
                       TerminalConnectionKind kind);

/**
 * Paths used to build the initial settings store.
 */
struct SettingsLoadOptions {
  /** Optional persistent INI path passed with -c. */
  std::optional<std::filesystem::path> config_path;
  /** Optional read-only startup INI path passed with -s. */
  std::optional<std::filesystem::path> startup_config_path;
  /** Optional global defaults INI path. Missing files are ignored. */
  std::optional<std::filesystem::path> global_config_path = std::nullopt;
};

/**
 * Result of loading settings from disk.
 */
struct SettingsLoadResult {
  /** Loaded store, with defaults applied for missing or invalid values. */
  SettingsStore store;
  /** True when every requested connection and startup INI file was read
   * successfully. */
  bool loaded = true;
  /** Non-fatal warnings encountered while loading. */
  std::vector<std::string> warnings;
};

/**
 * Result of saving settings to disk.
 */
struct SettingsSaveResult {
  /** True when the target INI file was written successfully. */
  bool saved = false;
  /** Non-fatal warnings encountered while saving. */
  std::vector<std::string> warnings;
};

/**
 * Creates a settings store initialized with application defaults.
 *
 * @param terminal_defaults Default terminal display settings.
 * @param default_connection_name Connection name used when [general] name is
 * absent or blank.
 * @returns Settings store containing all registered keys.
 */
ELDER_TERMS_API SettingsStore
create_default_settings(TerminalDisplaySettings terminal_defaults,
                        std::string default_connection_name);

/**
 * Returns the standard path for global defaults.
 *
 * @returns $XDG_CONFIG_HOME/elder-terms/global.ini using GLib's resolved
 * user configuration directory.
 */
ELDER_TERMS_API std::filesystem::path default_global_config_path();

/**
 * Loads settings from optional INI files.
 *
 * @param options Global, persistent, and startup configuration paths.
 * @param default_terminal_zoom VTE's runtime default font scale.
 * @returns Loaded settings and non-fatal warnings.
 */
ELDER_TERMS_API SettingsLoadResult
load_settings(const SettingsLoadOptions &options,
              gdouble default_terminal_zoom);

/**
 * Loads global defaults for editing.
 *
 * @param global_config_path Global defaults INI path.
 * @param default_terminal_zoom VTE's runtime default font scale.
 * @returns Store whose global keys are represented as explicit overrides.
 *
 * @remarks A missing global file produces an empty, editable defaults store.
 * The [general] name key is ignored.
 */
ELDER_TERMS_API SettingsLoadResult
load_global_settings(const std::filesystem::path &global_config_path,
                     gdouble default_terminal_zoom);

/**
 * Saves settings to an INI file.
 *
 * @param store Settings to serialize.
 * @param config_path Target INI path.
 * @returns Save status and warnings.
 *
 * @remarks Only inherited values are omitted. Explicit overrides are saved
 * even when equal to their fallback.
 */
ELDER_TERMS_API SettingsSaveResult
save_settings(const SettingsStore &store,
              const std::filesystem::path &config_path);

/**
 * Saves explicit global defaults to an INI file.
 *
 * @param store Editable global settings store.
 * @param global_config_path Target global.ini path.
 * @returns Save status and warnings.
 *
 * @remarks The [general] name key is always excluded.
 */
ELDER_TERMS_API SettingsSaveResult
save_global_settings(const SettingsStore &store,
                     const std::filesystem::path &global_config_path);

/**
 * Extracts the terminal connection profile from a store.
 *
 * @param store Source settings store.
 * @returns Terminal connection profile, or no value for a non-terminal
 * connection type.
 */
ELDER_TERMS_API std::optional<TerminalConnectionProfile>
terminal_connection_profile(const SettingsStore &store);

} // namespace elder_terms
