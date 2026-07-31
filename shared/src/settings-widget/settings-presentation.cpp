#include "settings-presentation.h"

#include <array>
#include <string>

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
    SettingLabelEntry{"general", "name", "Connection name"},
    SettingLabelEntry{"general", "type", "Connection type"},
    SettingLabelEntry{"general", "open_connection",
                      "Open connection shortcut"},
    SettingLabelEntry{"general", "startup_mode", "Startup mode"},
    SettingLabelEntry{"general", "open_application",
                      "Open application shortcut"},
    SettingLabelEntry{"general", "exterior_background",
                      "Title and status bar background"},
    SettingLabelEntry{"general", "background", "Content background"},
    SettingLabelEntry{"terminal", "encoding", "Character encoding"},
    SettingLabelEntry{"terminal", "backspace_code", "Backspace code"},
    SettingLabelEntry{"terminal", "cursor_key_mode", "Cursor key mode"},
    SettingLabelEntry{"terminal", "width", "Columns"},
    SettingLabelEntry{"terminal", "height", "Rows"},
    SettingLabelEntry{"terminal", "zoom", "Zoom factor"},
    SettingLabelEntry{"terminal", "auto_close",
                      "Close window when session ends"},
    SettingLabelEntry{"terminal", "zoom_in_key", "Zoom in shortcut"},
    SettingLabelEntry{"terminal", "zoom_out_key", "Zoom out shortcut"},
    SettingLabelEntry{"telnet", "address", "Address"},
    SettingLabelEntry{"telnet", "port", "Port"},
    SettingLabelEntry{"telnet", "terminal_type", "Terminal type"},
    SettingLabelEntry{"ssh", "address", "Address"},
    SettingLabelEntry{"ssh", "port", "Port"},
    SettingLabelEntry{"ssh", "username", "User name"},
    SettingLabelEntry{"ssh", "identity_file", "Identity file"},
    SettingLabelEntry{"ssh", "terminal_type", "Terminal type"},
    SettingLabelEntry{"sftp", "local_directory", "Local directory"},
    SettingLabelEntry{"sftp", "remote_directory", "Remote directory"},
    SettingLabelEntry{"serial", "device", "Device"},
    SettingLabelEntry{"serial", "baudrate", "Baud rate"},
    SettingLabelEntry{"serial", "bits", "Data bits"},
    SettingLabelEntry{"serial", "parity", "Parity"},
    SettingLabelEntry{"serial", "stop_bit", "Stop bits"},
    SettingLabelEntry{"serial", "flow_control", "Flow control"},
    SettingLabelEntry{"serial", "carrier_detect",
                      "Connection monitoring signal"},
    SettingLabelEntry{"transfer", "base_path", "Transfer base directory"},
    SettingLabelEntry{"transfer", "text_send_bytes_per_second",
                      "Text send rate (bytes/s)"},
    SettingLabelEntry{"transfer", "zmodem_autostart",
                      "Automatically start ZMODEM transfers"},
    SettingLabelEntry{"log", "enabled", "Enable logging"},
    SettingLabelEntry{"log", "base_directory", "Log directory"},
    SettingLabelEntry{"log", "file_name_format", "File name format"},
    SettingLabelEntry{"log", "mode", "Log content"},
};

