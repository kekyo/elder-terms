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
    SettingLabelEntry{"general", "name", N_("Connection name")},
    SettingLabelEntry{"general", "type", N_("Connection type")},
    SettingLabelEntry{"general", "open_connection",
                      N_("Open connection shortcut")},
    SettingLabelEntry{"general", "ui_language", N_("Display language")},
    SettingLabelEntry{"general", "startup_mode", N_("Startup mode")},
    SettingLabelEntry{"general", "open_application",
                      N_("Open application shortcut")},
    SettingLabelEntry{"general", "exterior_background",
                      N_("Title and status bar background")},
    SettingLabelEntry{"general", "background", N_("Content background")},
    SettingLabelEntry{"terminal", "encoding", N_("Character encoding")},
    SettingLabelEntry{"terminal", "backspace_code", N_("Backspace code")},
    SettingLabelEntry{"terminal", "cursor_key_mode", N_("Cursor key mode")},
    SettingLabelEntry{"terminal", "width", N_("Columns")},
    SettingLabelEntry{"terminal", "height", N_("Rows")},
    SettingLabelEntry{"terminal", "zoom", N_("Zoom factor")},
    SettingLabelEntry{"terminal", "auto_close",
                      N_("Close window when session ends")},
    SettingLabelEntry{"terminal", "zoom_in_key", N_("Zoom in shortcut")},
    SettingLabelEntry{"terminal", "zoom_out_key", N_("Zoom out shortcut")},
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
    SettingLabelEntry{"serial", "device", N_("Device")},
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
    SettingLabelEntry{"transfer", "zmodem_autostart",
                      N_("Automatically start ZMODEM transfers")},
    SettingLabelEntry{"log", "enabled", N_("Enable logging")},
    SettingLabelEntry{"log", "base_directory", N_("Log directory")},
    SettingLabelEntry{"log", "file_name_format", N_("File name format")},
    SettingLabelEntry{"log", "mode", N_("Log content")},
};

static constexpr std::array setting_choices{
    SettingChoiceEntry{"general", "type", "local", N_("Local shell")},
    SettingChoiceEntry{"general", "type", "telnet", N_("TELNET")},
    SettingChoiceEntry{"general", "type", "serial", N_("Serial")},
    SettingChoiceEntry{"general", "type", "ssh", N_("SSH")},
    SettingChoiceEntry{"general", "type", "sftp", N_("SFTP")},
    SettingChoiceEntry{"general", "ui_language", "system",
                       N_("System default")},
    SettingChoiceEntry{"general", "ui_language", "en", N_("English")},
    SettingChoiceEntry{"general", "ui_language", "ja", N_("日本語")},
    SettingChoiceEntry{"general", "startup_mode", "window",
                       N_("Simple startup")},
    SettingChoiceEntry{"general", "startup_mode", "tray",
                       N_("System tray only")},
    SettingChoiceEntry{"general", "startup_mode", "window_and_tray",
                       N_("System tray and main window")},
    SettingChoiceEntry{"terminal", "backspace_code", "bs", N_("BS")},
    SettingChoiceEntry{"terminal", "backspace_code", "del", N_("DEL")},
    SettingChoiceEntry{"terminal", "cursor_key_mode", "normal", N_("Normal")},
    SettingChoiceEntry{"terminal", "cursor_key_mode", "adm3", N_("ADM3")},
    SettingChoiceEntry{"serial", "parity", "n", N_("None")},
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
  case SettingsUiText::telnet_tab:
    return _("TELNET");
  case SettingsUiText::serial_tab:
    return _("Serial");
  case SettingsUiText::ssh_tab:
    return _("SSH");
  case SettingsUiText::sftp_tab:
    return _("SFTP");
  case SettingsUiText::terminal_tab:
    return _("Terminal");
  case SettingsUiText::macro_tab:
    return _("Macro");
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
  case SettingsUiText::enabled:
    return _("Enabled");
  case SettingsUiText::disabled:
    return _("Disabled");
  case SettingsUiText::no_color:
    return _("No color");
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
  return _("Invalid value");
}

} // namespace elder_terms
