#include <elder-terms/settings.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace elder_terms_settings_test {

using elder_terms::create_default_settings;
using elder_terms::clear_explicit_setting_value;
using elder_terms::default_terminal_text_settings;
using elder_terms::default_terminal_display_settings;
using elder_terms::general_type_setting_key;
using elder_terms::key_binding_matches;
using elder_terms::parse_key_binding;
using elder_terms::load_settings;
using elder_terms::LocalShellConnectionSettings;
using elder_terms::terminal_log_base_directory_setting_key;
using elder_terms::terminal_log_enabled_setting_key;
using elder_terms::terminal_log_file_name_format_is_valid;
using elder_terms::terminal_log_file_name_format_setting_key;
using elder_terms::TerminalLogMode;
using elder_terms::TerminalLogSettings;
using elder_terms::terminal_log_mode_setting_key;
using elder_terms::terminal_log_settings;
using elder_terms::save_settings;
using elder_terms::SerialCarrierDetect;
using elder_terms::SerialConnectionSettings;
using elder_terms::SerialFlowControl;
using elder_terms::SerialParity;
using elder_terms::serial_baudrate_setting_key;
using elder_terms::serial_bits_setting_key;
using elder_terms::serial_carrier_detect_setting_key;
using elder_terms::serial_connection_settings;
using elder_terms::serial_device_setting_key;
using elder_terms::serial_flow_control_setting_key;
using elder_terms::serial_parity_setting_key;
using elder_terms::serial_stop_bit_setting_key;
using elder_terms::set_explicit_setting_value;
using elder_terms::set_setting_value;
using elder_terms::SettingsLoadOptions;
using elder_terms::SettingsLoadResult;
using elder_terms::SettingsSaveResult;
using elder_terms::SettingsStore;
using elder_terms::TelnetConnectionSettings;
using elder_terms::telnet_address_setting_key;
using elder_terms::telnet_port_setting_key;
using elder_terms::terminal_auto_close_setting_key;
using elder_terms::TerminalBackspaceCode;
using elder_terms::TerminalConnectionKind;
using elder_terms::TerminalConnectionProfile;
using elder_terms::TerminalCursorKeyMode;
using elder_terms::TerminalDisplaySettings;
using elder_terms::TerminalTextSettings;
using elder_terms::terminal_auto_close;
using elder_terms::terminal_backspace_code_setting_key;
using elder_terms::terminal_connection_profile;
using elder_terms::terminal_cursor_key_mode_setting_key;
using elder_terms::terminal_display_settings;
using elder_terms::terminal_encoding_choices;
using elder_terms::terminal_encoding_name_is_valid;
using elder_terms::terminal_encoding_setting_key;
using elder_terms::terminal_height_setting_key;
using elder_terms::terminal_width_setting_key;
using elder_terms::terminal_zoom_setting_key;
using elder_terms::terminal_key_bindings;
using elder_terms::terminal_zoom_in_key_setting_key;
using elder_terms::terminal_zoom_out_key_setting_key;
using elder_terms::transfer_base_path;
using elder_terms::transfer_base_path_setting_key;
using elder_terms::transfer_text_send_bytes_per_second;
using elder_terms::transfer_text_send_bytes_per_second_setting_key;
using elder_terms::transfer_zmodem_autostart;
using elder_terms::transfer_zmodem_autostart_setting_key;

static bool warnings_contain(const std::vector<std::string> &warnings,
                             const std::string &text) {
  for (const std::string &warning : warnings) {
    if (warning.find(text) != std::string::npos) {
      return true;
    }
  }
  return false;
}