static constexpr std::array setting_choices{
    SettingChoiceEntry{"general", "type", "local", "Local shell"},
    SettingChoiceEntry{"general", "type", "telnet", "TELNET"},
    SettingChoiceEntry{"general", "type", "serial", "Serial"},
    SettingChoiceEntry{"general", "type", "ssh", "SSH"},
    SettingChoiceEntry{"general", "type", "sftp", "SFTP"},
    SettingChoiceEntry{"general", "startup_mode", "window",
                       "Simple startup"},
    SettingChoiceEntry{"general", "startup_mode", "tray",
                       "System tray only"},
    SettingChoiceEntry{"general", "startup_mode", "window_and_tray",
                       "System tray and main window"},
    SettingChoiceEntry{"terminal", "backspace_code", "bs", "BS"},
    SettingChoiceEntry{"terminal", "backspace_code", "del", "DEL"},
    SettingChoiceEntry{"terminal", "cursor_key_mode", "normal", "Normal"},
    SettingChoiceEntry{"terminal", "cursor_key_mode", "adm3", "ADM3"},
    SettingChoiceEntry{"serial", "parity", "n", "None"},
    SettingChoiceEntry{"serial", "parity", "e", "Even"},
    SettingChoiceEntry{"serial", "parity", "o", "Odd"},
    SettingChoiceEntry{"serial", "flow_control", "none", "None"},
    SettingChoiceEntry{"serial", "flow_control", "xon",
                       "XON/XOFF (software)"},
    SettingChoiceEntry{"serial", "flow_control", "hard",
                       "RTS/CTS (hardware)"},
    SettingChoiceEntry{"serial", "carrier_detect", "cd",
                       "DCD (Data Carrier Detect)"},
    SettingChoiceEntry{"serial", "carrier_detect", "cts",
                       "CTS (Clear to Send)"},
    SettingChoiceEntry{"serial", "carrier_detect", "dsr",
                       "DSR (Data Set Ready)"},
    SettingChoiceEntry{"log", "mode", "raw",
                       "Raw bytes (before character conversion)"},
    SettingChoiceEntry{"log", "mode", "cooked",
                       "UTF-8 text (after character conversion)"},
};

static bool matches_setting(const SettingKey &key, const char *section,
                            const char *name) {
  return key.section == section && key.name == name;
}

const char *settings_ui_text(SettingsUiText text) {
  switch (text) {
  case SettingsUiText::general_tab:
    return "General";
  case SettingsUiText::telnet_tab:
    return "TELNET";
  case SettingsUiText::serial_tab:
    return "Serial";
  case SettingsUiText::ssh_tab:
    return "SSH";
  case SettingsUiText::sftp_tab:
    return "SFTP";
  case SettingsUiText::terminal_tab:
    return "Terminal";
  case SettingsUiText::transfer_tab:
    return "Transfer";
  case SettingsUiText::logging_tab:
    return "Logging";
  case SettingsUiText::apply:
    return "Apply";
  case SettingsUiText::save:
    return "Save";
  case SettingsUiText::cancel:
    return "Cancel";
  case SettingsUiText::reset:
    return "Reset";
  case SettingsUiText::enabled:
    return "Enabled";
  case SettingsUiText::disabled:
    return "Disabled";
  case SettingsUiText::no_color:
    return "No color";
  case SettingsUiText::custom_color:
    return "Custom color";
  case SettingsUiText::press_key_combination:
    return "Press a key combination";
  case SettingsUiText::clear_key_binding:
    return "Clear key binding";
  case SettingsUiText::use_global_default:
    return "Use global default";
  case SettingsUiText::use_built_in_default:
    return "Use built-in default";
  }
  return "";
}

std::string setting_label(const SettingKey &key) {
  for (const SettingLabelEntry &entry : setting_labels) {
    if (matches_setting(key, entry.section, entry.name)) {
      return entry.label;
    }
  }
  return key.name;
}

std::string setting_choice_label(const SettingKey &key,
                                 const std::string &value) {
  for (const SettingChoiceEntry &entry : setting_choices) {
    if (matches_setting(key, entry.section, entry.name) &&
        value == entry.value) {
      return entry.label;
    }
  }
  return value;
}

std::string inherited_setting_label(const std::string &value,
                                    SettingValueSource source) {
  if (value.empty()) {
    return source == SettingValueSource::global ? "Global default"
                                                : "Built-in default";
  }
  return source == SettingValueSource::global
             ? value + " (global default)"
             : value + " (built-in default)";
}

std::string settings_validation_message(const std::string &reason) {
  return reason;
}

} // namespace elder_terms
