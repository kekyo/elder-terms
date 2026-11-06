// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "settings-presentation.h"

#include <array>
#include <string>

#define GETTEXT_PACKAGE "elder-terms"
#include <glib/gi18n-lib.h>

namespace elder_terms {

struct SettingLabelEntry {
  const char *section;
  const char *name;
  const char *label;
};

struct SettingChoiceEntry {
  const char *section;
  const char *name;
  const char *value;
  const char *label;
};

static constexpr std::array setting_labels{
    SettingLabelEntry{"ssh_proxy", "address", N_("Address")},
    SettingLabelEntry{"ssh_proxy", "port", N_("Port")},
    SettingLabelEntry{"ssh_proxy", "username", N_("User name")},
    SettingLabelEntry{"ssh_proxy", "identity_file", N_("Identity file")},
    SettingLabelEntry{"ftp", "tls_mode", N_("Encryption")},
    SettingLabelEntry{"http", "base_url", N_("Base URL")},
    SettingLabelEntry{"http", "start_url", N_("Start page URL")},
    SettingLabelEntry{"http", "authentication", N_("Authentication")},
    SettingLabelEntry{"http", "username", N_("User name")},
    SettingLabelEntry{"http", "local_directory", N_("Local directory")},
    SettingLabelEntry{"http", "ca_file", N_("CA certificates")},
    SettingLabelEntry{"http", "certificate_error_action", N_("Certificate validation failure")},
    SettingLabelEntry{"http", "connect_timeout_seconds", N_("Connection timeout (seconds)")},
    SettingLabelEntry{"http", "idle_timeout_seconds", N_("Idle timeout (seconds)")},
    SettingLabelEntry{"webdav", "scheme", N_("Connection security")},
    SettingLabelEntry{"webdav", "address", N_("Address")},
    SettingLabelEntry{"webdav", "port", N_("Port")},
    SettingLabelEntry{"webdav", "base_path", N_("Published path")},
    SettingLabelEntry{"webdav", "authentication", N_("Authentication")},
    SettingLabelEntry{"webdav", "username", N_("User name")},
    SettingLabelEntry{"webdav", "local_directory", N_("Local directory")},
    SettingLabelEntry{"webdav", "remote_directory", N_("Remote directory")},
    SettingLabelEntry{"webdav", "ca_file", N_("CA certificates")},
    SettingLabelEntry{"webdav", "certificate_error_action", N_("Certificate validation failure")},
    SettingLabelEntry{"webdav", "connect_timeout_seconds", N_("Connection timeout (seconds)")},
    SettingLabelEntry{"webdav", "idle_timeout_seconds", N_("Idle timeout (seconds)")},
    SettingLabelEntry{"smb", "address", N_("Address")},
    SettingLabelEntry{"nfs", "address", N_("Address")},
    SettingLabelEntry{"nfs", "version", N_("NFS version")},
    SettingLabelEntry{"nfs", "filename_encoding", N_("Filename encoding")},
    SettingLabelEntry{"nfs", "export_path", N_("Initial export path (empty: server root)")},
    SettingLabelEntry{"nfs", "known_exports", N_("Known exports (one path per line)")},
    SettingLabelEntry{"nfs", "discover_exports", N_("Discover exports")},
    SettingLabelEntry{"nfs", "namespace_path", N_("Initial v4 namespace path (empty: server root)")},
    SettingLabelEntry{"nfs", "known_namespace_paths", N_("Known v4 namespace paths (one per line)")},
    SettingLabelEntry{"nfs", "discover_namespace", N_("Explore v4 public namespace")},
    SettingLabelEntry{"nfs", "local_directory", N_("Local directory")},
    SettingLabelEntry{"nfs", "nfs_port", N_("NFS port (0: automatic)")},
    SettingLabelEntry{"nfs", "mount_port", N_("MOUNT port (0: automatic)")},
    SettingLabelEntry{"nfs", "require_reserved_port", N_("Require reserved source port")},
    SettingLabelEntry{"nfs", "uid", N_("User ID (-1: current process)")},
    SettingLabelEntry{"nfs", "gid", N_("Group ID (-1: current process)")},
    SettingLabelEntry{"nfs", "supplementary_gids", N_("Additional group IDs (one per line)")},
    SettingLabelEntry{"nfs", "connect_timeout_seconds", N_("Connection timeout (seconds)")},
    SettingLabelEntry{"nfs", "request_timeout_seconds", N_("Request timeout (seconds)")},
    SettingLabelEntry{"smb", "port", N_("Port")},
    SettingLabelEntry{"smb", "version", N_("SMB version")},
    SettingLabelEntry{"smb", "netbios_name", N_("NetBIOS server name (port 139)")},
    SettingLabelEntry{"smb", "domain", N_("Domain")},
    SettingLabelEntry{"smb", "username", N_("User name")},
    SettingLabelEntry{"smb", "local_directory", N_("Local directory")},
    SettingLabelEntry{"smb", "remote_directory", N_("Remote directory")},
    SettingLabelEntry{"smb", "require_signing", N_("Require signing")},
    SettingLabelEntry{"smb", "require_encryption", N_("Require encryption")},
    SettingLabelEntry{"smb", "connect_timeout_seconds", N_("Connection timeout (seconds)")},
    SettingLabelEntry{"smb", "idle_timeout_seconds", N_("Idle timeout (seconds)")},
    SettingLabelEntry{"ftp", "tls_min_version", N_("Minimum TLS version")},
    SettingLabelEntry{"ftp", "tls_max_version", N_("Maximum TLS version")},
    SettingLabelEntry{"ftp", "tls_auth_order", N_("AUTH command preference")},
    SettingLabelEntry{"ftp", "tls_compatibility", N_("TLS compatibility")},
    SettingLabelEntry{"ftp", "certificate_error_action", N_("Certificate validation failure")},
    SettingLabelEntry{"ftp", "ca_file", N_("CA certificates")},
    SettingLabelEntry{"ftp", "tls_cipher_list", N_("TLS 1.2 and earlier ciphers")},
    SettingLabelEntry{"ftp", "tls13_cipher_list", N_("TLS 1.3 ciphers")},
    SettingLabelEntry{"general", "name", N_("Connection name")},
    SettingLabelEntry{"general", "icon", N_("Connection icon")},
    SettingLabelEntry{"general", "type", N_("Connection type")},
    SettingLabelEntry{"general", "open_connection",
                      N_("Open connection shortcut")},
    SettingLabelEntry{"general", "new_window", N_("Use for \"New Window\"")},
    SettingLabelEntry{"general", "ui_language", N_("Display language")},
    SettingLabelEntry{"general", "startup_mode", N_("Startup mode")},
    SettingLabelEntry{"general", "open_application",
                      N_("Open application shortcut")},
    SettingLabelEntry{"general", "exterior_background",
                      N_("Title and status bar background")},
    SettingLabelEntry{"general", "background", N_("Content background")},
    SettingLabelEntry{"general", "auto_close",
                      N_("Close window when session ends")},
    SettingLabelEntry{"general", "compact_mode", N_("Compact mode")},
    SettingLabelEntry{"local", "command_line", N_("Startup command")},
    SettingLabelEntry{"terminal", "encoding", N_("Character encoding")},
    SettingLabelEntry{"terminal", "ambiguous_width", N_("Ambiguous character width")},
    SettingLabelEntry{"terminal", "backspace_code", N_("Backspace code")},
    SettingLabelEntry{"terminal", "cursor_key_mode", N_("Cursor key mode")},
    SettingLabelEntry{"terminal", "cursor_shape", N_("Cursor shape")},
    SettingLabelEntry{"terminal", "color_filter", N_("Color filter")},
    SettingLabelEntry{"terminal", "regex_link_decoration", N_("Regex link display")},
    SettingLabelEntry{"terminal", "osc8_link_decoration", N_("OSC 8 link display")},
    SettingLabelEntry{"terminal", "color_vision", N_("Color discrimination")},
    SettingLabelEntry{"terminal", "color_vision_strength", N_("Correction strength (0–100)")},
    SettingLabelEntry{"terminal", "background_mode", N_("Background mode")},
    SettingLabelEntry{"terminal", "cell_background_mode", N_("Cell background transparency")},
    SettingLabelEntry{"terminal", "background_opacity", N_("Background opacity (%)")},
    SettingLabelEntry{"terminal", "background_tint_color", N_("Background tint")},
    SettingLabelEntry{"terminal", "background_tint_opacity", N_("Tint opacity (%)")},
    SettingLabelEntry{"terminal", "background_image", N_("Background image")},
    SettingLabelEntry{"terminal", "background_image_opacity", N_("Image opacity (%)")},
    SettingLabelEntry{"terminal", "background_image_fit", N_("Image placement")},
    SettingLabelEntry{"terminal", "cursor_blink", N_("Cursor blinking")},
    SettingLabelEntry{"terminal", "return_code", N_("Enter/Return code")},
    SettingLabelEntry{"terminal", "width", N_("Columns")},
    SettingLabelEntry{"terminal", "height", N_("Rows")},
    SettingLabelEntry{"terminal", "scrollback_lines",
                      N_("Scrollback lines")},
    SettingLabelEntry{"terminal", "block_copy_trim_trailing_spaces",
                      N_("Trim trailing spaces in block copies")},
    SettingLabelEntry{"terminal", "font_size_dip", N_("Font size (DIP)")},
    SettingLabelEntry{"general", "indicator_color", N_("Active indicator color")},
    SettingLabelEntry{"general", "indicator_off_color", N_("Inactive indicator color")},
    SettingLabelEntry{"terminal", "font_families", N_("Font families")},
    SettingLabelEntry{"general", "show_border",
                      N_("Show window side borders")},
    SettingLabelEntry{"terminal", "accessibility_enabled",
                      N_("Screen reader access")},
    SettingLabelEntry{"general", "border_width",
                      N_("Window side border width (px)")},
    SettingLabelEntry{"terminal", "bell_sound", N_("BEL sound")},
    SettingLabelEntry{"terminal", "visual_bell", N_("Visual BEL notification")},
    SettingLabelEntry{"general", "receive_effect", N_("Titlebar receive effect")},
    SettingLabelEntry{"terminal", "zoom_in_key", N_("Zoom in shortcut")},
    SettingLabelEntry{"terminal", "zoom_out_key", N_("Zoom out shortcut")},
    SettingLabelEntry{"terminal", "send_break_key",
                      N_("Send BREAK shortcut")},
    SettingLabelEntry{"telnet", "address", N_("Address")},
    SettingLabelEntry{"telnet", "port", N_("Port")},
    SettingLabelEntry{"telnet", "terminal_type", N_("Terminal type")},
    SettingLabelEntry{"ssh", "address", N_("Address")},
    SettingLabelEntry{"ssh", "port", N_("Port")},
    SettingLabelEntry{"ssh", "username", N_("User name")},
    SettingLabelEntry{"ssh", "identity_file", N_("Identity file")},
    SettingLabelEntry{"ssh", "terminal_type", N_("Terminal type")},
    SettingLabelEntry{"sftp", "local_directory", N_("Local directory")},
    SettingLabelEntry{"sftp", "remote_directory", N_("Remote directory")},
    SettingLabelEntry{"ftp", "address", N_("Address")},
    SettingLabelEntry{"ftp", "port", N_("Port")},
    SettingLabelEntry{"ftp", "username", N_("User name")},
    SettingLabelEntry{"ftp", "data_connection_mode",
                      N_("Data connection mode")},
    SettingLabelEntry{"ftp", "local_directory", N_("Local directory")},
    SettingLabelEntry{"ftp", "remote_directory", N_("Remote directory")},
    SettingLabelEntry{"serial", "device", N_("Device")},
    SettingLabelEntry{"serial", "device_match_mode",
                      N_("Device identification")},
    SettingLabelEntry{"serial", "device_usb_serial",
                      N_("USB serial number")},
    SettingLabelEntry{"serial", "baudrate", N_("Baud rate")},
    SettingLabelEntry{"serial", "bits", N_("Data bits")},
    SettingLabelEntry{"serial", "parity", N_("Parity")},
    SettingLabelEntry{"serial", "stop_bit", N_("Stop bits")},
    SettingLabelEntry{"serial", "flow_control", N_("Flow control")},
    SettingLabelEntry{"serial", "carrier_detect",
                      N_("Connection monitoring signal")},
    SettingLabelEntry{"transfer", "base_path",
                      N_("Transfer base directory")},
    SettingLabelEntry{"transfer", "text_send_bytes_per_second",
                      N_("Text send rate (bytes/s)")},
    SettingLabelEntry{"transfer", "text_send_follow_return_code",
                      N_("Follow Enter/Return code for text send")},
    SettingLabelEntry{"transfer", "zmodem_autostart",
                      N_("Automatically start ZMODEM transfers")},
    SettingLabelEntry{"log", "enabled", N_("Enable logging")},
    SettingLabelEntry{"log", "base_directory", N_("Log directory")},
    SettingLabelEntry{"log", "file_name_format", N_("File name format")},
    SettingLabelEntry{"log", "mode", N_("Log content")},
};

static constexpr std::array setting_choices{
    SettingChoiceEntry{"ftp", "tls_mode", "none", N_("FTP (unencrypted)")},
    SettingChoiceEntry{"ftp", "tls_mode", "explicit", N_("FTPS (explicit TLS)")},
    SettingChoiceEntry{"ftp", "tls_mode", "implicit", N_("FTPS (implicit TLS)")},
    SettingChoiceEntry{"ftp", "tls_auth_order", "tls", N_("Prefer AUTH TLS")},
    SettingChoiceEntry{"ftp", "tls_auth_order", "ssl", N_("Prefer AUTH SSL")},
    SettingChoiceEntry{"ftp", "tls_auth_order", "default", N_("Library default")},
    SettingChoiceEntry{"ftp", "tls_compatibility", "standard", N_("Standard")},
    SettingChoiceEntry{"ftp", "tls_compatibility", "openssl_legacy", N_("Legacy TLS (relaxed cipher/signature rules)")},
    SettingChoiceEntry{"ftp", "certificate_error_action", "reject", N_("Reject connection")},
    SettingChoiceEntry{"ftp", "certificate_error_action", "prompt", N_("Confirm in an overlay")},
    SettingChoiceEntry{"ftp", "tls_max_version", "default", N_("Backend maximum")},
    SettingChoiceEntry{"general", "type", "local", N_("Local shell")},
    SettingChoiceEntry{"general", "type", "telnet", N_("TELNET")},
    SettingChoiceEntry{"general", "type", "serial", N_("Serial")},
    SettingChoiceEntry{"general", "type", "ssh", N_("SSH")},
    SettingChoiceEntry{"general", "type", "sftp", N_("SFTP")},
    SettingChoiceEntry{"general", "type", "ftp", N_("FTP")},
    SettingChoiceEntry{"general", "type", "webdav", N_("WebDAV")},
    SettingChoiceEntry{"general", "type", "http", N_("HTTP/HTTPS")},
    SettingChoiceEntry{"general", "type", "smb", N_("SMB")},
    SettingChoiceEntry{"smb", "version", "auto", N_("Automatic — SMB 2/3")},
    SettingChoiceEntry{"smb", "version", "1", N_("SMB 1")},
    SettingChoiceEntry{"smb", "version", "2", N_("SMB 2")},
    SettingChoiceEntry{"smb", "version", "3", N_("SMB 3")},
    SettingChoiceEntry{"general", "type", "nfs", N_("NFS")},
    SettingChoiceEntry{"http", "authentication", "auto", N_("Automatic (Basic or Digest)")},
    SettingChoiceEntry{"http", "authentication", "basic", N_("Basic")},
    SettingChoiceEntry{"http", "authentication", "digest", N_("Digest")},
    SettingChoiceEntry{"http", "authentication", "none", N_("None")},
    SettingChoiceEntry{"http", "ca_file", "default", N_("System CA certificates")},
    SettingChoiceEntry{"http", "ca_file", "custom", N_("Custom")},
    SettingChoiceEntry{"http", "certificate_error_action", "reject", N_("Reject connection")},
    SettingChoiceEntry{"http", "certificate_error_action", "prompt", N_("Confirm in an overlay")},
    SettingChoiceEntry{"webdav", "scheme", "https", N_("HTTPS (encrypted)")},
    SettingChoiceEntry{"webdav", "scheme", "http", N_("HTTP (unencrypted)")},
    SettingChoiceEntry{"webdav", "authentication", "auto", N_("Automatic (Basic or Digest)")},
    SettingChoiceEntry{"webdav", "authentication", "basic", N_("Basic")},
    SettingChoiceEntry{"webdav", "authentication", "digest", N_("Digest")},
    SettingChoiceEntry{"webdav", "authentication", "none", N_("None")},
    SettingChoiceEntry{"webdav", "ca_file", "default", N_("System CA certificates")},
    SettingChoiceEntry{"webdav", "ca_file", "custom", N_("Custom")},
    SettingChoiceEntry{"webdav", "certificate_error_action", "reject", N_("Reject connection")},
    SettingChoiceEntry{"webdav", "certificate_error_action", "prompt", N_("Confirm in an overlay")},
    SettingChoiceEntry{"general", "ui_language", "system",
                       N_("System default")},
    SettingChoiceEntry{"general", "ui_language", "en", N_("English")},
    SettingChoiceEntry{"general", "ui_language", "ar", N_("العربية")},
    SettingChoiceEntry{"general", "ui_language", "es", N_("Español")},
    SettingChoiceEntry{"general", "ui_language", "fr", N_("Français")},
    SettingChoiceEntry{"general", "ui_language", "hi", N_("हिन्दी")},
    SettingChoiceEntry{"general", "ui_language", "ja", N_("日本語")},
    SettingChoiceEntry{"general", "ui_language", "ko", N_("한국어")},
    SettingChoiceEntry{"general", "ui_language", "pt", N_("Português")},
    SettingChoiceEntry{"general", "ui_language", "ru", N_("Русский")},
    SettingChoiceEntry{"general", "ui_language", "zh", N_("中文")},
    SettingChoiceEntry{"general", "startup_mode", "window",
                       N_("Simple startup")},
    SettingChoiceEntry{"general", "startup_mode", "background",
                       N_("Background only")},
    SettingChoiceEntry{"general", "startup_mode", "tray",
                       N_("System tray only")},
    SettingChoiceEntry{"general", "startup_mode", "window_and_tray",
                       N_("System tray and main window")},
    SettingChoiceEntry{"terminal", "backspace_code", "auto", N_("Auto")},
    SettingChoiceEntry{"terminal", "backspace_code", "bs", N_("BS")},
    SettingChoiceEntry{"terminal", "backspace_code", "del", N_("DEL")},
    SettingChoiceEntry{"terminal", "cursor_key_mode", "normal", N_("Normal")},
    SettingChoiceEntry{"terminal", "ambiguous_width", "narrow", N_("1 column")},
    SettingChoiceEntry{"terminal", "ambiguous_width", "wide", N_("2 columns")},
    SettingChoiceEntry{"terminal", "ambiguous_width", "font", N_("Font dependent")},
    SettingChoiceEntry{"terminal", "cursor_shape", "block", N_("Full box")},
    SettingChoiceEntry{"terminal", "color_filter", "none", N_("No correction")},
    SettingChoiceEntry{"terminal", "regex_link_decoration", "none", N_("No decoration")},
    SettingChoiceEntry{"terminal", "regex_link_decoration", "underline", N_("Always underline")},
    SettingChoiceEntry{"terminal", "regex_link_decoration", "hover", N_("Underline on hover")},
    SettingChoiceEntry{"terminal", "regex_link_decoration", "validated", N_("Validate links (with dimming)")},
    SettingChoiceEntry{"terminal", "osc8_link_decoration", "none", N_("No decoration")},
    SettingChoiceEntry{"terminal", "osc8_link_decoration", "underline", N_("Always underline")},
    SettingChoiceEntry{"terminal", "osc8_link_decoration", "hover", N_("Underline on hover")},
    SettingChoiceEntry{"terminal", "osc8_link_decoration", "validated", N_("Validate links (with dimming)")},
    SettingChoiceEntry{"terminal", "color_filter", "oklch", N_("Improve visibility (Oklch)")},
    SettingChoiceEntry{"terminal", "color_filter", "reverse-palette", N_("Reverse optimized palette (E)")},
    SettingChoiceEntry{"terminal", "color_filter", "reverse-cam16", N_("Reverse appearance (D, CAM16)")},
    SettingChoiceEntry{"terminal", "color_filter", "reverse-roles", N_("Reverse text and background (C)")},
    SettingChoiceEntry{"terminal", "color_filter", "reverse-contrast", N_("Reverse contrast (B)")},
    SettingChoiceEntry{"terminal", "color_filter", "reverse-lightness", N_("Reverse lightness (A)")},
    SettingChoiceEntry{"terminal", "color_vision", "none", N_("No correction")},
    SettingChoiceEntry{"terminal", "color_vision", "protan", N_("Protan (P)")},
    SettingChoiceEntry{"terminal", "color_vision", "deutan", N_("Deutan (D)")},
    SettingChoiceEntry{"terminal", "color_vision", "tritan", N_("Tritan (T)")},
    SettingChoiceEntry{"terminal", "background_mode", "opaque", N_("Opaque")},
    SettingChoiceEntry{"terminal", "background_mode", "transparent", N_("Transparent")},
    SettingChoiceEntry{"terminal", "background_mode", "blur", N_("Frosted glass")},
    SettingChoiceEntry{"terminal", "cell_background_mode", "transparent", N_("Transparent")},
    SettingChoiceEntry{"terminal", "cell_background_mode", "opaque", N_("Opaque")},
    SettingChoiceEntry{"terminal", "background_image_fit", "cover", N_("Cover")},
    SettingChoiceEntry{"terminal", "background_image_fit", "contain", N_("Contain")},
    SettingChoiceEntry{"terminal", "background_image_fit", "stretch", N_("Stretch")},
    SettingChoiceEntry{"terminal", "cursor_shape", "bar", N_("Vertical box")},
    SettingChoiceEntry{"terminal", "cursor_shape", "underline", N_("Underline box")},
    SettingChoiceEntry{"terminal", "cursor_key_mode", "trs80", N_("TRS80")},
    SettingChoiceEntry{"terminal", "return_code", "auto", N_("Auto")},
    SettingChoiceEntry{"terminal", "return_code", "cr", N_("CR")},
    SettingChoiceEntry{"terminal", "return_code", "lf", N_("LF")},
    SettingChoiceEntry{"terminal", "return_code", "crlf", N_("CRLF")},
    SettingChoiceEntry{"ftp", "data_connection_mode", "passive",
                       N_("Passive (recommended)")},
    SettingChoiceEntry{"ftp", "data_connection_mode", "active",
                       N_("Active")},
    SettingChoiceEntry{"serial", "parity", "n", N_("None")},
    SettingChoiceEntry{"serial", "device_match_mode", "path",
                       N_("Device path")},
    SettingChoiceEntry{"serial", "device_match_mode", "by-id",
                       N_("Stable device identity")},
    SettingChoiceEntry{"serial", "device_match_mode", "by-path",
                       N_("Physical USB port")},
    SettingChoiceEntry{"serial", "parity", "e", N_("Even")},
    SettingChoiceEntry{"serial", "parity", "o", N_("Odd")},
    SettingChoiceEntry{"serial", "flow_control", "none", N_("None")},
    SettingChoiceEntry{"serial", "flow_control", "xon",
                       N_("XON/XOFF (software)")},
    SettingChoiceEntry{"serial", "flow_control", "hard",
                       N_("RTS/CTS (hardware)")},
    SettingChoiceEntry{"serial", "carrier_detect", "cd",
                       N_("DCD (Data Carrier Detect)")},
    SettingChoiceEntry{"serial", "carrier_detect", "cts",
                       N_("CTS (Clear to Send)")},
    SettingChoiceEntry{"serial", "carrier_detect", "dsr",
                       N_("DSR (Data Set Ready)")},
    SettingChoiceEntry{"serial", "carrier_detect", "ignore",
                       N_("Ignore (do not monitor)")},
    SettingChoiceEntry{"log", "mode", "raw",
                       N_("Raw bytes (before character conversion)")},
    SettingChoiceEntry{"log", "mode", "cooked",
                       N_("UTF-8 text (after character conversion)")},
};

static bool matches_setting(const SettingKey &key, const char *section,
                            const char *name) {
  return key.section == section && key.name == name;
}

const char *settings_ui_text(SettingsUiText text) {
  switch (text) {
  case SettingsUiText::general_tab:
    return _("General");
  case SettingsUiText::local_tab:
    return _("Local");
  case SettingsUiText::telnet_tab:
    return _("TELNET");
  case SettingsUiText::serial_tab:
    return _("Serial");
  case SettingsUiText::ssh_tab:
    return _("SSH");
  case SettingsUiText::sftp_tab:
    return _("SFTP");
  case SettingsUiText::ftp_tab:
    return _("FTP");
  case SettingsUiText::terminal_tab:
    return _("Terminal");
  case SettingsUiText::terminal_appearance_tab:
    return _("Terminal appearance");
  case SettingsUiText::macro_tab:
    return _("Macro");
  case SettingsUiText::links_tab:
    return _("Links");
  case SettingsUiText::transfer_tab:
    return _("Transfer");
  case SettingsUiText::logging_tab:
    return _("Logging");
  case SettingsUiText::apply:
    return _("Apply");
  case SettingsUiText::save:
    return _("Save");
  case SettingsUiText::cancel:
    return _("Cancel");
  case SettingsUiText::reset:
    return _("Reset");
  case SettingsUiText::select_file:
    return _("Select file");
  case SettingsUiText::open:
    return _("Open");
  case SettingsUiText::select_terminal_font:
    return _("Select Terminal Font");
  case SettingsUiText::font_add:
    return _("Add");
  case SettingsUiText::font_remove:
    return _("Remove");
  case SettingsUiText::font_move_up:
    return _("Move up");
  case SettingsUiText::font_move_down:
    return _("Move down");
  case SettingsUiText::enabled:
    return _("Enabled");
  case SettingsUiText::disabled:
    return _("Disabled");
  case SettingsUiText::font_inherit_global:
    return _("Inherited from: global settings");
  case SettingsUiText::font_inherit_builtin:
    return _("Inherited from: app defaults");
  case SettingsUiText::font_app_defaults:
    return _("App defaults");
  case SettingsUiText::font_use_app_defaults:
    return _("Use app defaults");
  case SettingsUiText::font_specify_connection:
    return _("Specify for this connection");
  case SettingsUiText::font_specify:
    return _("Specify fonts");
  case SettingsUiText::no_color:
    return _("No color");
  case SettingsUiText::default_color:
    return _("Default color");
  case SettingsUiText::custom_color:
    return _("Custom color");
  case SettingsUiText::press_key_combination:
    return _("Press a key combination");
  case SettingsUiText::clear_key_binding:
    return _("Clear key binding");
  case SettingsUiText::use_global_default:
    return _("Use global default");
  case SettingsUiText::use_built_in_default:
    return _("Use built-in default");
  case SettingsUiText::macro_rules:
    return _("Macro rules");
  case SettingsUiText::macro_id:
    return _("Rule ID");
  case SettingsUiText::macro_regex:
    return _("Regular expression");
  case SettingsUiText::macro_action:
    return _("Action");
  case SettingsUiText::macro_send:
    return _("Text to send");
  case SettingsUiText::macro_command:
    return _("Command");
  case SettingsUiText::macro_arguments:
    return _("Arguments");
  case SettingsUiText::macro_add:
    return _("Add rule");
  case SettingsUiText::macro_remove:
    return _("Remove rule");
  case SettingsUiText::macro_move_up:
    return _("Move up");
  case SettingsUiText::macro_move_down:
    return _("Move down");
  case SettingsUiText::macro_send_action:
    return _("Send text");
  case SettingsUiText::macro_command_action:
    return _("Run command");
  case SettingsUiText::macro_add_argument:
    return _("Add argument");
  case SettingsUiText::macro_remove_argument:
    return _("Remove argument");
  case SettingsUiText::serial_no_device:
    return _("No device");
  case SettingsUiText::serial_stable_id:
    return _("Stable ID");
  case SettingsUiText::serial_usb_serial:
    return _("USB serial number");
  case SettingsUiText::serial_current_node:
    return _("Current device node");
  case SettingsUiText::unavailable:
    return _("Unavailable");
  case SettingsUiText::ip_scan:
    return _("IP scan");
  case SettingsUiText::ip_address:
    return _("IP address");
  case SettingsUiText::resolved_name:
    return _("Hostname");
  case SettingsUiText::ssh_sftp_port_column:
    return _("SSH/SFTP(22)");
  case SettingsUiText::telnet_port_column:
    return _("TELNET(23)");
  case SettingsUiText::ftp_port_column:
    return _("FTP/FTPS(21)");
  case SettingsUiText::ftps_port_column:
    return _("FTPS(990)");
  case SettingsUiText::webdav_https_port_column:
    return _("WebDAV(443)");
  case SettingsUiText::webdav_http_port_column:
    return _("WebDAV(80)");
  case SettingsUiText::smb_port_column:
    return _("SMB(445)");
  case SettingsUiText::nfs_port_column:
    return _("NFS(2049)");
  case SettingsUiText::ip_scan_description:
    return _("Shows TCP port responses. Service support and authentication are not verified.");
  case SettingsUiText::scanning:
    return _("Scanning…");
  case SettingsUiText::scan_complete:
    return _("Scan complete");
  case SettingsUiText::scan_failed:
    return _("Scan failed");
  }
  return "";
}

std::string setting_label(const SettingKey &key) {
  for (const SettingLabelEntry &entry : setting_labels) {
    if (matches_setting(key, entry.section, entry.name)) {
      return _(entry.label);
    }
  }
  return key.name;
}

std::string setting_choice_label(const SettingKey &key,
                                 const std::string &value) {
  if ((key.section == "terminal" && key.name == "visual_bell") ||
      (key.section == "general" && key.name == "receive_effect")) {
    if (value == "disabled") return _("Disabled");
    if (value == "weak") return _("Weak");
    if (value == "medium") return _("Medium");
    if (value == "strong") return _("Strong");
  }
  for (const SettingChoiceEntry &entry : setting_choices) {
    if (matches_setting(key, entry.section, entry.name) &&
        value == entry.value) {
      return _(entry.label);
    }
  }
  return value;
}

std::string inherited_setting_label(const std::string &value,
                                    SettingValueSource source) {
  if (value.empty()) {
    return source == SettingValueSource::global ? _("Global default")
                                                : _("Built-in default");
  }
  gchar *label =
      source == SettingValueSource::global
          ? g_strdup_printf(_("%s (global default)"), value.c_str())
          : g_strdup_printf(_("%s (built-in default)"), value.c_str());
  const std::string result = label;
  g_free(label);
  return result;
}

std::string settings_validation_message(const std::string &reason) {
  if (reason.empty()) {
    return {};
  }
  if (reason == "Enter a whole number") {
    return _("Enter a whole number");
  }
  if (reason == "Enter a number") {
    return _("Enter a number");
  }

  constexpr char range_prefix[] = "Value must be between ";
  if (reason.starts_with(range_prefix)) {
    const std::string range = reason.substr(sizeof(range_prefix) - 1);
    const std::size_t separator = range.find(" and ");
    if (separator != std::string::npos) {
      const std::string minimum = range.substr(0, separator);
      const std::string maximum = range.substr(separator + 5);
      gchar *message =
          g_strdup_printf(_("Value must be between %s and %s"),
                          minimum.c_str(), maximum.c_str());
      const std::string result = message;
      g_free(message);
      return result;
    }
  }

  if (reason == "must include Ctrl, Shift, Alt, or Super") {
    return _("Must include Ctrl, Shift, Alt, or Super");
  }
  if (reason == "contains an empty token") {
    return _("Contains an empty token");
  }

  constexpr char unknown_modifier_prefix[] =
      "contains an unknown modifier: ";
  if (reason.starts_with(unknown_modifier_prefix)) {
    const std::string modifier =
        reason.substr(sizeof(unknown_modifier_prefix) - 1);
    gchar *message =
        g_strdup_printf(_("Contains an unknown modifier: %s"),
                        modifier.c_str());
    const std::string result = message;
    g_free(message);
    return result;
  }

  constexpr char duplicate_modifier_prefix[] =
      "contains a duplicate modifier: ";
  if (reason.starts_with(duplicate_modifier_prefix)) {
    const std::string modifier =
        reason.substr(sizeof(duplicate_modifier_prefix) - 1);
    gchar *message =
        g_strdup_printf(_("Contains a duplicate modifier: %s"),
                        modifier.c_str());
    const std::string result = message;
    g_free(message);
    return result;
  }

  if (reason == "does not contain a key") {
    return _("Does not contain a key");
  }

  constexpr char unknown_key_prefix[] = "contains an unknown key: ";
  if (reason.starts_with(unknown_key_prefix)) {
    const std::string key = reason.substr(sizeof(unknown_key_prefix) - 1);
    gchar *message =
        g_strdup_printf(_("Contains an unknown key: %s"), key.c_str());
    const std::string result = message;
    g_free(message);
    return result;
  }

  if (reason ==
      "Zoom in and zoom out must use different key bindings") {
    return _("Zoom in and zoom out must use different key bindings");
  }
  if (reason == "must not be empty") {
    return _("Must not be empty");
  }
  if (reason == "is not supported by iconv in both directions") {
    return _("The character encoding is not supported");
  }
  if (reason == "contains a NUL byte") {
    return _("Contains a NUL byte");
  }
  if (reason ==
      "must be default or an existing absolute .oga, .ogg, or .wav file") {
    return _(
        "Enter default or select an existing absolute .oga, .ogg, or .wav file");
  }
  if (reason == "contains an unmatched closing brace") {
    return _("Contains an unmatched closing brace");
  }
  if (reason == "contains an unmatched opening brace") {
    return _("Contains an unmatched opening brace");
  }
  if (reason == "contains an unknown placeholder") {
    return _("Contains an unknown placeholder");
  }
  if (reason == "must be relative to the log base directory") {
    return _("Must be relative to the log base directory");
  }
  if (reason == "must not contain parent directory traversal") {
    return _("Must not contain parent directory traversal");
  }
  if (reason == "must name a log file") {
    return _("Must name a log file");
  }
  constexpr const char *messages[] = {
      N_("A known export path is required when export discovery is disabled"),
      N_("A known NFS v4 namespace path is required when namespace discovery is disabled"),
      N_("[ftp] tls_min_version must not exceed tls_max_version"),
      N_("HTTP or HTTPS URL required"),
      N_("HTTP base URL must not contain a query or fragment"),
      N_("HTTP base URL must end in a slash"),
      N_("Malformed HTTP URL escape"),
      N_("Ambiguous HTTP URL character"),
      N_("Ambiguous encoded HTTP path separator"),
      N_("Invalid HTTP base URL"),
      N_("Invalid HTTP endpoint"),
      N_("must be an absolute path without dot segments or control characters"),
      N_("must be an encoded absolute URL path without a query, fragment, or dot segments"),
      N_("must not contain control characters"),
      N_("security levels must be selected with tls_compatibility"),
      N_("TLS 1.3 cipher suites must encrypt data"),
      N_("SMB server address is required"),
      N_("NFS server address is required"),
      N_("HTTP base URL is required"),
      N_("HTTP start page is outside the base URL"),
      N_("[ssh_proxy] address: SSH proxy server address is required"),
      N_("Enter a valid value"),
      N_("must contain valid UTF-8 without control characters"),
      N_("must be a hostname or IP address without a scheme, path, or credentials"),
      N_("must be an integer between 1 and 65535"),
      N_("must be between 0 (automatic) and 65535"),
      N_("must be between 1 and 86400 seconds"),
      N_("must be an integer between 1 and 86400"),
      N_("must be auto, 1, 2, or 3"),
      N_("must be disabled, weak, medium, or strong"),
      N_("must select NFS version 2, 3 or 4.0"),
      N_("must be at most 15 ASCII letters, digits, hyphens, or underscores"),
      N_("must be an absolute path without dot segments, colons, trailing dots or spaces, or control characters"),
      N_("must be empty or an absolute path without empty, dot, or parent components"),
      N_("must contain absolute paths without empty, dot, or parent components"),
      N_("must be -1 (current process) or an unsigned 32-bit ID"),
      N_("must contain at most 16 unsigned 32-bit group IDs"),
      N_("must be an available encoding without replacement or ignore modifiers"),
      N_("must be an absolute PEM CA bundle path or empty"),
      N_("identifier must not be empty"),
      N_("identifier may contain only letters, digits, '-' and '_'"),
      N_("identifier must be unique"),
      N_("regular expression must not be empty"),
      N_("command must not be empty"),
      N_("command must not contain a NUL byte"),
      N_("send text must not be empty"),
      N_("path template must not be empty when path validation is enabled"),
      N_("font family names must be valid UTF-8 without NUL characters"),
      N_("font family names must not contain commas or control characters"),
      N_("font family names must be non-empty and unique"),
      N_("contains an empty capture placeholder"),
      N_("contains an unmatched capture placeholder"),
      N_("literal '$' must be written as '$$'"),
      N_("contains more than one capture transformation"),
  };
  for (const auto *message : messages)
    if (reason == message) return _(message);
  constexpr char capture_prefix[] = "references an unknown capture: ";
  if (reason.starts_with(capture_prefix)) {
    auto *message = g_strdup_printf(_("references an unknown capture: %s"), reason.substr(sizeof(capture_prefix) - 1).c_str());
    const std::string result = message;
    g_free(message);
    return result;
  }
  return reason;
}

} // namespace elder_terms