static void expect_true(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

static std::filesystem::path temporary_config_path(const std::string &name) {
  const auto timestamp =
      std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         ("elder-terms-settings-" + std::to_string(timestamp) + "-" + name +
          ".ini");
}

static void write_config(const std::filesystem::path &path,
                         const std::string &content) {
  std::ofstream file(path);
  expect_true(file.good(), "failed to create test configuration file");
  file << content;
  expect_true(file.good(), "failed to write test configuration file");
}

static std::string read_config(const std::filesystem::path &path) {
  std::ifstream file(path);
  expect_true(file.good(), "failed to open test configuration file");
  return std::string(std::istreambuf_iterator<char>(file),
                     std::istreambuf_iterator<char>());
}

static void remove_config(const std::filesystem::path &path) {
  std::error_code error;
  std::filesystem::remove(path, error);
}

static void test_default_settings() {
  const SettingsStore store =
      create_default_settings(default_terminal_display_settings(1.2));
  const TerminalDisplaySettings display = terminal_display_settings(store);
  expect_true(display.width == 80, "default terminal width should be 80");
  expect_true(display.height == 24, "default terminal height should be 24");
  expect_true(display.zoom == 1.2, "default terminal zoom should be retained");
  expect_true(terminal_auto_close(store),
              "default terminal auto-close should be enabled");
  const auto key_bindings = terminal_key_bindings(store);
  expect_true(key_bindings.zoom_in.has_value(),
              "default terminal zoom-in key should be enabled");
  expect_true(key_bindings.zoom_out.has_value(),
              "default terminal zoom-out key should be enabled");
  expect_true(key_binding_matches(*key_bindings.zoom_in, GDK_KEY_plus,
                                  GDK_CONTROL_MASK),
              "default terminal zoom-in key should be Ctrl+plus");
  expect_true(key_binding_matches(*key_bindings.zoom_out, GDK_KEY_minus,
                                  GDK_CONTROL_MASK),
              "default terminal zoom-out key should be Ctrl+minus");
  expect_true(transfer_base_path(store).empty(),
              "default transfer base path should be empty");
  expect_true(transfer_text_send_bytes_per_second(store) == 1024,
              "default text send rate should be 1024 bytes per second");
  expect_true(!transfer_zmodem_autostart(store),
              "default local transfer ZMODEM auto-start should be disabled");
  const TerminalLogSettings log = terminal_log_settings(store);
  expect_true(!log.enabled,
              "terminal logging should be disabled by default");
  expect_true(log.base_directory == "{XDG_DOCUMENTS}/logs/",
              "default terminal log base directory should use XDG "
              "Documents/logs");
  expect_true(log.file_name_format ==
                  "{YYYYMMDD}_{hhmmss}_{fff}.txt",
              "default terminal log file name format should include milliseconds");
  expect_true(log.mode == TerminalLogMode::raw,
              "default terminal log mode should preserve raw bytes");

  const TerminalConnectionProfile profile = terminal_connection_profile(store);
  expect_true(profile.kind == TerminalConnectionKind::local_shell,
              "default connection should be local shell");
  expect_true(std::holds_alternative<LocalShellConnectionSettings>(
                  profile.settings),
              "default connection settings should be local shell settings");
  expect_true(profile.text_settings.encoding == "UTF-8",
              "default local terminal encoding should be UTF-8");
  expect_true(profile.text_settings.backspace_code ==
                  TerminalBackspaceCode::del,
              "default local Backspace should send DEL");
  expect_true(profile.text_settings.cursor_key_mode ==
                  TerminalCursorKeyMode::normal,
              "default local cursor keys should use normal sequences");
}

static void test_terminal_log_settings() {
  const std::filesystem::path path = temporary_config_path("terminal-log");
  write_config(path,
               "[log]\n"
               "enabled=true\n"
               "base_directory=/tmp/elder-terms-logs\n"
               "file_name_format={YYYYMMDD}/{hhmmss}_{fff}.txt\n"
               "mode=cooked\n");

  const SettingsLoadResult result = load_settings(
      SettingsLoadOptions{
          .config_path = std::optional<std::filesystem::path>{path},
          .startup_config_path = std::nullopt,
      },
      1.0);
  remove_config(path);

  const TerminalLogSettings settings = terminal_log_settings(result.store);
  expect_true(settings.enabled,
              "configured terminal logging should be enabled");
  expect_true(settings.base_directory == "/tmp/elder-terms-logs",
              "terminal log base directory should come from configuration");
  expect_true(settings.file_name_format ==
                  "{YYYYMMDD}/{hhmmss}_{fff}.txt",
              "terminal log file name format should come from configuration");
  expect_true(settings.mode == TerminalLogMode::cooked,
              "configured cooked terminal log mode should be retained");
}

static void test_terminal_log_file_name_format_validation() {
  std::string reason;
  expect_true(terminal_log_file_name_format_is_valid(
                  "{YYYYMMDD}/{hhmmss}_{fff}.txt", &reason),
              "nested terminal log file name format should be valid");
  expect_true(terminal_log_file_name_format_is_valid("session.log", &reason),
              "literal terminal log file name should be valid");
  expect_true(!terminal_log_file_name_format_is_valid("/tmp/session.log",
                                                       &reason),
              "absolute terminal log file name format should be invalid");
  expect_true(!terminal_log_file_name_format_is_valid("../session.log",
                                                       &reason),
              "parent traversal in terminal log format should be invalid");
  expect_true(!terminal_log_file_name_format_is_valid("{unknown}.log",
                                                       &reason),
              "unknown terminal log placeholder should be invalid");
  expect_true(!terminal_log_file_name_format_is_valid("{YYYYMMDD}/", &reason),
              "terminal log format ending in a directory should be invalid");
}

static void test_invalid_terminal_log_values_fall_back_to_defaults() {
  const std::filesystem::path path =
      temporary_config_path("invalid-terminal-log");
  write_config(path,
               "[log]\n"
               "file_name_format=../{YYYYMMDD}.txt\n"
               "mode=formatted\n");

  const SettingsLoadResult result = load_settings(
      SettingsLoadOptions{
          .config_path = std::optional<std::filesystem::path>{path},
          .startup_config_path = std::nullopt,
      },
      1.0);
  remove_config(path);

  const TerminalLogSettings settings = terminal_log_settings(result.store);
  expect_true(settings.file_name_format ==
                  "{YYYYMMDD}_{hhmmss}_{fff}.txt",
              "invalid terminal log format should use the default");
  expect_true(settings.mode == TerminalLogMode::raw,
              "invalid terminal log mode should use raw mode");
  expect_true(warnings_contain(
                  result.warnings,
                  "invalid configuration value [log] file_name_format"),
              "invalid terminal log format should emit a warning");
  expect_true(warnings_contain(result.warnings,
                               "invalid configuration value [log] mode"),
              "invalid terminal log mode should emit a warning");
}

static void test_terminal_text_defaults_follow_connection_type() {
  const TerminalTextSettings local =
      default_terminal_text_settings(TerminalConnectionKind::local_shell);
  expect_true(local.encoding == "UTF-8" &&
                  local.backspace_code == TerminalBackspaceCode::del &&
                  local.cursor_key_mode == TerminalCursorKeyMode::normal,
              "local terminal text defaults should match gtk-oldtype");

  const TerminalTextSettings telnet =
      default_terminal_text_settings(TerminalConnectionKind::telnet);
  expect_true(telnet.encoding == "UTF-8" &&
                  telnet.backspace_code == TerminalBackspaceCode::bs &&
                  telnet.cursor_key_mode == TerminalCursorKeyMode::normal,
              "TELNET terminal text defaults should match gtk-oldtype");

  const TerminalTextSettings serial =
      default_terminal_text_settings(TerminalConnectionKind::serial);
  expect_true(serial.encoding == "UTF-8" &&
                  serial.backspace_code == TerminalBackspaceCode::bs &&
                  serial.cursor_key_mode == TerminalCursorKeyMode::adm3,
              "serial terminal text defaults should match gtk-oldtype");
}

static void test_terminal_text_explicit_settings_override_connection_defaults() {
  SettingsStore store =
      create_default_settings(default_terminal_display_settings(1.0));
  set_setting_value(&store, general_type_setting_key(),
                    elder_terms::SettingValue{std::string("serial")});
  set_explicit_setting_value(
      &store, terminal_encoding_setting_key(),
      elder_terms::SettingValue{std::string("CP932")});
  set_explicit_setting_value(
      &store, terminal_backspace_code_setting_key(),
      elder_terms::SettingValue{std::string("del")});
  set_explicit_setting_value(
      &store, terminal_cursor_key_mode_setting_key(),
      elder_terms::SettingValue{std::string("normal")});

  TerminalConnectionProfile profile = terminal_connection_profile(store);
  expect_true(profile.text_settings.encoding == "CP932",
              "explicit terminal encoding should override serial default");
  expect_true(profile.text_settings.backspace_code ==
                  TerminalBackspaceCode::del,
              "explicit Backspace code should override serial default");
  expect_true(profile.text_settings.cursor_key_mode ==
                  TerminalCursorKeyMode::normal,
              "explicit cursor-key mode should override serial default");

  clear_explicit_setting_value(&store, terminal_encoding_setting_key());
  clear_explicit_setting_value(&store,
                               terminal_backspace_code_setting_key());
  clear_explicit_setting_value(&store,
                               terminal_cursor_key_mode_setting_key());
  profile = terminal_connection_profile(store);
  expect_true(profile.text_settings.encoding == "UTF-8" &&
                  profile.text_settings.backspace_code ==
                      TerminalBackspaceCode::bs &&
                  profile.text_settings.cursor_key_mode ==
                      TerminalCursorKeyMode::adm3,
              "clearing terminal text overrides should restore serial defaults");
}

static void test_terminal_encoding_choices_are_supported() {
  const std::vector<std::string> choices = terminal_encoding_choices();
  expect_true(!choices.empty(),
              "terminal encoding choices should not be empty");
  expect_true(std::find(choices.begin(), choices.end(), "UTF-8") !=
                  choices.end(),
              "terminal encoding choices should include UTF-8");
  for (const std::string &choice : choices) {
    std::string reason;
    expect_true(terminal_encoding_name_is_valid(choice, &reason),
                "every terminal encoding choice should be usable: " +
                    choice + " " + reason);
  }

  std::string reason;
  expect_true(terminal_encoding_name_is_valid("  CP932  ", &reason),
              "encoding validation should accept a supported trimmed alias");
  expect_true(!terminal_encoding_name_is_valid(
                  "elder-terms-invalid-encoding", &reason),
              "encoding validation should reject an unknown iconv name");
}

static void test_invalid_terminal_text_values_fall_back_to_type_defaults() {
  const std::filesystem::path path =
      temporary_config_path("invalid-terminal-text");
  write_config(path,
               "[general]\n"
               "type=serial\n"
               "\n"
               "[terminal]\n"
               "encoding=elder-terms-invalid-encoding\n"
               "backspace_code=invalid\n"
               "cursor_key_mode=invalid\n");

  const SettingsLoadResult result = load_settings(
      SettingsLoadOptions{
          .config_path = std::optional<std::filesystem::path>{path},
          .startup_config_path = std::nullopt,
      },
      1.0);
  remove_config(path);

  const TerminalTextSettings text =
      terminal_connection_profile(result.store).text_settings;
  expect_true(text.encoding == "UTF-8" &&
                  text.backspace_code == TerminalBackspaceCode::bs &&
                  text.cursor_key_mode == TerminalCursorKeyMode::adm3,
              "invalid terminal text values should use serial defaults");
  expect_true(warnings_contain(
                  result.warnings,
                  "invalid configuration value [terminal] encoding"),
              "invalid terminal encoding should emit a warning");
  expect_true(warnings_contain(
                  result.warnings,
                  "invalid configuration value [terminal] backspace_code"),
              "invalid Backspace code should emit a warning");
  expect_true(warnings_contain(
                  result.warnings,
                  "invalid configuration value [terminal] cursor_key_mode"),
              "invalid cursor-key mode should emit a warning");
}

static void test_key_binding_parser_uses_exact_modifiers() {
  const auto parsed = parse_key_binding(" CTRL - shift+PLUS ");
  expect_true(parsed.error.empty(),
              "mixed separators and case should parse");
  expect_true(parsed.binding.has_value(),
              "non-empty key binding should be enabled");
  expect_true(key_binding_matches(*parsed.binding, GDK_KEY_plus,
                                  static_cast<GdkModifierType>(
                                      GDK_CONTROL_MASK | GDK_SHIFT_MASK)),
              "configured modifiers should match exactly");
  expect_true(!key_binding_matches(*parsed.binding, GDK_KEY_plus,
                                   GDK_SHIFT_MASK),
              "missing Ctrl should not match");
  expect_true(!key_binding_matches(*parsed.binding, GDK_KEY_plus,
                                   static_cast<GdkModifierType>(
                                       GDK_CONTROL_MASK | GDK_SHIFT_MASK |
                                       GDK_MOD1_MASK)),
              "an additional Alt modifier should not match");
  expect_true(key_binding_matches(
                  *parsed.binding, GDK_KEY_plus,
                  static_cast<GdkModifierType>(GDK_CONTROL_MASK |
                                               GDK_SHIFT_MASK |
                                               GDK_LOCK_MASK)),
              "lock state should not count as a hotkey modifier");

  const auto all_modifiers =
      parse_key_binding("super-alt-shift-ctrl-F1");
  expect_true(all_modifiers.error.empty() &&
                  all_modifiers.binding.has_value(),
              "all supported modifiers and case-insensitive F1 should parse");
  expect_true(
      key_binding_matches(
          *all_modifiers.binding, GDK_KEY_F1,
          static_cast<GdkModifierType>(
              GDK_SUPER_MASK | GDK_MOD1_MASK | GDK_SHIFT_MASK |
              GDK_CONTROL_MASK)),
      "all configured modifiers should match");

  const auto disabled = parse_key_binding("   ");
  expect_true(disabled.error.empty() && !disabled.binding.has_value(),
              "an empty key binding should disable the action");

  for (const std::string &invalid : {
           std::string("ctrl++plus"), std::string("ctrl+ctrl+plus"),
           std::string("ctrl+unknown_key_name"), std::string("ctrl"),
           std::string("plus+ctrl")}) {
    expect_true(!parse_key_binding(invalid).error.empty(),
                "invalid key binding should report a validation error: " +
                    invalid);
  }
}

static void test_terminal_key_binding_configuration() {
  const std::filesystem::path custom_path =
      temporary_config_path("terminal-key-bindings");
  write_config(custom_path,
               "[terminal]\n"
               "zoom_in_key=alt+Up\n"
               "zoom_out_key=\n");
  const SettingsLoadResult custom = load_settings(
      SettingsLoadOptions{
          .config_path = std::optional<std::filesystem::path>{custom_path},
          .startup_config_path = std::nullopt,
      },
      1.0);
  remove_config(custom_path);

  const auto custom_bindings = terminal_key_bindings(custom.store);
  expect_true(custom_bindings.zoom_in.has_value() &&
                  key_binding_matches(*custom_bindings.zoom_in, GDK_KEY_Up,
                                      GDK_MOD1_MASK),
              "configured terminal zoom-in key should be loaded");
  expect_true(!custom_bindings.zoom_out.has_value(),
              "empty terminal zoom-out key should disable the action");

  const std::filesystem::path invalid_path =
      temporary_config_path("terminal-key-bindings-invalid");
  write_config(invalid_path,
               "[terminal]\n"
               "zoom_in_key=ctrl++plus\n"
               "zoom_out_key=ctrl+minus\n");
  const SettingsLoadResult invalid = load_settings(
      SettingsLoadOptions{
          .config_path = std::optional<std::filesystem::path>{invalid_path},
          .startup_config_path = std::nullopt,
      },
      1.0);
  remove_config(invalid_path);
  expect_true(warnings_contain(
                  invalid.warnings,
                  "invalid configuration value [terminal] zoom_in_key"),
              "invalid terminal key binding should emit a warning");
  const auto invalid_bindings = terminal_key_bindings(invalid.store);
  expect_true(invalid_bindings.zoom_in.has_value() &&
                  key_binding_matches(*invalid_bindings.zoom_in,
                                      GDK_KEY_plus, GDK_CONTROL_MASK),
              "invalid terminal key binding should use its default");

  const std::filesystem::path conflict_path =
      temporary_config_path("terminal-key-bindings-conflict");
  write_config(conflict_path,
               "[terminal]\n"
               "zoom_in_key=alt+F1\n"
               "zoom_out_key=ALT-f1\n");
  const SettingsLoadResult conflict = load_settings(
      SettingsLoadOptions{
          .config_path = std::optional<std::filesystem::path>{conflict_path},
          .startup_config_path = std::nullopt,
      },
      1.0);
  remove_config(conflict_path);
  expect_true(warnings_contain(conflict.warnings,
                               "conflicting terminal key bindings"),
              "conflicting terminal key bindings should emit a warning");
  const auto conflict_bindings = terminal_key_bindings(conflict.store);
  expect_true(conflict_bindings.zoom_in.has_value() &&
                  conflict_bindings.zoom_out.has_value() &&
                  key_binding_matches(*conflict_bindings.zoom_in,
                                      GDK_KEY_plus, GDK_CONTROL_MASK) &&
                  key_binding_matches(*conflict_bindings.zoom_out,
                                      GDK_KEY_minus, GDK_CONTROL_MASK),
              "conflicting terminal key bindings should both use defaults");
}

static void test_transfer_base_path_setting() {
  const std::filesystem::path path = temporary_config_path("transfer-base");
  write_config(path,
               "[transfer]\n"
               "base_path=file:///tmp/elder-terms-transfer\n");

  const SettingsLoadResult result = load_settings(
      SettingsLoadOptions{
          .config_path = std::optional<std::filesystem::path>{path},
          .startup_config_path = std::nullopt,
      },
      1.0);
  remove_config(path);

  expect_true(transfer_base_path(result.store) ==
                  "file:///tmp/elder-terms-transfer",
              "transfer base_path should come from the configuration file");
}

static void test_transfer_text_send_bytes_per_second_setting() {
  const std::filesystem::path valid = temporary_config_path("text-send-rate");
  write_config(valid,
               "[transfer]\n"
               "text_send_bytes_per_second=4096\n");

  const SettingsLoadResult valid_result = load_settings(
      SettingsLoadOptions{
          .config_path = std::optional<std::filesystem::path>{valid},
          .startup_config_path = std::nullopt,
      },
      1.0);
  remove_config(valid);

  expect_true(transfer_text_send_bytes_per_second(valid_result.store) == 4096,
              "text send rate should come from the configuration file");

  const std::filesystem::path invalid =
      temporary_config_path("invalid-text-send-rate");
  write_config(invalid,
               "[transfer]\n"
               "text_send_bytes_per_second=8000001\n");

  const SettingsLoadResult invalid_result = load_settings(
      SettingsLoadOptions{
          .config_path = std::optional<std::filesystem::path>{invalid},
          .startup_config_path = std::nullopt,
      },
      1.0);
  remove_config(invalid);

  expect_true(transfer_text_send_bytes_per_second(invalid_result.store) ==
                  1024,
              "out-of-range text send rate should use the default");
  expect_true(warnings_contain(invalid_result.warnings,
                               "text_send_bytes_per_second"),
              "out-of-range text send rate should emit a warning");
}

static void test_transfer_zmodem_autostart_setting() {
  SettingsStore local_store =
      create_default_settings(default_terminal_display_settings(1.0));
  expect_true(!transfer_zmodem_autostart(local_store),
              "implicit local ZMODEM auto-start should be disabled");

  set_setting_value(&local_store, general_type_setting_key(),
                    elder_terms::SettingValue{std::string("serial")});
  expect_true(transfer_zmodem_autostart(local_store),
              "implicit serial ZMODEM auto-start should be enabled");

  set_explicit_setting_value(
      &local_store, transfer_zmodem_autostart_setting_key(),
      elder_terms::SettingValue{false});
  expect_true(!transfer_zmodem_autostart(local_store),
              "explicit false should disable serial ZMODEM auto-start");

  const std::filesystem::path path =
      temporary_config_path("transfer-zmodem-autostart");
  write_config(path,
               "[general]\n"
               "type=telnet\n"
               "\n"
               "[transfer]\n"
               "zmodem_autostart=true\n");

  const SettingsLoadResult result = load_settings(
      SettingsLoadOptions{
          .config_path = std::optional<std::filesystem::path>{path},
          .startup_config_path = std::nullopt,
      },
      1.0);
  remove_config(path);

  expect_true(transfer_zmodem_autostart(result.store),
              "explicit true should enable TELNET ZMODEM auto-start");
}

static void test_telnet_profile() {
  const std::filesystem::path path = temporary_config_path("telnet-profile");
  write_config(path,
               "[general]\n"
               "type=telnet\n"
               "\n"
               "[telnet]\n"
               "address=127.0.0.1\n"
               "port=2323\n");

  const SettingsLoadResult result = load_settings(
      SettingsLoadOptions{
          .config_path = std::optional<std::filesystem::path>{path},
          .startup_config_path = std::nullopt,
      },
      1.0);
  remove_config(path);

  const TerminalConnectionProfile profile =
      terminal_connection_profile(result.store);
  expect_true(profile.kind == TerminalConnectionKind::telnet,
              "configured connection should be TELNET");
  const auto *settings =
      std::get_if<TelnetConnectionSettings>(&profile.settings);
  expect_true(settings != nullptr,
              "configured connection settings should be TELNET settings");
  expect_true(settings->address == "127.0.0.1",
              "TELNET address should come from the configuration file");
  expect_true(settings->port == 2323,
              "TELNET port should come from the configuration file");
}

static void test_serial_profile() {
  const std::filesystem::path path = temporary_config_path("serial-profile");
  write_config(path,
               "[general]\n"
               "type=serial\n"
               "\n"
               "[serial]\n"
               "device=/dev/ttyUSB0\n"
               "baudrate=115200\n"
               "bits=7\n"
               "parity=e\n"
               "stop_bit=2\n"
               "flow_control=xon\n"
               "carrier_detect=dsr\n");

  const SettingsLoadResult result = load_settings(
      SettingsLoadOptions{
          .config_path = std::optional<std::filesystem::path>{path},
          .startup_config_path = std::nullopt,
      },
      1.0);
  remove_config(path);

  const TerminalConnectionProfile profile =
      terminal_connection_profile(result.store);
  expect_true(profile.kind == TerminalConnectionKind::serial,
              "configured connection should be serial");
  const auto *settings =
      std::get_if<SerialConnectionSettings>(&profile.settings);
  expect_true(settings != nullptr,
              "configured connection settings should be serial settings");
  expect_true(settings->device == "/dev/ttyUSB0",
              "serial device should come from the configuration file");
  expect_true(settings->baudrate == 115200,
              "serial baudrate should come from the configuration file");
  expect_true(settings->bits == 7,
              "serial bits should come from the configuration file");
  expect_true(settings->parity == SerialParity::even,
              "serial parity should come from the configuration file");
  expect_true(settings->stop_bit == 2,
              "serial stop bit should come from the configuration file");
  expect_true(settings->flow_control == SerialFlowControl::xon,
              "serial flow control should come from the configuration file");
  expect_true(settings->carrier_detect == SerialCarrierDetect::dsr,
              "serial carrier detect should come from the configuration file");
}

static void test_invalid_values_fall_back_to_defaults() {
  const std::filesystem::path path = temporary_config_path("invalid-values");
  write_config(path,
               "[general]\n"
               "type=telnet\n"
               "\n"
               "[terminal]\n"
               "width=invalid\n"
               "height=-5\n"
               "zoom=0\n"
               "auto_close=invalid\n"
               "\n"
               "[telnet]\n"
               "address=127.0.0.1\n"
               "port=70000\n");

  const SettingsLoadResult result = load_settings(
      SettingsLoadOptions{
          .config_path = std::optional<std::filesystem::path>{path},
          .startup_config_path = std::nullopt,
      },
      1.0);
  remove_config(path);

  const TerminalDisplaySettings display =
      terminal_display_settings(result.store);
  expect_true(display.width == 80,
              "invalid terminal width should fall back to default");
  expect_true(display.height == 24,
              "invalid terminal height should fall back to default");
  expect_true(display.zoom == 1.0,
              "invalid terminal zoom should fall back to default");
  expect_true(terminal_auto_close(result.store),
              "invalid terminal auto-close should fall back to default");

  const TerminalConnectionProfile profile =
      terminal_connection_profile(result.store);
  const auto *settings =
      std::get_if<TelnetConnectionSettings>(&profile.settings);
  expect_true(settings != nullptr,
              "TELNET profile should still be selected after invalid port");
  expect_true(settings->port == 23,
              "invalid TELNET port should fall back to default");

  expect_true(warnings_contain(result.warnings,
                               "invalid configuration value [terminal] width"),
              "invalid terminal width should emit a warning");
  expect_true(warnings_contain(result.warnings,
                               "invalid configuration value [terminal] height"),
              "invalid terminal height should emit a warning");
  expect_true(warnings_contain(result.warnings,
                               "invalid configuration value [terminal] zoom"),
              "invalid terminal zoom should emit a warning");
  expect_true(warnings_contain(
                  result.warnings,
                  "invalid configuration value [terminal] auto_close"),
              "invalid terminal auto-close should emit a warning");
  expect_true(warnings_contain(result.warnings,
                               "invalid configuration value [telnet] port"),
              "invalid TELNET port should emit a warning");
}

static void test_invalid_serial_values_fall_back_to_defaults() {
  const std::filesystem::path path =
      temporary_config_path("invalid-serial-values");
  write_config(path,
               "[general]\n"
               "type=serial\n"
               "\n"
               "[serial]\n"
               "baudrate=149\n"
               "bits=9\n"
               "parity=x\n"
               "stop_bit=3\n"
               "flow_control=invalid\n"
               "carrier_detect=ri\n");

  const SettingsLoadResult result = load_settings(
      SettingsLoadOptions{
          .config_path = std::optional<std::filesystem::path>{path},
          .startup_config_path = std::nullopt,
      },
      1.0);
  remove_config(path);

  const TerminalConnectionProfile profile =
      terminal_connection_profile(result.store);
  const auto *settings =
      std::get_if<SerialConnectionSettings>(&profile.settings);
  expect_true(settings != nullptr,
              "serial profile should still be selected after invalid values");
  expect_true(settings->device.empty(),
              "missing serial device should fall back to default");
  expect_true(settings->baudrate == 115200,
              "invalid serial baudrate should fall back to default");
  expect_true(settings->bits == 8,
              "invalid serial bits should fall back to default");
  expect_true(settings->parity == SerialParity::none,
              "invalid serial parity should fall back to default");
  expect_true(settings->stop_bit == 1,
              "invalid serial stop bit should fall back to default");
  expect_true(settings->flow_control == SerialFlowControl::none,
              "invalid serial flow control should fall back to default");
  expect_true(settings->carrier_detect == SerialCarrierDetect::cd,
              "invalid serial carrier detect should fall back to default");

  expect_true(warnings_contain(result.warnings,
                               "invalid configuration value [serial] baudrate"),
              "invalid serial baudrate should emit a warning");
  expect_true(warnings_contain(result.warnings,
                               "invalid configuration value [serial] bits"),
              "invalid serial bits should emit a warning");
  expect_true(warnings_contain(result.warnings,
                               "invalid configuration value [serial] parity"),
              "invalid serial parity should emit a warning");
  expect_true(warnings_contain(result.warnings,
                               "invalid configuration value [serial] stop_bit"),
              "invalid serial stop bit should emit a warning");
  expect_true(
      warnings_contain(result.warnings,
                       "invalid configuration value [serial] flow_control"),
      "invalid serial flow control should emit a warning");
  expect_true(
      warnings_contain(result.warnings,
                       "invalid configuration value [serial] carrier_detect"),
      "invalid serial carrier detect should emit a warning");
  expect_true(warnings_contain(
                  result.warnings,
                  "missing required configuration value [serial] device"),
              "missing serial device should emit a warning");
}

static void test_public_setting_keys() {
  expect_true(general_type_setting_key().section == "general",
              "general type key should use the general section");
  expect_true(general_type_setting_key().name == "type",
              "general type key should use the type name");
  expect_true(terminal_width_setting_key().section == "terminal",
              "terminal width key should use the terminal section");
  expect_true(terminal_width_setting_key().name == "width",
              "terminal width key should use the width name");
  expect_true(terminal_height_setting_key().name == "height",
              "terminal height key should use the height name");
  expect_true(terminal_zoom_setting_key().name == "zoom",
              "terminal zoom key should use the zoom name");
  expect_true(terminal_auto_close_setting_key().name == "auto_close",
              "terminal auto_close key should use the auto_close name");
  expect_true(terminal_zoom_in_key_setting_key().section == "terminal" &&
                  terminal_zoom_in_key_setting_key().name == "zoom_in_key",
              "terminal zoom-in key should use [terminal] zoom_in_key");
  expect_true(terminal_zoom_out_key_setting_key().section == "terminal" &&
                  terminal_zoom_out_key_setting_key().name == "zoom_out_key",
              "terminal zoom-out key should use [terminal] zoom_out_key");
  expect_true(terminal_encoding_setting_key().section == "terminal" &&
                  terminal_encoding_setting_key().name == "encoding",
              "terminal encoding key should use [terminal] encoding");
  expect_true(terminal_backspace_code_setting_key().section == "terminal" &&
                  terminal_backspace_code_setting_key().name ==
                      "backspace_code",
              "Backspace code key should use [terminal] backspace_code");
  expect_true(terminal_cursor_key_mode_setting_key().section == "terminal" &&
                  terminal_cursor_key_mode_setting_key().name ==
                      "cursor_key_mode",
              "cursor-key mode key should use [terminal] cursor_key_mode");
  expect_true(telnet_address_setting_key().section == "telnet",
              "TELNET address key should use the telnet section");
  expect_true(telnet_address_setting_key().name == "address",
              "TELNET address key should use the address name");
  expect_true(telnet_port_setting_key().name == "port",
              "TELNET port key should use the port name");
  expect_true(serial_device_setting_key().section == "serial",
              "serial device key should use the serial section");
  expect_true(serial_device_setting_key().name == "device",
              "serial device key should use the device name");
  expect_true(serial_baudrate_setting_key().name == "baudrate",
              "serial baudrate key should use the baudrate name");
  expect_true(serial_bits_setting_key().name == "bits",
              "serial bits key should use the bits name");
  expect_true(serial_parity_setting_key().name == "parity",
              "serial parity key should use the parity name");
  expect_true(serial_stop_bit_setting_key().name == "stop_bit",
              "serial stop_bit key should use the stop_bit name");
  expect_true(serial_flow_control_setting_key().name == "flow_control",
              "serial flow_control key should use the flow_control name");
  expect_true(serial_carrier_detect_setting_key().name == "carrier_detect",
              "serial carrier_detect key should use the carrier_detect name");
  expect_true(transfer_base_path_setting_key().section == "transfer",
              "transfer base_path key should use the transfer section");
  expect_true(transfer_base_path_setting_key().name == "base_path",
              "transfer base_path key should use the base_path name");
  expect_true(
      transfer_text_send_bytes_per_second_setting_key().section == "transfer",
      "text send rate key should use the transfer section");
  expect_true(transfer_text_send_bytes_per_second_setting_key().name ==
                  "text_send_bytes_per_second",
              "text send rate key should use the requested name");
  expect_true(transfer_zmodem_autostart_setting_key().section == "transfer",
              "transfer zmodem_autostart key should use the transfer section");
  expect_true(transfer_zmodem_autostart_setting_key().name ==
                  "zmodem_autostart",
              "transfer zmodem_autostart key should use the requested name");
  expect_true(terminal_log_enabled_setting_key().section == "log" &&
                  terminal_log_enabled_setting_key().name == "enabled",
              "terminal log enabled key should use [log] enabled");
  expect_true(terminal_log_base_directory_setting_key().section == "log" &&
                  terminal_log_base_directory_setting_key().name ==
                      "base_directory",
              "terminal log base directory key should use [log] base_directory");
  expect_true(terminal_log_file_name_format_setting_key().section == "log" &&
                  terminal_log_file_name_format_setting_key().name ==
                      "file_name_format",
              "terminal log format key should use [log] file_name_format");
  expect_true(terminal_log_mode_setting_key().section == "log" &&
                  terminal_log_mode_setting_key().name == "mode",
              "terminal log mode key should use [log] mode");
}

static void test_save_terminal_log_settings() {
  const std::filesystem::path path =
      temporary_config_path("save-terminal-log");
  SettingsStore store =
      create_default_settings(default_terminal_display_settings(1.0));
  set_setting_value(&store, terminal_log_enabled_setting_key(),
                    elder_terms::SettingValue{true});
  set_setting_value(
      &store, terminal_log_base_directory_setting_key(),
      elder_terms::SettingValue{std::string("/var/log/elder-terms")});
  set_setting_value(
      &store, terminal_log_file_name_format_setting_key(),
      elder_terms::SettingValue{std::string("{YYYYMMDD}/session.txt")});
  set_setting_value(&store, terminal_log_mode_setting_key(),
                    elder_terms::SettingValue{std::string("cooked")});

  const SettingsSaveResult result = save_settings(store, path);
  expect_true(result.saved, "terminal log settings save should succeed");
  const std::string content = read_config(path);
  remove_config(path);

  expect_true(content.find("[log]") != std::string::npos,
              "saved settings should include non-default log section");
  expect_true(content.find("enabled=true") != std::string::npos,
              "saved settings should include enabled terminal logging");
  expect_true(content.find("base_directory=/var/log/elder-terms") !=
                  std::string::npos,
              "saved settings should include terminal log base directory");
  expect_true(content.find("file_name_format={YYYYMMDD}/session.txt") !=
                  std::string::npos,
              "saved settings should include terminal log file name format");
  expect_true(content.find("mode=cooked") != std::string::npos,
              "saved settings should include cooked terminal log mode");
}

static void test_save_settings_omits_default_values() {
  const std::filesystem::path path = temporary_config_path("save-values");
  SettingsStore store =
      create_default_settings(default_terminal_display_settings(1.0));
  set_setting_value(&store, general_type_setting_key(),
                    elder_terms::SettingValue{std::string("telnet")});
  set_setting_value(&store, terminal_width_setting_key(),
                    elder_terms::SettingValue{gint64{81}});
  set_setting_value(&store, terminal_height_setting_key(),
                    elder_terms::SettingValue{gint64{24}});
  set_setting_value(&store, terminal_zoom_setting_key(),
                    elder_terms::SettingValue{gdouble{1.0}});
  set_setting_value(&store, terminal_auto_close_setting_key(),
                    elder_terms::SettingValue{false});
  set_setting_value(&store, terminal_zoom_in_key_setting_key(),
                    elder_terms::SettingValue{std::string("alt+Up")});
  set_setting_value(&store, telnet_address_setting_key(),
                    elder_terms::SettingValue{std::string("host.example")});
  set_setting_value(&store, telnet_port_setting_key(),
                    elder_terms::SettingValue{gint64{23}});
  set_setting_value(
      &store, transfer_base_path_setting_key(),
      elder_terms::SettingValue{std::string("file:///tmp/downloads")});
  set_setting_value(&store, transfer_text_send_bytes_per_second_setting_key(),
                    elder_terms::SettingValue{gint64{2048}});

  const SettingsSaveResult result = save_settings(store, path);
  expect_true(result.saved, "settings save should succeed");
  const std::string content = read_config(path);
  remove_config(path);

  expect_true(content.find("[general]") != std::string::npos,
              "saved settings should include non-default general section");
  expect_true(content.find("type=telnet") != std::string::npos,
              "saved settings should include non-default connection type");
  expect_true(content.find("[terminal]") != std::string::npos,
              "saved settings should include non-default terminal section");
  expect_true(content.find("width=81") != std::string::npos,
              "saved settings should include non-default terminal width");
  expect_true(content.find("auto_close=false") != std::string::npos,
              "saved settings should include non-default auto-close");
  expect_true(content.find("zoom_in_key=alt+Up") != std::string::npos,
              "saved settings should include non-default zoom-in key");
  expect_true(content.find("[telnet]") != std::string::npos,
              "saved settings should include non-default TELNET section");
  expect_true(content.find("address=host.example") != std::string::npos,
              "saved settings should include non-default TELNET address");
  expect_true(content.find("[transfer]") != std::string::npos,
              "saved settings should include non-default transfer section");
  expect_true(content.find("base_path=file:///tmp/downloads") !=
                  std::string::npos,
              "saved settings should include non-default transfer base_path");
  expect_true(content.find("text_send_bytes_per_second=2048") !=
                  std::string::npos,
              "saved settings should include non-default text send rate");
  expect_true(content.find("height=") == std::string::npos,
              "saved settings should omit default terminal height");
  expect_true(content.find("zoom=") == std::string::npos,
              "saved settings should omit default terminal zoom");
  expect_true(content.find("zoom_out_key=") == std::string::npos,
              "saved settings should omit default zoom-out key");
  expect_true(content.find("port=") == std::string::npos,
              "saved settings should omit default TELNET port");
}

static void test_save_explicit_zmodem_autostart() {
  const std::filesystem::path false_path =
      temporary_config_path("save-zmodem-autostart-false");
  SettingsStore false_store =
      create_default_settings(default_terminal_display_settings(1.0));
  set_explicit_setting_value(
      &false_store, transfer_zmodem_autostart_setting_key(),
      elder_terms::SettingValue{false});

  const SettingsSaveResult false_result =
      save_settings(false_store, false_path);
  expect_true(false_result.saved,
              "explicit false ZMODEM auto-start save should succeed");
  const std::string false_content = read_config(false_path);
  remove_config(false_path);

  expect_true(false_content.find("[transfer]") != std::string::npos,
              "saved settings should include explicit transfer section");
  expect_true(false_content.find("zmodem_autostart=false") !=
                  std::string::npos,
              "saved settings should include explicit false ZMODEM auto-start");

  const std::filesystem::path true_path =
      temporary_config_path("save-zmodem-autostart-true");
  SettingsStore true_store =
      create_default_settings(default_terminal_display_settings(1.0));
  set_explicit_setting_value(
      &true_store, transfer_zmodem_autostart_setting_key(),
      elder_terms::SettingValue{true});

  const SettingsSaveResult true_result = save_settings(true_store, true_path);
  expect_true(true_result.saved,
              "explicit true ZMODEM auto-start save should succeed");
  const std::string true_content = read_config(true_path);
  remove_config(true_path);

  expect_true(true_content.find("zmodem_autostart=true") != std::string::npos,
              "saved settings should include explicit true ZMODEM auto-start");
}

static void test_save_explicit_terminal_text_defaults() {
  const std::filesystem::path path =
      temporary_config_path("save-terminal-text-defaults");
  SettingsStore store =
      create_default_settings(default_terminal_display_settings(1.0));
  set_explicit_setting_value(
      &store, terminal_encoding_setting_key(),
      elder_terms::SettingValue{std::string("UTF-8")});
  set_explicit_setting_value(
      &store, terminal_backspace_code_setting_key(),
      elder_terms::SettingValue{std::string("del")});
  set_explicit_setting_value(
      &store, terminal_cursor_key_mode_setting_key(),
      elder_terms::SettingValue{std::string("normal")});

  const SettingsSaveResult result = save_settings(store, path);
  expect_true(result.saved,
              "explicit terminal text default save should succeed");
  const std::string content = read_config(path);

  expect_true(content.find("encoding=UTF-8") != std::string::npos,
              "explicit UTF-8 should be persisted");
  expect_true(content.find("backspace_code=del") != std::string::npos,
              "explicit local Backspace default should be persisted");
  expect_true(content.find("cursor_key_mode=normal") != std::string::npos,
              "explicit local cursor-key default should be persisted");

  const SettingsLoadResult loaded = load_settings(
      SettingsLoadOptions{
          .config_path = std::optional<std::filesystem::path>{path},
          .startup_config_path = std::nullopt,
      },
      1.0);
  remove_config(path);
  SettingsStore changed_type = loaded.store;
  set_setting_value(&changed_type, general_type_setting_key(),
                    elder_terms::SettingValue{std::string("serial")});
  const TerminalTextSettings text =
      terminal_connection_profile(changed_type).text_settings;
  expect_true(text.encoding == "UTF-8" &&
                  text.backspace_code == TerminalBackspaceCode::del &&
                  text.cursor_key_mode == TerminalCursorKeyMode::normal,
              "loaded explicit values should remain explicit after type change");
}

static void test_save_serial_settings_omits_default_values() {
  const std::filesystem::path path = temporary_config_path("save-serial-values");
  SettingsStore store =
      create_default_settings(default_terminal_display_settings(1.0));
  set_setting_value(&store, general_type_setting_key(),
                    elder_terms::SettingValue{std::string("serial")});
  set_setting_value(&store, serial_device_setting_key(),
                    elder_terms::SettingValue{std::string("/dev/ttyUSB0")});
  set_setting_value(&store, serial_baudrate_setting_key(),
                    elder_terms::SettingValue{gint64{57600}});
  set_setting_value(&store, serial_bits_setting_key(),
                    elder_terms::SettingValue{gint64{7}});
  set_setting_value(&store, serial_parity_setting_key(),
                    elder_terms::SettingValue{std::string("e")});
  set_setting_value(&store, serial_stop_bit_setting_key(),
                    elder_terms::SettingValue{gint64{2}});
  set_setting_value(&store, serial_flow_control_setting_key(),
                    elder_terms::SettingValue{std::string("hard")});
  set_setting_value(&store, serial_carrier_detect_setting_key(),
                    elder_terms::SettingValue{std::string("cts")});

  const SettingsSaveResult result = save_settings(store, path);
  expect_true(result.saved, "serial settings save should succeed");
  const std::string content = read_config(path);
  remove_config(path);

  expect_true(content.find("type=serial") != std::string::npos,
              "saved settings should include non-default serial type");
  expect_true(content.find("[serial]") != std::string::npos,
              "saved settings should include non-default serial section");
  expect_true(content.find("device=/dev/ttyUSB0") != std::string::npos,
              "saved settings should include non-default serial device");
  expect_true(content.find("baudrate=57600") != std::string::npos,
              "saved settings should include non-default serial baudrate");
  expect_true(content.find("bits=7") != std::string::npos,
              "saved settings should include non-default serial bits");
  expect_true(content.find("parity=e") != std::string::npos,
              "saved settings should include non-default serial parity");
  expect_true(content.find("stop_bit=2") != std::string::npos,
              "saved settings should include non-default serial stop bit");
  expect_true(content.find("flow_control=hard") != std::string::npos,
              "saved settings should include non-default serial flow control");
  expect_true(content.find("carrier_detect=cts") != std::string::npos,
              "saved settings should include non-default serial carrier detect");
}

static void test_save_settings_writes_empty_file_for_defaults() {
  const std::filesystem::path path = temporary_config_path("save-defaults");
  const SettingsStore store =
      create_default_settings(default_terminal_display_settings(1.0));

  const SettingsSaveResult result = save_settings(store, path);
  expect_true(result.saved, "default settings save should succeed");
  const std::string content = read_config(path);
  remove_config(path);

  expect_true(content.empty(), "default-only settings should save an empty INI");
}

static void test_load_settings_reports_file_read_status() {
  const std::filesystem::path missing =
      temporary_config_path("missing-load-status");
  const SettingsLoadResult missing_result = load_settings(
      SettingsLoadOptions{
          .config_path = missing,
          .startup_config_path = std::nullopt,
      },
      1.0);
  expect_true(!missing_result.loaded,
              "missing requested settings should report load failure");

  const std::filesystem::path valid =
      temporary_config_path("valid-load-status");
  write_config(valid, "[terminal]\nwidth=91\n");
  const SettingsLoadResult valid_result = load_settings(
      SettingsLoadOptions{
          .config_path = valid,
          .startup_config_path = std::nullopt,
      },
      1.0);
  remove_config(valid);
  expect_true(valid_result.loaded,
              "readable requested settings should report success");
  expect_true(terminal_display_settings(valid_result.store).width == 91,
              "successfully loaded settings should retain their value");
}

} // namespace elder_terms_settings_test

int main() {
  try {
    elder_terms_settings_test::test_default_settings();
    elder_terms_settings_test::test_terminal_text_defaults_follow_connection_type();
    elder_terms_settings_test::test_terminal_text_explicit_settings_override_connection_defaults();
    elder_terms_settings_test::test_terminal_encoding_choices_are_supported();
    elder_terms_settings_test::test_terminal_log_settings();
    elder_terms_settings_test::test_terminal_log_file_name_format_validation();
    elder_terms_settings_test::test_key_binding_parser_uses_exact_modifiers();
    elder_terms_settings_test::test_terminal_key_binding_configuration();
    elder_terms_settings_test::test_telnet_profile();
    elder_terms_settings_test::test_serial_profile();
    elder_terms_settings_test::test_transfer_base_path_setting();
    elder_terms_settings_test::test_transfer_text_send_bytes_per_second_setting();
    elder_terms_settings_test::test_transfer_zmodem_autostart_setting();
    elder_terms_settings_test::test_invalid_values_fall_back_to_defaults();
    elder_terms_settings_test::test_invalid_terminal_text_values_fall_back_to_type_defaults();
    elder_terms_settings_test::test_invalid_terminal_log_values_fall_back_to_defaults();
    elder_terms_settings_test::test_invalid_serial_values_fall_back_to_defaults();
    elder_terms_settings_test::test_public_setting_keys();
    elder_terms_settings_test::test_save_settings_omits_default_values();
    elder_terms_settings_test::test_save_serial_settings_omits_default_values();
    elder_terms_settings_test::test_save_explicit_zmodem_autostart();
    elder_terms_settings_test::test_save_explicit_terminal_text_defaults();
    elder_terms_settings_test::test_save_terminal_log_settings();
    elder_terms_settings_test::test_save_settings_writes_empty_file_for_defaults();
    elder_terms_settings_test::test_load_settings_reports_file_read_status();
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
