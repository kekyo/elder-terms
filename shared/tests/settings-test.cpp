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
using elder_terms::application_open_hotkey;
using elder_terms::application_open_hotkey_setting_key;
using elder_terms::application_startup_mode;
using elder_terms::application_startup_mode_setting_key;
using elder_terms::application_ui_language;
using elder_terms::application_ui_language_setting_key;
using elder_terms::application_ui_language_to_string;
using elder_terms::ApplicationUiLanguage;
using elder_terms::default_global_config_path;
using elder_terms::default_terminal_text_settings;
using elder_terms::default_terminal_display_settings;
using elder_terms::general_open_connection_hotkey;
using elder_terms::general_open_connection_hotkey_setting_key;
using elder_terms::general_open_connection_hotkey_text;
using elder_terms::GeneralColorSettings;
using elder_terms::general_background_setting_key;
using elder_terms::general_color_settings;
using elder_terms::general_exterior_background_setting_key;
using elder_terms::general_type_setting_key;
using elder_terms::key_binding_matches;
using elder_terms::parse_key_binding;
using elder_terms::load_global_settings;
using elder_terms::load_application_ui_language_preference;
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
using elder_terms::save_global_settings;
using elder_terms::SerialCarrierDetect;
using elder_terms::SerialConnectionSettings;
using elder_terms::SerialDeviceMatchMode;
using elder_terms::SerialFlowControl;
using elder_terms::SerialParity;
using elder_terms::serial_baudrate_setting_key;
using elder_terms::serial_bits_setting_key;
using elder_terms::serial_carrier_detect_setting_key;
using elder_terms::serial_connection_settings;
using elder_terms::serial_device_setting_key;
using elder_terms::serial_device_match_mode_setting_key;
using elder_terms::serial_device_usb_serial_setting_key;
using elder_terms::serial_flow_control_setting_key;
using elder_terms::serial_parity_setting_key;
using elder_terms::serial_stop_bit_setting_key;
using elder_terms::set_explicit_setting_value;
using elder_terms::set_setting_value;
using elder_terms::SettingsLoadOptions;
using elder_terms::SettingsLoadResult;
using elder_terms::SettingsSaveResult;
using elder_terms::SettingsStore;
using elder_terms::SettingValueSource;
using elder_terms::StartupMode;
using elder_terms::SshConnectionSettings;
using elder_terms::ssh_address_setting_key;
using elder_terms::ssh_connection_settings;
using elder_terms::ssh_identity_file_setting_key;
using elder_terms::ssh_port_setting_key;
using elder_terms::ssh_terminal_type_setting_key;
using elder_terms::ssh_username_setting_key;
using elder_terms::TelnetConnectionSettings;
using elder_terms::telnet_address_setting_key;
using elder_terms::telnet_port_setting_key;
using elder_terms::telnet_terminal_type_setting_key;
using elder_terms::terminal_auto_close_setting_key;
using elder_terms::TerminalBackspaceCode;
using elder_terms::TerminalConnectionKind;
using elder_terms::TerminalConnectionProfile;
using elder_terms::TerminalCursorKeyMode;
using elder_terms::TerminalDisplaySettings;
using elder_terms::TerminalFontFamilies;
using elder_terms::TerminalReturnCode;
using elder_terms::TerminalTextSettings;
using elder_terms::terminal_auto_close;
using elder_terms::terminal_backspace_code_setting_key;
using elder_terms::terminal_connection_profile;
using elder_terms::terminal_cursor_key_mode_setting_key;
using elder_terms::terminal_cursor_key_mode_to_string;
using elder_terms::terminal_display_settings;
using elder_terms::terminal_encoding_choices;
using elder_terms::terminal_encoding_name_is_valid;
using elder_terms::terminal_encoding_setting_key;
using elder_terms::terminal_font_fallback_family_setting_key;
using elder_terms::terminal_font_families;
using elder_terms::terminal_font_primary_family_setting_key;
using elder_terms::terminal_height_setting_key;
using elder_terms::terminal_scrollback_lines_setting_key;
using elder_terms::terminal_show_border;
using elder_terms::terminal_show_border_setting_key;
using elder_terms::terminal_return_code_setting_key;
using elder_terms::terminal_return_code_to_string;
using elder_terms::terminal_width_setting_key;
using elder_terms::terminal_zoom_setting_key;
using elder_terms::terminal_key_bindings;
using elder_terms::terminal_send_break_key;
using elder_terms::terminal_send_break_key_setting_key;
using elder_terms::terminal_zoom_in_key_setting_key;
using elder_terms::terminal_zoom_out_key_setting_key;
using elder_terms::transfer_base_path;
using elder_terms::transfer_base_path_setting_key;
using elder_terms::transfer_text_send_bytes_per_second;
using elder_terms::transfer_text_send_bytes_per_second_setting_key;
using elder_terms::transfer_text_send_follow_return_code;
using elder_terms::transfer_text_send_follow_return_code_setting_key;
using elder_terms::transfer_zmodem_autostart;
using elder_terms::transfer_zmodem_autostart_setting_key;
using elder_terms::rebase_settings_store_fallbacks;
using elder_terms::setting_fallback_source;
using elder_terms::setting_fallback_value;
using elder_terms::setting_has_configured_value;
using elder_terms::setting_has_explicit_value;
using elder_terms::setting_is_dirty;
using elder_terms::setting_value_source;

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

static TerminalConnectionProfile
required_terminal_connection_profile(const SettingsStore &store) {
  const std::optional<TerminalConnectionProfile> profile =
      terminal_connection_profile(store);
  expect_true(profile.has_value(),
              "terminal settings should produce a terminal profile");
  return profile.value();
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
      create_default_settings(default_terminal_display_settings(1.2),
                              "elder-terms");
  const TerminalDisplaySettings display = terminal_display_settings(store);
  expect_true(display.width == 80, "default terminal width should be 80");
  expect_true(display.height == 24, "default terminal height should be 24");
  expect_true(display.scrollback_lines == 10000,
              "default terminal scrollback should retain 10000 lines");
  expect_true(display.zoom == 1.2, "default terminal zoom should be retained");
  const TerminalFontFamilies fonts = terminal_font_families(store);
  expect_true(fonts.primary_family ==
                  std::optional<std::string>{"Noto Sans Mono"},
              "the default primary terminal font should be Noto Sans Mono");
  expect_true(fonts.fallback_family ==
                  std::optional<std::string>{"Monospace"},
              "the default fallback terminal font should be Monospace");
  expect_true(terminal_auto_close(store),
              "default terminal auto-close should be enabled");
  expect_true(!terminal_show_border(store),
              "terminal window side borders should be disabled by default");
  const GeneralColorSettings colors = general_color_settings(store);
  expect_true(!colors.exterior_background.has_value(),
              "default exterior background should remain theme-controlled");
  expect_true(!colors.background.has_value(),
              "default content background should remain toolkit-controlled");
  const auto key_bindings = terminal_key_bindings(store);
  expect_true(key_bindings.zoom_in.has_value(),
              "default terminal zoom-in key should be enabled");
  expect_true(key_bindings.zoom_out.has_value(),
              "default terminal zoom-out key should be enabled");
  expect_true(!key_bindings.send_break.has_value(),
              "default terminal BREAK key should be disabled");
  expect_true(key_binding_matches(*key_bindings.zoom_in, GDK_KEY_equal,
                                  GDK_CONTROL_MASK),
              "default terminal zoom-in key should be Ctrl+equal");
  expect_true(key_binding_matches(*key_bindings.zoom_out, GDK_KEY_minus,
                                  GDK_CONTROL_MASK),
              "default terminal zoom-out key should be Ctrl+minus");
  expect_true(terminal_send_break_key(store).empty(),
              "default terminal BREAK key text should be empty");
  expect_true(transfer_base_path(store).empty(),
              "default transfer base path should be empty");
  expect_true(transfer_text_send_bytes_per_second(store) == 1024,
              "default text send rate should be 1024 bytes per second");
  expect_true(transfer_text_send_follow_return_code(store),
              "text send should follow the Return code by default");
  expect_true(!transfer_zmodem_autostart(store),
              "default local transfer ZMODEM auto-start should be disabled");
  const TerminalLogSettings log = terminal_log_settings(store);
  expect_true(!log.enabled,
              "terminal logging should be disabled by default");
  expect_true(log.base_directory == "${documents}/logs/",
              "default terminal log base directory should use XDG "
              "Documents/logs");
  expect_true(log.file_name_format ==
                  "${YYYYMMDD}_${hhmmss}_${fff}.txt",
              "default terminal log file name format should include milliseconds");
  expect_true(log.mode == TerminalLogMode::raw,
              "default terminal log mode should preserve raw bytes");
  expect_true(log.connection_name == "elder-terms",
              "default terminal log settings should retain the effective "
              "connection name");
  const TelnetConnectionSettings telnet =
      elder_terms::telnet_connection_settings(store);
  expect_true(telnet.terminal_type == "xterm-256color",
              "default TELNET terminal type should match gtk-oldtype");
  const SshConnectionSettings ssh = ssh_connection_settings(store);
  expect_true(ssh.endpoint.address.empty(),
              "default SSH address should be empty");
  expect_true(ssh.endpoint.port == 22, "default SSH port should be 22");
  expect_true(ssh.endpoint.username.empty(),
              "default SSH username should defer to the local user");
  expect_true(ssh.endpoint.identity_file.empty(),
              "default SSH identity should use automatic discovery");
  expect_true(ssh.terminal_type == "xterm-256color",
              "default SSH terminal type should be xterm-256color");
  const elder_terms::SftpConnectionSettings sftp =
      elder_terms::sftp_connection_settings(store);
  expect_true(sftp.local_directory.empty(),
              "default SFTP local directory should use a runtime fallback");
  expect_true(sftp.remote_directory == ".",
              "default SFTP remote directory should use the login directory");
  expect_true(elder_terms::general_connection_kind(store) ==
                  elder_terms::ConnectionKind::local_shell,
              "default general connection kind should be local shell");

  const TerminalConnectionProfile profile =
      required_terminal_connection_profile(store);
  expect_true(profile.name == "elder-terms",
              "default connection profile should retain its effective name");
  expect_true(profile.kind == TerminalConnectionKind::local_shell,
              "default connection should be local shell");
  expect_true(std::holds_alternative<LocalShellConnectionSettings>(
                  profile.settings),
              "default connection settings should be local shell settings");
  expect_true(profile.text_settings.encoding == "UTF-8",
              "default local terminal encoding should be UTF-8");
  expect_true(elder_terms::setting_string_value_or_default(
                  store, terminal_backspace_code_setting_key(), "") ==
                  "auto",
              "built-in terminal Backspace setting should be Auto");
  expect_true(profile.text_settings.backspace_code ==
                  TerminalBackspaceCode::automatic,
              "default local Backspace should use automatic binding");
  expect_true(profile.text_settings.cursor_key_mode ==
                  TerminalCursorKeyMode::normal,
              "default local cursor keys should use normal sequences");
}

static SettingsLoadResult
load_terminal_scrollback_lines(const std::string &name,
                               const std::string &value) {
  const std::filesystem::path path = temporary_config_path(name);
  write_config(path, "[terminal]\nscrollback_lines=" + value + "\n");
  const SettingsLoadResult result = load_settings(
      SettingsLoadOptions{
          .config_path = path,
          .startup_config_path = std::nullopt,
      },
      1.0);
  remove_config(path);
  return result;
}

static void test_terminal_scrollback_lines_range_and_round_trip() {
  const SettingsLoadResult minimum =
      load_terminal_scrollback_lines("scrollback-minimum", "1000");
  expect_true(terminal_display_settings(minimum.store).scrollback_lines ==
                  1000,
              "minimum terminal scrollback should be accepted");
  expect_true(minimum.warnings.empty(),
              "minimum terminal scrollback should not emit warnings");

  const SettingsLoadResult maximum =
      load_terminal_scrollback_lines("scrollback-maximum", "100000");
  expect_true(terminal_display_settings(maximum.store).scrollback_lines ==
                  100000,
              "maximum terminal scrollback should be accepted");
  expect_true(maximum.warnings.empty(),
              "maximum terminal scrollback should not emit warnings");

  const SettingsLoadResult below_minimum =
      load_terminal_scrollback_lines("scrollback-below-minimum", "999");
  expect_true(
      terminal_display_settings(below_minimum.store).scrollback_lines ==
          10000,
      "terminal scrollback below the minimum should use the default");
  expect_true(warnings_contain(
                  below_minimum.warnings,
                  "invalid configuration value [terminal] scrollback_lines"),
              "terminal scrollback below the minimum should emit a warning");

  const SettingsLoadResult above_maximum =
      load_terminal_scrollback_lines("scrollback-above-maximum", "100001");
  expect_true(
      terminal_display_settings(above_maximum.store).scrollback_lines ==
          10000,
      "terminal scrollback above the maximum should use the default");
  expect_true(warnings_contain(
                  above_maximum.warnings,
                  "invalid configuration value [terminal] scrollback_lines"),
              "terminal scrollback above the maximum should emit a warning");

  const std::filesystem::path saved_path =
      temporary_config_path("scrollback-round-trip");
  SettingsStore configured = minimum.store;
  expect_true(set_explicit_setting_value(
                  &configured, terminal_scrollback_lines_setting_key(),
                  elder_terms::SettingValue{gint64{54321}}),
              "valid terminal scrollback should be accepted in memory");
  const SettingsSaveResult saved = save_settings(configured, saved_path);
  expect_true(saved.saved, "terminal scrollback settings should save");
  expect_true(read_config(saved_path).find("scrollback_lines=54321") !=
                  std::string::npos,
              "saved settings should include terminal scrollback lines");
  const SettingsLoadResult reloaded = load_settings(
      SettingsLoadOptions{
          .config_path = saved_path,
          .startup_config_path = std::nullopt,
      },
      1.0);
  remove_config(saved_path);
  expect_true(terminal_display_settings(reloaded.store).scrollback_lines ==
                  54321,
              "terminal scrollback should survive saving and reloading");
}

static void test_terminal_font_family_settings_round_trip_and_layering() {
  const std::filesystem::path global_path =
      temporary_config_path("global-terminal-fonts");
  const std::filesystem::path connection_path =
      temporary_config_path("connection-terminal-fonts");
  write_config(global_path,
               "[terminal]\n"
               "font_primary_family=Global Latin\n"
               "font_fallback_family=Global CJK\n");
  write_config(connection_path,
               "[terminal]\n"
               "font_primary_family=Connection Latin\n");

  SettingsLoadResult loaded = load_settings(
      SettingsLoadOptions{
          .config_path = connection_path,
          .startup_config_path = std::nullopt,
          .global_config_path = global_path,
      },
      1.0);
  remove_config(global_path);

  TerminalFontFamilies fonts = terminal_font_families(loaded.store);
  expect_true(fonts.primary_family ==
                  std::optional<std::string>{"Connection Latin"},
              "a connection primary font should override the global font");
  expect_true(fonts.fallback_family ==
                  std::optional<std::string>{"Global CJK"},
              "an unspecified fallback font should inherit the global font");
  expect_true(setting_value_source(
                  loaded.store,
                  terminal_font_primary_family_setting_key()) ==
                  SettingValueSource::override,
              "the connection primary font should report an override source");
  expect_true(setting_value_source(
                  loaded.store,
                  terminal_font_fallback_family_setting_key()) ==
                  SettingValueSource::global,
              "the inherited fallback font should report a global source");

  expect_true(
      set_explicit_setting_value(
          &loaded.store, terminal_font_fallback_family_setting_key(),
          elder_terms::SettingValue{std::string("Connection CJK")}),
      "a fallback font family override should be accepted");
  const SettingsSaveResult save_result =
      save_settings(loaded.store, connection_path);
  expect_true(save_result.saved, "terminal font families should save");
  const std::string content = read_config(connection_path);
  remove_config(connection_path);
  expect_true(content.find("font_primary_family=Connection Latin") !=
                  std::string::npos,
              "the primary font family should be persisted");
  expect_true(content.find("font_fallback_family=Connection CJK") !=
                  std::string::npos,
              "the fallback font family should be persisted");
}

static void test_terminal_font_family_defaults_override_global_fonts() {
  const std::filesystem::path global_path =
      temporary_config_path("global-terminal-font-defaults");
  const std::filesystem::path connection_path =
      temporary_config_path("connection-terminal-font-defaults");
  write_config(global_path,
               "[terminal]\n"
               "font_primary_family=Global Latin\n"
               "font_fallback_family=Global CJK\n");
  write_config(connection_path,
               "[terminal]\n"
               "font_primary_family=default\n"
               "font_fallback_family=default\n");

  const SettingsLoadResult loaded = load_settings(
      SettingsLoadOptions{
          .config_path = connection_path,
          .startup_config_path = std::nullopt,
          .global_config_path = global_path,
      },
      1.0);
  remove_config(global_path);

  const TerminalFontFamilies fonts = terminal_font_families(loaded.store);
  expect_true(fonts.primary_family ==
                  std::optional<std::string>{"Noto Sans Mono"},
              "an explicit default should restore the built-in primary font");
  expect_true(fonts.fallback_family ==
                  std::optional<std::string>{"Monospace"},
              "an explicit default should restore the built-in fallback font");
  expect_true(setting_value_source(
                  loaded.store,
                  terminal_font_primary_family_setting_key()) ==
                  SettingValueSource::override,
              "the default primary font should remain an override");
  expect_true(setting_value_source(
                  loaded.store,
                  terminal_font_fallback_family_setting_key()) ==
                  SettingValueSource::override,
              "the default fallback font should remain an override");

  const SettingsSaveResult save_result =
      save_settings(loaded.store, connection_path);
  expect_true(save_result.saved,
              "explicit terminal font defaults should save");
  const std::string content = read_config(connection_path);
  remove_config(connection_path);
  expect_true(content.find("font_primary_family=default") !=
                  std::string::npos,
              "the default primary font should be persisted");
  expect_true(content.find("font_fallback_family=default") !=
                  std::string::npos,
              "the default fallback font should be persisted");
}

static void test_general_color_settings() {
  const std::filesystem::path path =
      temporary_config_path("general-colors");
  write_config(path,
               "[general]\n"
               "exterior_background=#12aBcF\n"
               "background=#001122\n");

  const SettingsLoadResult result = load_settings(
      SettingsLoadOptions{
          .config_path = path,
          .startup_config_path = std::nullopt,
      },
      1.0);
  remove_config(path);

  const GeneralColorSettings colors = general_color_settings(result.store);
  expect_true(colors.exterior_background.has_value(),
              "an RGB exterior color should be parsed");
  expect_true(colors.exterior_background->red == 0x12 &&
                  colors.exterior_background->green == 0xab &&
                  colors.exterior_background->blue == 0xcf,
              "the exterior color should retain all RGB channels");
  expect_true(colors.background.has_value(),
              "an RGB content color should be parsed");
  expect_true(colors.background->red == 0x00 &&
                  colors.background->green == 0x11 &&
                  colors.background->blue == 0x22,
              "the content color should retain all RGB channels");
}

static void test_general_color_none_overrides_global_colors() {
  const std::filesystem::path global_path =
      temporary_config_path("global-general-colors");
  const std::filesystem::path config_path =
      temporary_config_path("connection-general-colors");
  write_config(global_path,
               "[general]\n"
               "exterior_background=#102030\n"
               "background=#405060\n");
  write_config(config_path,
               "[general]\n"
               "exterior_background=none\n"
               "background=#708090\n");

  const SettingsLoadResult result = load_settings(
      SettingsLoadOptions{
          .config_path = config_path,
          .startup_config_path = std::nullopt,
          .global_config_path = global_path,
      },
      1.0);
  remove_config(global_path);
  remove_config(config_path);

  const GeneralColorSettings colors = general_color_settings(result.store);
  expect_true(!colors.exterior_background.has_value(),
              "an explicit none should suppress the inherited exterior color");
  expect_true(colors.background.has_value() &&
                  colors.background->red == 0x70 &&
                  colors.background->green == 0x80 &&
                  colors.background->blue == 0x90,
              "a connection content color should override the global color");
  expect_true(setting_value_source(
                  result.store,
                  general_exterior_background_setting_key()) ==
                  SettingValueSource::override,
              "an explicit none should remain a connection override");
}

static void test_terminal_type_defaults_follow_background_color() {
  SettingsStore store =
      create_default_settings(default_terminal_display_settings(1.0),
                              "elder-terms");
  expect_true(
      elder_terms::telnet_connection_settings(store).terminal_type ==
          "xterm-256color" &&
          ssh_connection_settings(store).terminal_type ==
              "xterm-256color",
      "terminal connections without a background should use "
      "xterm-256color");

  set_explicit_setting_value(
      &store, general_background_setting_key(),
      elder_terms::SettingValue{std::string("#102030")});
  expect_true(
      elder_terms::telnet_connection_settings(store).terminal_type ==
          "xterm" &&
          ssh_connection_settings(store).terminal_type == "xterm",
      "terminal connections with an RGB background should use xterm");

  set_explicit_setting_value(
      &store, general_type_setting_key(),
      elder_terms::SettingValue{std::string("telnet")});
  TerminalConnectionProfile profile =
      required_terminal_connection_profile(store);
  const auto *telnet =
      std::get_if<TelnetConnectionSettings>(&profile.settings);
  expect_true(telnet != nullptr && telnet->terminal_type == "xterm",
              "the TELNET connection profile should receive the "
              "background-dependent terminal type");

  set_explicit_setting_value(
      &store, general_type_setting_key(),
      elder_terms::SettingValue{std::string("ssh")});
  profile = required_terminal_connection_profile(store);
  const auto *ssh =
      std::get_if<SshConnectionSettings>(&profile.settings);
  expect_true(ssh != nullptr && ssh->terminal_type == "xterm",
              "the SSH connection profile should receive the "
              "background-dependent terminal type");

  set_explicit_setting_value(
      &store, general_background_setting_key(),
      elder_terms::SettingValue{std::string("none")});
  expect_true(
      elder_terms::telnet_connection_settings(store).terminal_type ==
          "xterm-256color" &&
          ssh_connection_settings(store).terminal_type ==
              "xterm-256color",
      "changing the background to none should restore xterm-256color");

  set_explicit_setting_value(
      &store, general_background_setting_key(),
      elder_terms::SettingValue{std::string("#405060")});
  expect_true(
      elder_terms::telnet_connection_settings(store).terminal_type ==
          "xterm" &&
          ssh_connection_settings(store).terminal_type == "xterm",
      "changing the background back to RGB should restore xterm");

  const std::filesystem::path save_path =
      temporary_config_path("background-terminal-type-default");
  const SettingsSaveResult save_result = save_settings(store, save_path);
  const std::string saved = read_config(save_path);
  remove_config(save_path);
  expect_true(save_result.saved,
              "background-dependent terminal defaults should remain "
              "saveable");
  expect_true(saved.find("terminal_type=") == std::string::npos,
              "a background-dependent terminal default should not be "
              "serialized as an explicit setting");
}

static void test_configured_terminal_types_override_background_default() {
  const std::filesystem::path global_path =
      temporary_config_path("global-terminal-type-default");
  const std::filesystem::path connection_path =
      temporary_config_path("connection-terminal-type-default");
  write_config(global_path,
               "[general]\n"
               "background=#102030\n"
               "\n"
               "[telnet]\n"
               "terminal_type=vt220\n"
               "\n"
               "[ssh]\n"
               "terminal_type=ansi\n");
  write_config(connection_path,
               "[general]\n"
               "type=telnet\n"
               "background=#405060\n"
               "\n"
               "[telnet]\n"
               "terminal_type=screen\n");

  SettingsLoadResult result = load_settings(
      SettingsLoadOptions{
          .config_path = connection_path,
          .startup_config_path = std::nullopt,
          .global_config_path = global_path,
      },
      1.0);
  expect_true(
      elder_terms::telnet_connection_settings(result.store).terminal_type ==
          "screen",
      "an explicit TELNET terminal type should override the RGB background "
      "default");
  expect_true(
      ssh_connection_settings(result.store).terminal_type == "ansi",
      "a global SSH terminal type should override the RGB background "
      "default");

  write_config(connection_path,
               "[general]\n"
               "type=telnet\n"
               "background=none\n");
  result = load_settings(
      SettingsLoadOptions{
          .config_path = connection_path,
          .startup_config_path = std::nullopt,
          .global_config_path = global_path,
      },
      1.0);
  remove_config(global_path);
  remove_config(connection_path);

  expect_true(
      elder_terms::telnet_connection_settings(result.store).terminal_type ==
          "vt220" &&
          ssh_connection_settings(result.store).terminal_type == "ansi",
      "configured terminal types should remain active when a connection "
      "overrides a global RGB background with none");

  const std::filesystem::path color_only_global_path =
      temporary_config_path("global-background-terminal-type-default");
  const std::filesystem::path no_color_connection_path =
      temporary_config_path("connection-no-background-terminal-type-default");
  write_config(color_only_global_path,
               "[general]\n"
               "background=#708090\n");
  write_config(no_color_connection_path,
               "[general]\n"
               "type=ssh\n");
  const SettingsLoadResult inherited_color = load_settings(
      SettingsLoadOptions{
          .config_path = no_color_connection_path,
          .startup_config_path = std::nullopt,
          .global_config_path = color_only_global_path,
      },
      1.0);
  expect_true(
      elder_terms::telnet_connection_settings(inherited_color.store)
                  .terminal_type == "xterm" &&
          ssh_connection_settings(inherited_color.store).terminal_type ==
              "xterm",
      "a global RGB background should select the xterm built-in default");

  write_config(no_color_connection_path,
               "[general]\n"
               "type=ssh\n"
               "background=none\n");
  const SettingsLoadResult no_color = load_settings(
      SettingsLoadOptions{
          .config_path = no_color_connection_path,
          .startup_config_path = std::nullopt,
          .global_config_path = color_only_global_path,
      },
      1.0);
  remove_config(color_only_global_path);
  remove_config(no_color_connection_path);

  expect_true(
      elder_terms::telnet_connection_settings(no_color.store).terminal_type ==
          "xterm-256color" &&
          ssh_connection_settings(no_color.store).terminal_type ==
              "xterm-256color",
      "a connection background of none should suppress the global RGB "
      "terminal type default");
}

static void test_invalid_general_colors_fall_back_and_warn() {
  const std::filesystem::path global_path =
      temporary_config_path("valid-global-general-colors");
  const std::filesystem::path config_path =
      temporary_config_path("invalid-connection-general-colors");
  write_config(global_path,
               "[general]\n"
               "exterior_background=#112233\n");
  write_config(config_path,
               "[general]\n"
               "exterior_background=#44556677\n"
               "background=112233\n");

  const SettingsLoadResult result = load_settings(
      SettingsLoadOptions{
          .config_path = config_path,
          .startup_config_path = std::nullopt,
          .global_config_path = global_path,
      },
      1.0);
  remove_config(global_path);
  remove_config(config_path);

  const GeneralColorSettings colors = general_color_settings(result.store);
  expect_true(colors.exterior_background.has_value() &&
                  colors.exterior_background->red == 0x11 &&
                  colors.exterior_background->green == 0x22 &&
                  colors.exterior_background->blue == 0x33,
              "an ARGB connection color should fall back to the global RGB "
              "color");
  expect_true(!colors.background.has_value(),
              "an invalid content color should use the no-color default");
  expect_true(
      warnings_contain(
          result.warnings,
          "invalid configuration value [general] exterior_background"),
      "an ARGB exterior color should emit a warning");
  expect_true(
      warnings_contain(
          result.warnings,
          "invalid configuration value [general] background"),
      "a content color without # should emit a warning");
}

static void test_connection_name_settings() {
  const auto connection_name = [](const SettingsStore &store) {
    return elder_terms::general_connection_name(store);
  };

  const SettingsStore defaults =
      create_default_settings(default_terminal_display_settings(1.0),
                              "elder-terms");
  expect_true(connection_name(defaults) == "elder-terms",
              "settings without an INI path should use the application name");

  const std::filesystem::path explicit_path =
      temporary_config_path("explicit-connection-name");
  write_config(explicit_path, "[general]\nname=Tokyo / Lab\n");
  const SettingsLoadResult explicit_result = load_settings(
      SettingsLoadOptions{
          .config_path = explicit_path,
          .startup_config_path = std::nullopt,
      },
      1.0);
  remove_config(explicit_path);
  expect_true(connection_name(explicit_result.store) == "Tokyo / Lab",
              "an explicit general name should be retained without path-name "
              "restrictions");

  const std::filesystem::path fallback_path =
      temporary_config_path("fallback-connection-name");
  write_config(fallback_path, "[general]\ntype=local\n");
  const SettingsLoadResult fallback_result = load_settings(
      SettingsLoadOptions{
          .config_path = fallback_path,
          .startup_config_path = std::nullopt,
      },
      1.0);
  remove_config(fallback_path);
  expect_true(connection_name(fallback_result.store) ==
                  fallback_path.stem().string(),
              "a missing general name should use the persistent INI stem");

  const std::filesystem::path empty_path =
      temporary_config_path("empty-connection-name");
  write_config(empty_path, "[general]\nname=   \n");
  const SettingsLoadResult empty_result = load_settings(
      SettingsLoadOptions{
          .config_path = empty_path,
          .startup_config_path = std::nullopt,
      },
      1.0);
  remove_config(empty_path);
  expect_true(connection_name(empty_result.store) == empty_path.stem().string(),
              "an empty general name should use the persistent INI stem");

  const std::filesystem::path startup_path =
      temporary_config_path("startup-connection-name");
  write_config(startup_path, "[general]\ntype=local\n");
  const SettingsLoadResult startup_result = load_settings(
      SettingsLoadOptions{
          .config_path = std::nullopt,
          .startup_config_path = startup_path,
      },
      1.0);
  remove_config(startup_path);
  expect_true(connection_name(startup_result.store) ==
                  startup_path.stem().string(),
              "a startup-only configuration should use its INI stem");
}

static void test_terminal_log_settings() {
  const std::filesystem::path path = temporary_config_path("terminal-log");
  write_config(path,
               "[general]\n"
               "name=Tokyo / Lab\n"
               "\n"
               "[log]\n"
               "enabled=true\n"
               "base_directory=/tmp/elder-terms-logs\n"
               "file_name_format=${YYYY-MM-DD}/${hh:mm:ss}_${fff}.txt\n"
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
                  "${YYYY-MM-DD}/${hh:mm:ss}_${fff}.txt",
              "terminal log file name format should come from configuration");
  expect_true(settings.mode == TerminalLogMode::cooked,
              "configured cooked terminal log mode should be retained");
  expect_true(settings.connection_name == "Tokyo / Lab",
              "terminal log settings should retain the effective connection "
              "name for path formatting");
}

static void test_terminal_log_file_name_format_validation() {
  std::string reason;
  expect_true(terminal_log_file_name_format_is_valid(
                  "${YYYYMMDD}/${hhmmss}_${fff}.txt", &reason),
              "nested terminal log file name format should be valid");
  expect_true(terminal_log_file_name_format_is_valid(
                  "${YYYY}-${MM}-${DD}/${hh}-${mm}-${ss}_${fff}.txt", &reason),
              "separate terminal log date and time placeholders should be "
              "valid");
  expect_true(terminal_log_file_name_format_is_valid(
                  "${YYYY-MM-DD}/${hh:mm:ss}_${fff}.txt", &reason),
              "date and time separators inside placeholders should be valid");
  expect_true(terminal_log_file_name_format_is_valid(
                  "${YYYY/MM/DD}/${hhmmss}.txt", &reason),
              "directory separators inside a date placeholder should be "
              "valid");
  expect_true(terminal_log_file_name_format_is_valid(
                  "${documents}/${downloads}/${home}/${name}/${YYYY}.txt",
                  &reason),
              "named path placeholders should be valid in a file name "
              "format");
  expect_true(
      terminal_log_file_name_format_is_valid("literal-{YYYY}-$$.log", &reason),
      "braces without a dollar prefix and an escaped dollar should be "
      "literal text");
  expect_true(terminal_log_file_name_format_is_valid("session.log", &reason),
              "literal terminal log file name should be valid");
  expect_true(!terminal_log_file_name_format_is_valid("/tmp/session.log",
                                                       &reason),
              "absolute terminal log file name format should be invalid");
  expect_true(!terminal_log_file_name_format_is_valid("../session.log",
                                                       &reason),
              "parent traversal in terminal log format should be invalid");
  expect_true(!terminal_log_file_name_format_is_valid("${unknown}.log",
                                                       &reason),
              "unknown terminal log placeholder should be invalid");
  expect_true(!terminal_log_file_name_format_is_valid("${YYYYfooMM}.log",
                                                       &reason),
              "unknown text inside a temporal placeholder should be invalid");
  expect_true(!terminal_log_file_name_format_is_valid("${YYYY.log", &reason),
              "an unmatched opening brace should be invalid");
  expect_true(!terminal_log_file_name_format_is_valid("$YYYY.log", &reason),
              "a literal dollar should require escaping");
  expect_true(!terminal_log_file_name_format_is_valid(
                  "${YYYY/../DD}.log", &reason),
              "parent traversal introduced inside a temporal placeholder "
              "should be invalid");
  expect_true(!terminal_log_file_name_format_is_valid("${YYYY/}", &reason),
              "a temporal placeholder ending in a directory should be "
              "invalid");
  expect_true(!terminal_log_file_name_format_is_valid("${YYYYMMDD}/", &reason),
              "terminal log format ending in a directory should be invalid");
}

static void test_invalid_terminal_log_values_fall_back_to_defaults() {
  const std::filesystem::path path =
      temporary_config_path("invalid-terminal-log");
  write_config(path,
               "[log]\n"
               "file_name_format=../${YYYYMMDD}.txt\n"
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
                  "${YYYYMMDD}_${hhmmss}_${fff}.txt",
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
                  local.backspace_code == TerminalBackspaceCode::automatic &&
                  local.cursor_key_mode == TerminalCursorKeyMode::normal &&
                  local.return_code == TerminalReturnCode::automatic,
              "local terminal text defaults should preserve VTE Return");

  const TerminalTextSettings telnet =
      default_terminal_text_settings(TerminalConnectionKind::telnet);
  expect_true(telnet.encoding == "UTF-8" &&
                  telnet.backspace_code == TerminalBackspaceCode::automatic &&
                  telnet.cursor_key_mode == TerminalCursorKeyMode::normal &&
                  telnet.return_code == TerminalReturnCode::automatic,
              "TELNET terminal text defaults should preserve VTE Return");

  const TerminalTextSettings serial =
      default_terminal_text_settings(TerminalConnectionKind::serial);
  expect_true(serial.encoding == "UTF-8" &&
                  serial.backspace_code == TerminalBackspaceCode::bs &&
                  serial.cursor_key_mode == TerminalCursorKeyMode::trs80 &&
                  serial.return_code == TerminalReturnCode::cr,
              "serial terminal text defaults should match gtk-oldtype");

  const TerminalTextSettings ssh =
      default_terminal_text_settings(TerminalConnectionKind::ssh);
  expect_true(ssh.encoding == "UTF-8" &&
                  ssh.backspace_code == TerminalBackspaceCode::automatic &&
                  ssh.cursor_key_mode == TerminalCursorKeyMode::normal &&
                  ssh.return_code == TerminalReturnCode::automatic,
              "SSH terminal text defaults should preserve VTE Return");
}

static void test_terminal_text_explicit_settings_override_connection_defaults() {
  SettingsStore store =
      create_default_settings(default_terminal_display_settings(1.0),
                              "elder-terms");
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
  set_explicit_setting_value(
      &store, terminal_return_code_setting_key(),
      elder_terms::SettingValue{std::string("lf")});

  TerminalConnectionProfile profile =
      required_terminal_connection_profile(store);
  expect_true(profile.text_settings.encoding == "CP932",
              "explicit terminal encoding should override serial default");
  expect_true(profile.text_settings.backspace_code ==
                  TerminalBackspaceCode::del,
              "explicit Backspace code should override serial default");
  expect_true(profile.text_settings.cursor_key_mode ==
                  TerminalCursorKeyMode::normal,
              "explicit cursor-key mode should override serial default");
  expect_true(profile.text_settings.return_code == TerminalReturnCode::lf,
              "explicit Return code should override serial default");

  clear_explicit_setting_value(&store, terminal_encoding_setting_key());
  clear_explicit_setting_value(&store,
                               terminal_backspace_code_setting_key());
  clear_explicit_setting_value(&store,
                               terminal_cursor_key_mode_setting_key());
  clear_explicit_setting_value(&store, terminal_return_code_setting_key());
  profile = required_terminal_connection_profile(store);
  expect_true(profile.text_settings.encoding == "UTF-8" &&
                  profile.text_settings.backspace_code ==
                      TerminalBackspaceCode::bs &&
                  profile.text_settings.cursor_key_mode ==
                      TerminalCursorKeyMode::trs80 &&
                  profile.text_settings.return_code == TerminalReturnCode::cr,
              "clearing terminal text overrides should restore serial defaults");
}

static void test_terminal_return_code_setting_round_trips() {
  const std::filesystem::path path =
      temporary_config_path("terminal-return-code");
  write_config(path,
               "[general]\n"
               "type=local\n"
               "\n"
               "[terminal]\n"
               "return_code=crlf\n");

  const SettingsLoadResult loaded = load_settings(
      SettingsLoadOptions{
          .config_path = std::optional<std::filesystem::path>{path},
          .startup_config_path = std::nullopt,
      },
      1.0);
  expect_true(loaded.loaded,
              "Return code configuration should load successfully");
  const TerminalTextSettings text =
      required_terminal_connection_profile(loaded.store).text_settings;
  expect_true(text.return_code == TerminalReturnCode::crlf,
              "CRLF Return code should be loaded");
  expect_true(std::string(terminal_return_code_to_string(text.return_code)) ==
                  "crlf",
              "CRLF Return code should use its stable setting name");

  const SettingsSaveResult saved = save_settings(loaded.store, path);
  expect_true(saved.saved,
              "Return code configuration should save successfully");
  expect_true(read_config(path).find("return_code=crlf") != std::string::npos,
              "saved settings should preserve the Return code");
  remove_config(path);
}

static void test_terminal_backspace_auto_setting_round_trips() {
  const std::filesystem::path path =
      temporary_config_path("terminal-backspace-auto");
  write_config(path,
               "[general]\n"
               "type=serial\n"
               "\n"
               "[terminal]\n"
               "backspace_code=auto\n");

  const SettingsLoadResult loaded = load_settings(
      SettingsLoadOptions{
          .config_path = std::optional<std::filesystem::path>{path},
          .startup_config_path = std::nullopt,
      },
      1.0);
  expect_true(loaded.loaded,
              "Auto Backspace configuration should load successfully");
  expect_true(!warnings_contain(
                  loaded.warnings,
                  "invalid configuration value [terminal] backspace_code"),
              "Auto Backspace configuration should be valid");
  const TerminalConnectionProfile profile =
      required_terminal_connection_profile(loaded.store);
  expect_true(std::string(terminal_backspace_code_to_string(
                  profile.text_settings.backspace_code)) == "auto",
              "explicit Auto Backspace should override the serial BS default");

  const SettingsSaveResult saved = save_settings(loaded.store, path);
  expect_true(saved.saved,
              "Auto Backspace configuration should save successfully");
  expect_true(read_config(path).find("backspace_code=auto") !=
                  std::string::npos,
              "saved settings should preserve Auto Backspace");
  remove_config(path);
}

static void test_terminal_cursor_key_mode_uses_trs80_name() {
  const std::filesystem::path path =
      temporary_config_path("trs80-cursor-key-mode");
  write_config(path,
               "[general]\n"
               "type=local\n"
               "\n"
               "[terminal]\n"
               "cursor_key_mode=trs80\n");

  const SettingsLoadResult result = load_settings(
      SettingsLoadOptions{
          .config_path = std::optional<std::filesystem::path>{path},
          .startup_config_path = std::nullopt,
      },
      1.0);
  remove_config(path);

  const TerminalTextSettings text =
      required_terminal_connection_profile(result.store).text_settings;
  expect_true(text.cursor_key_mode == TerminalCursorKeyMode::trs80,
              "TRS80 cursor-key mode should be loaded from its setting name");
  expect_true(std::string(terminal_cursor_key_mode_to_string(
                  text.cursor_key_mode)) == "trs80",
              "TRS80 cursor-key mode should use its setting name when saved");
  expect_true(result.warnings.empty(),
              "TRS80 cursor-key mode should not emit a settings warning");
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
               "cursor_key_mode=invalid\n"
               "return_code=invalid\n");

  const SettingsLoadResult result = load_settings(
      SettingsLoadOptions{
          .config_path = std::optional<std::filesystem::path>{path},
          .startup_config_path = std::nullopt,
      },
      1.0);
  remove_config(path);

  const TerminalTextSettings text =
      required_terminal_connection_profile(result.store).text_settings;
  expect_true(text.encoding == "UTF-8" &&
                  text.backspace_code == TerminalBackspaceCode::bs &&
                  text.cursor_key_mode == TerminalCursorKeyMode::trs80 &&
                  text.return_code == TerminalReturnCode::cr,
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
  expect_true(warnings_contain(
                  result.warnings,
                  "invalid configuration value [terminal] return_code"),
              "invalid Return code should emit a warning");
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

  const auto alphabetic = parse_key_binding("shift+x");
  expect_true(alphabetic.error.empty() && alphabetic.binding.has_value(),
              "an alphabetic key binding should parse");
  expect_true(key_binding_matches(
                  *alphabetic.binding, GDK_KEY_X,
                  static_cast<GdkModifierType>(GDK_SHIFT_MASK |
                                               GDK_LOCK_MASK)),
              "an uppercase event key should match a lowercase binding");
  expect_true(!key_binding_matches(*alphabetic.binding, GDK_KEY_X,
                                   static_cast<GdkModifierType>(0)),
              "alphabetic key normalization should preserve exact modifiers");

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
               "zoom_out_key=\n"
               "send_break_key=shift+F12\n");
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
  expect_true(custom_bindings.send_break.has_value() &&
                  key_binding_matches(*custom_bindings.send_break,
                                      GDK_KEY_F12, GDK_SHIFT_MASK),
              "configured terminal BREAK key should be loaded");

  const std::filesystem::path invalid_path =
      temporary_config_path("terminal-key-bindings-invalid");
  write_config(invalid_path,
               "[terminal]\n"
               "zoom_in_key=ctrl++plus\n"
               "zoom_out_key=ctrl+minus\n"
               "send_break_key=ctrl++F12\n");
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
  expect_true(warnings_contain(
                  invalid.warnings,
                  "invalid configuration value [terminal] send_break_key"),
              "invalid terminal BREAK binding should emit a warning");
  const auto invalid_bindings = terminal_key_bindings(invalid.store);
  expect_true(invalid_bindings.zoom_in.has_value() &&
                  key_binding_matches(*invalid_bindings.zoom_in,
                                      GDK_KEY_equal, GDK_CONTROL_MASK),
              "invalid terminal key binding should use its default");
  expect_true(!invalid_bindings.send_break.has_value(),
              "invalid terminal BREAK binding should use its empty default");

  const std::filesystem::path conflict_path =
      temporary_config_path("terminal-key-bindings-conflict");
  write_config(conflict_path,
               "[terminal]\n"
               "zoom_in_key=alt+F1\n"
               "zoom_out_key=alt+F2\n"
               "send_break_key=ALT-f1\n");
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
                                      GDK_KEY_equal, GDK_CONTROL_MASK) &&
                  key_binding_matches(*conflict_bindings.zoom_out,
                                      GDK_KEY_minus, GDK_CONTROL_MASK) &&
                  !conflict_bindings.send_break.has_value(),
              "conflicting terminal key bindings should all use defaults");
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

static void test_transfer_text_send_follow_return_code_setting() {
  const std::filesystem::path valid =
      temporary_config_path("text-send-follow-return-code");
  write_config(valid,
               "[transfer]\n"
               "text_send_follow_return_code=false\n");

  const SettingsLoadResult valid_result = load_settings(
      SettingsLoadOptions{
          .config_path = std::optional<std::filesystem::path>{valid},
          .startup_config_path = std::nullopt,
      },
      1.0);
  remove_config(valid);

  expect_true(!transfer_text_send_follow_return_code(valid_result.store),
              "explicit false should preserve text-send line endings");

  const std::filesystem::path invalid =
      temporary_config_path("invalid-text-send-follow-return-code");
  write_config(invalid,
               "[transfer]\n"
               "text_send_follow_return_code=sometimes\n");

  const SettingsLoadResult invalid_result = load_settings(
      SettingsLoadOptions{
          .config_path = std::optional<std::filesystem::path>{invalid},
          .startup_config_path = std::nullopt,
      },
      1.0);
  remove_config(invalid);

  expect_true(transfer_text_send_follow_return_code(invalid_result.store),
              "invalid text-send Return-code behavior should use true");
  expect_true(warnings_contain(invalid_result.warnings,
                               "text_send_follow_return_code"),
              "invalid text-send Return-code behavior should warn");
}

static void test_transfer_zmodem_autostart_setting() {
  SettingsStore local_store =
      create_default_settings(default_terminal_display_settings(1.0),
                              "elder-terms");
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
               "port=2323\n"
               "terminal_type=vt220\n");

  const SettingsLoadResult result = load_settings(
      SettingsLoadOptions{
          .config_path = std::optional<std::filesystem::path>{path},
          .startup_config_path = std::nullopt,
      },
      1.0);
  remove_config(path);

  const TerminalConnectionProfile profile =
      required_terminal_connection_profile(result.store);
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
  expect_true(settings->terminal_type == "vt220",
              "TELNET terminal type should come from the configuration file");
}

static void test_ssh_profile() {
  const std::filesystem::path path = temporary_config_path("ssh-profile");
  write_config(path,
               "[general]\n"
               "type=ssh\n"
               "\n"
               "[terminal]\n"
               "backspace_code=bs\n"
               "\n"
               "[ssh]\n"
               "address=ssh.example.test\n"
               "port=2222\n"
               "username=alice\n"
               "identity_file=~/.ssh/id_test\n"
               "terminal_type=vt220\n");

  const SettingsLoadResult result = load_settings(
      SettingsLoadOptions{
          .config_path = std::optional<std::filesystem::path>{path},
          .startup_config_path = std::nullopt,
      },
      1.0);
  remove_config(path);

  const TerminalConnectionProfile profile =
      required_terminal_connection_profile(result.store);
  expect_true(profile.kind == TerminalConnectionKind::ssh,
              "configured connection should be SSH");
  const auto *settings =
      std::get_if<SshConnectionSettings>(&profile.settings);
  expect_true(settings != nullptr,
              "configured connection settings should be SSH settings");
  expect_true(settings->endpoint.address == "ssh.example.test",
              "SSH address should come from the configuration file");
  expect_true(settings->endpoint.port == 2222,
              "SSH port should come from the configuration file");
  expect_true(settings->endpoint.username == "alice",
              "SSH username should come from the configuration file");
  expect_true(settings->endpoint.identity_file == "~/.ssh/id_test",
              "SSH identity should come from the configuration file");
  expect_true(settings->terminal_type == "vt220",
              "SSH terminal type should come from the configuration file");
  expect_true(profile.text_settings.backspace_code ==
                  TerminalBackspaceCode::bs,
              "an explicit Backspace setting should override SSH DEL");
}

static void test_sftp_profile_uses_ssh_endpoint_without_terminal_profile() {
  const std::filesystem::path path = temporary_config_path("sftp-profile");
  write_config(path,
               "[general]\n"
               "type=sftp\n"
               "\n"
               "[ssh]\n"
               "address=sftp.example.test\n"
               "port=2222\n"
               "username=alice\n"
               "identity_file=~/.ssh/id_sftp_test\n"
               "\n"
               "[sftp]\n"
               "local_directory=/home/alice/uploads\n"
               "remote_directory=/srv/incoming\n");

  const SettingsLoadResult result = load_settings(
      SettingsLoadOptions{
          .config_path = std::optional<std::filesystem::path>{path},
          .startup_config_path = std::nullopt,
      },
      1.0);
  remove_config(path);

  expect_true(elder_terms::general_connection_kind(result.store) ==
                  elder_terms::ConnectionKind::sftp,
              "configured connection should be SFTP");
  expect_true(
      elder_terms::general_settings_select_sftp_connection(result.store),
      "SFTP selector should recognize the configured connection");
  expect_true(!terminal_connection_profile(result.store).has_value(),
              "SFTP should not produce a VTE terminal connection profile");

  const elder_terms::SftpConnectionSettings settings =
      elder_terms::sftp_connection_settings(result.store);
  expect_true(settings.endpoint.address == "sftp.example.test",
              "SFTP should reuse the configured SSH address");
  expect_true(settings.endpoint.port == 2222,
              "SFTP should reuse the configured SSH port");
  expect_true(settings.endpoint.username == "alice",
              "SFTP should reuse the configured SSH username");
  expect_true(settings.endpoint.identity_file == "~/.ssh/id_sftp_test",
              "SFTP should reuse the configured SSH identity");
  expect_true(settings.local_directory == "/home/alice/uploads",
              "SFTP local directory should come from the SFTP section");
  expect_true(settings.remote_directory == "/srv/incoming",
              "SFTP remote directory should come from the SFTP section");
}

static void test_sftp_missing_ssh_address_warns() {
  const std::filesystem::path path =
      temporary_config_path("sftp-missing-address");
  write_config(path,
               "[general]\n"
               "type=sftp\n");

  const SettingsLoadResult result = load_settings(
      SettingsLoadOptions{
          .config_path = std::optional<std::filesystem::path>{path},
          .startup_config_path = std::nullopt,
      },
      1.0);
  remove_config(path);

  expect_true(warnings_contain(
                  result.warnings,
                  "missing required configuration value [ssh] address"),
              "SFTP without an SSH address should emit a warning");
}

static void test_invalid_ssh_values_fall_back_and_warn() {
  const std::filesystem::path path =
      temporary_config_path("invalid-ssh-values");
  write_config(path,
               "[general]\n"
               "type=ssh\n"
               "\n"
               "[ssh]\n"
               "address=   \n"
               "port=70000\n"
               "terminal_type=   \n");

  const SettingsLoadResult result = load_settings(
      SettingsLoadOptions{
          .config_path = std::optional<std::filesystem::path>{path},
          .startup_config_path = std::nullopt,
      },
      1.0);
  remove_config(path);

  const TerminalConnectionProfile profile =
      required_terminal_connection_profile(result.store);
  const auto *settings =
      std::get_if<SshConnectionSettings>(&profile.settings);
  expect_true(settings != nullptr,
              "invalid SSH values should retain the SSH profile");
  expect_true(settings->endpoint.address.empty(),
              "blank SSH address should normalize to empty");
  expect_true(settings->endpoint.port == 22,
              "invalid SSH port should fall back to 22");
  expect_true(settings->terminal_type == "xterm-256color",
              "blank SSH terminal type should use the default");
  expect_true(profile.text_settings.backspace_code ==
                  TerminalBackspaceCode::automatic,
              "SSH should use automatic Backspace when it is not explicit");
  expect_true(warnings_contain(
                  result.warnings,
                  "missing required configuration value [ssh] address"),
              "blank SSH address should emit a warning");
  expect_true(warnings_contain(result.warnings,
                               "invalid configuration value [ssh] port"),
              "invalid SSH port should emit a warning");
  expect_true(
      warnings_contain(result.warnings,
                       "invalid configuration value [ssh] terminal_type"),
      "blank SSH terminal type should emit a warning");
}

static void test_serial_profile() {
  const std::filesystem::path path = temporary_config_path("serial-profile");
  write_config(path,
               "[general]\n"
               "type=serial\n"
               "\n"
               "[serial]\n"
               "device=/dev/ttyUSB0\n"
               "device_match_mode=by-path\n"
               "device_usb_serial=FT123456\n"
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
      required_terminal_connection_profile(result.store);
  expect_true(profile.kind == TerminalConnectionKind::serial,
              "configured connection should be serial");
  const auto *settings =
      std::get_if<SerialConnectionSettings>(&profile.settings);
  expect_true(settings != nullptr,
              "configured connection settings should be serial settings");
  expect_true(settings->device == "/dev/ttyUSB0",
              "serial device should come from the configuration file");
  expect_true(settings->device_match_mode ==
                  SerialDeviceMatchMode::physical_port,
              "serial match mode should come from the configuration file");
  expect_true(settings->device_usb_serial ==
                  std::optional<std::string>("FT123456"),
              "serial USB identity should come from the configuration file");
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

static void test_serial_ignore_carrier_profile() {
  const std::filesystem::path path =
      temporary_config_path("serial-ignore-carrier-profile");
  write_config(path,
               "[general]\n"
               "type=serial\n"
               "\n"
               "[serial]\n"
               "device=/dev/ttyUSB0\n"
               "carrier_detect=ignore\n");

  const SettingsLoadResult result = load_settings(
      SettingsLoadOptions{
          .config_path = std::optional<std::filesystem::path>{path},
          .startup_config_path = std::nullopt,
      },
      1.0);
  remove_config(path);

  const TerminalConnectionProfile profile =
      required_terminal_connection_profile(result.store);
  const auto *settings =
      std::get_if<SerialConnectionSettings>(&profile.settings);
  expect_true(settings != nullptr,
              "ignored carrier profile should remain a serial profile");
  expect_true(settings->carrier_detect == SerialCarrierDetect::ignore,
              "ignore should disable serial carrier signal monitoring");
  expect_true(elder_terms::serial_carrier_detect_to_string(
                  settings->carrier_detect) == "ignore",
              "ignored serial carrier detection should round trip");
  expect_true(result.warnings.empty(),
              "ignore should be accepted without configuration warnings");
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
               "show_border=invalid\n"
               "\n"
               "[telnet]\n"
               "address=127.0.0.1\n"
               "port=70000\n"
               "terminal_type=   \n");

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
  expect_true(!terminal_show_border(result.store),
              "invalid terminal border visibility should fall back to false");

  const TerminalConnectionProfile profile =
      required_terminal_connection_profile(result.store);
  const auto *settings =
      std::get_if<TelnetConnectionSettings>(&profile.settings);
  expect_true(settings != nullptr,
              "TELNET profile should still be selected after invalid port");
  expect_true(settings->port == 23,
              "invalid TELNET port should fall back to default");
  expect_true(settings->terminal_type == "xterm-256color",
              "blank TELNET terminal type should fall back to default");

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
  expect_true(warnings_contain(
                  result.warnings,
                  "invalid configuration value [terminal] show_border"),
              "invalid terminal border visibility should emit a warning");
  expect_true(warnings_contain(result.warnings,
                               "invalid configuration value [telnet] port"),
              "invalid TELNET port should emit a warning");
  expect_true(
      warnings_contain(
          result.warnings,
          "invalid configuration value [telnet] terminal_type"),
      "blank TELNET terminal type should emit a warning");
}

static void test_invalid_serial_values_fall_back_to_defaults() {
  const std::filesystem::path path =
      temporary_config_path("invalid-serial-values");
  write_config(path,
               "[general]\n"
               "type=serial\n"
               "\n"
               "[serial]\n"
               "device_match_mode=invalid\n"
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
      required_terminal_connection_profile(result.store);
  const auto *settings =
      std::get_if<SerialConnectionSettings>(&profile.settings);
  expect_true(settings != nullptr,
              "serial profile should still be selected after invalid values");
  expect_true(settings->device.empty(),
              "missing serial device should fall back to default");
  expect_true(settings->device_match_mode == SerialDeviceMatchMode::stable_id,
              "invalid serial match mode should fall back to stable ID");
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
                  "invalid configuration value [serial] device_match_mode"),
              "invalid serial match mode should emit a warning");
  expect_true(warnings_contain(
                  result.warnings,
                  "missing required configuration value [serial] device"),
              "missing serial device should emit a warning");
}

static void test_public_setting_keys() {
  expect_true(elder_terms::general_name_setting_key().section == "general" &&
                  elder_terms::general_name_setting_key().name == "name",
              "general name key should use [general] name");
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
  expect_true(terminal_scrollback_lines_setting_key().section == "terminal" &&
                  terminal_scrollback_lines_setting_key().name ==
                      "scrollback_lines",
              "terminal scrollback key should use [terminal] "
              "scrollback_lines");
  expect_true(terminal_zoom_setting_key().name == "zoom",
              "terminal zoom key should use the zoom name");
  expect_true(
      terminal_font_primary_family_setting_key().section == "terminal" &&
          terminal_font_primary_family_setting_key().name ==
              "font_primary_family",
      "primary font family key should use [terminal] font_primary_family");
  expect_true(
      terminal_font_fallback_family_setting_key().section == "terminal" &&
          terminal_font_fallback_family_setting_key().name ==
              "font_fallback_family",
      "fallback font family key should use [terminal] font_fallback_family");
  expect_true(terminal_auto_close_setting_key().name == "auto_close",
              "terminal auto_close key should use the auto_close name");
  expect_true(terminal_show_border_setting_key().section == "terminal" &&
                  terminal_show_border_setting_key().name == "show_border",
              "terminal border key should use [terminal] show_border");
  expect_true(terminal_zoom_in_key_setting_key().section == "terminal" &&
                  terminal_zoom_in_key_setting_key().name == "zoom_in_key",
              "terminal zoom-in key should use [terminal] zoom_in_key");
  expect_true(terminal_zoom_out_key_setting_key().section == "terminal" &&
                  terminal_zoom_out_key_setting_key().name == "zoom_out_key",
              "terminal zoom-out key should use [terminal] zoom_out_key");
  expect_true(terminal_send_break_key_setting_key().section == "terminal" &&
                  terminal_send_break_key_setting_key().name ==
                      "send_break_key",
              "terminal BREAK key should use [terminal] send_break_key");
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
  expect_true(terminal_return_code_setting_key().section == "terminal" &&
                  terminal_return_code_setting_key().name == "return_code",
              "Return code key should use [terminal] return_code");
  expect_true(
      general_exterior_background_setting_key().section == "general" &&
          general_exterior_background_setting_key().name ==
              "exterior_background",
      "exterior background key should use [general] exterior_background");
  expect_true(
      general_background_setting_key().section == "general" &&
          general_background_setting_key().name == "background",
      "content background key should use [general] background");
  expect_true(telnet_address_setting_key().section == "telnet",
              "TELNET address key should use the telnet section");
  expect_true(telnet_address_setting_key().name == "address",
              "TELNET address key should use the address name");
  expect_true(telnet_port_setting_key().name == "port",
              "TELNET port key should use the port name");
  expect_true(telnet_terminal_type_setting_key().section == "telnet" &&
                  telnet_terminal_type_setting_key().name ==
                      "terminal_type",
              "TELNET terminal type key should use [telnet] terminal_type");
  expect_true(ssh_address_setting_key().section == "ssh" &&
                  ssh_address_setting_key().name == "address",
              "SSH address key should use [ssh] address");
  expect_true(ssh_port_setting_key().section == "ssh" &&
                  ssh_port_setting_key().name == "port",
              "SSH port key should use [ssh] port");
  expect_true(ssh_username_setting_key().section == "ssh" &&
                  ssh_username_setting_key().name == "username",
              "SSH username key should use [ssh] username");
  expect_true(ssh_identity_file_setting_key().section == "ssh" &&
                  ssh_identity_file_setting_key().name == "identity_file",
              "SSH identity key should use [ssh] identity_file");
  expect_true(ssh_terminal_type_setting_key().section == "ssh" &&
                  ssh_terminal_type_setting_key().name == "terminal_type",
              "SSH terminal type key should use [ssh] terminal_type");
  expect_true(
      elder_terms::sftp_local_directory_setting_key().section == "sftp" &&
          elder_terms::sftp_local_directory_setting_key().name ==
              "local_directory",
      "SFTP local directory key should use [sftp] local_directory");
  expect_true(
      elder_terms::sftp_remote_directory_setting_key().section == "sftp" &&
          elder_terms::sftp_remote_directory_setting_key().name ==
              "remote_directory",
      "SFTP remote directory key should use [sftp] remote_directory");
  expect_true(serial_device_setting_key().section == "serial",
              "serial device key should use the serial section");
  expect_true(serial_device_setting_key().name == "device",
              "serial device key should use the device name");
  expect_true(serial_device_match_mode_setting_key().section == "serial" &&
                  serial_device_match_mode_setting_key().name ==
                      "device_match_mode",
              "serial match mode key should use [serial] device_match_mode");
  expect_true(serial_device_usb_serial_setting_key().section == "serial" &&
                  serial_device_usb_serial_setting_key().name ==
                      "device_usb_serial",
              "serial USB identity key should use [serial] device_usb_serial");
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
  expect_true(
      transfer_text_send_follow_return_code_setting_key().section ==
          "transfer" &&
          transfer_text_send_follow_return_code_setting_key().name ==
              "text_send_follow_return_code",
      "text send Return-code behavior key should use the requested name");
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
      create_default_settings(default_terminal_display_settings(1.0),
                              "elder-terms");
  set_setting_value(&store, terminal_log_enabled_setting_key(),
                    elder_terms::SettingValue{true});
  set_setting_value(
      &store, terminal_log_base_directory_setting_key(),
      elder_terms::SettingValue{std::string("/var/log/elder-terms")});
  set_setting_value(
      &store, terminal_log_file_name_format_setting_key(),
      elder_terms::SettingValue{std::string("${YYYYMMDD}/session.txt")});
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
  expect_true(content.find("file_name_format=${YYYYMMDD}/session.txt") !=
                  std::string::npos,
              "saved settings should include terminal log file name format");
  expect_true(content.find("mode=cooked") != std::string::npos,
              "saved settings should include cooked terminal log mode");
}

static void test_save_general_color_settings() {
  const std::filesystem::path path =
      temporary_config_path("save-general-colors");
  SettingsStore store =
      create_default_settings(default_terminal_display_settings(1.0),
                              "elder-terms");
  set_explicit_setting_value(
      &store, general_exterior_background_setting_key(),
      elder_terms::SettingValue{std::string("#A1B2C3")});
  set_explicit_setting_value(
      &store, general_background_setting_key(),
      elder_terms::SettingValue{std::string("none")});

  const SettingsSaveResult result = save_settings(store, path);
  expect_true(result.saved, "general color settings save should succeed");
  const std::string content = read_config(path);

  expect_true(content.find("[general]") != std::string::npos,
              "saved color settings should use the general section");
  expect_true(content.find("exterior_background=#A1B2C3") !=
                  std::string::npos,
              "saved settings should include the exterior RGB color");
  expect_true(content.find("background=none") !=
                  std::string::npos,
              "saved settings should preserve an explicit no-color override");
  expect_true(content.find("[terminal]") == std::string::npos,
              "colors alone should not create a terminal section");

  const SettingsLoadResult loaded = load_settings(
      SettingsLoadOptions{
          .config_path = path,
          .startup_config_path = std::nullopt,
      },
      1.0);
  remove_config(path);
  const GeneralColorSettings colors = general_color_settings(loaded.store);
  expect_true(colors.exterior_background.has_value() &&
                  colors.exterior_background->red == 0xa1 &&
                  colors.exterior_background->green == 0xb2 &&
                  colors.exterior_background->blue == 0xc3,
              "a saved exterior RGB color should survive reloading");
  expect_true(!colors.background.has_value(),
              "a saved no-color override should survive reloading");
  expect_true(setting_has_explicit_value(
                  loaded.store,
                  general_background_setting_key()),
              "a reloaded no-color value should remain explicit");
}

static void test_save_settings_omits_default_values() {
  const std::filesystem::path path = temporary_config_path("save-values");
  SettingsStore store =
      create_default_settings(default_terminal_display_settings(1.0),
                              "elder-terms");
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
  set_setting_value(&store, terminal_show_border_setting_key(),
                    elder_terms::SettingValue{true});
  set_setting_value(&store, terminal_zoom_in_key_setting_key(),
                    elder_terms::SettingValue{std::string("alt+Up")});
  set_setting_value(&store, terminal_send_break_key_setting_key(),
                    elder_terms::SettingValue{std::string("shift+F12")});
  set_setting_value(&store, telnet_address_setting_key(),
                    elder_terms::SettingValue{std::string("host.example")});
  set_setting_value(&store, telnet_port_setting_key(),
                    elder_terms::SettingValue{gint64{23}});
  set_setting_value(&store, telnet_terminal_type_setting_key(),
                    elder_terms::SettingValue{std::string("vt220")});
  set_setting_value(
      &store, transfer_base_path_setting_key(),
      elder_terms::SettingValue{std::string("file:///tmp/downloads")});
  set_setting_value(&store, transfer_text_send_bytes_per_second_setting_key(),
                    elder_terms::SettingValue{gint64{2048}});
  set_setting_value(
      &store, transfer_text_send_follow_return_code_setting_key(),
      elder_terms::SettingValue{false});

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
  expect_true(content.find("show_border=true") != std::string::npos,
              "saved settings should include enabled terminal borders");
  expect_true(content.find("zoom_in_key=alt+Up") != std::string::npos,
              "saved settings should include non-default zoom-in key");
  expect_true(content.find("send_break_key=shift+F12") != std::string::npos,
              "saved settings should include non-default BREAK key");
  expect_true(content.find("[telnet]") != std::string::npos,
              "saved settings should include non-default TELNET section");
  expect_true(content.find("address=host.example") != std::string::npos,
              "saved settings should include non-default TELNET address");
  expect_true(content.find("terminal_type=vt220") != std::string::npos,
              "saved settings should include non-default TELNET terminal type");
  expect_true(content.find("[transfer]") != std::string::npos,
              "saved settings should include non-default transfer section");
  expect_true(content.find("base_path=file:///tmp/downloads") !=
                  std::string::npos,
              "saved settings should include non-default transfer base_path");
  expect_true(content.find("text_send_bytes_per_second=2048") !=
                  std::string::npos,
              "saved settings should include non-default text send rate");
  expect_true(content.find("text_send_follow_return_code=false") !=
                  std::string::npos,
              "saved settings should include text-send Return-code behavior");
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
      create_default_settings(default_terminal_display_settings(1.0),
                              "elder-terms");
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
      create_default_settings(default_terminal_display_settings(1.0),
                              "elder-terms");
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
      create_default_settings(default_terminal_display_settings(1.0),
                              "elder-terms");
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
      required_terminal_connection_profile(changed_type).text_settings;
  expect_true(text.encoding == "UTF-8" &&
                  text.backspace_code == TerminalBackspaceCode::del &&
                  text.cursor_key_mode == TerminalCursorKeyMode::normal,
              "loaded explicit values should remain explicit after type change");
}

static void test_save_explicit_value_equal_to_built_in() {
  const std::filesystem::path path =
      temporary_config_path("save-explicit-built-in");
  SettingsStore store =
      create_default_settings(default_terminal_display_settings(1.0),
                              "elder-terms");
  set_explicit_setting_value(
      &store, terminal_zoom_setting_key(),
      elder_terms::SettingValue{gdouble{1.0}});

  const SettingsSaveResult result = save_settings(store, path);
  expect_true(result.saved,
              "saving an explicit built-in value should succeed");
  const std::string content = read_config(path);
  remove_config(path);
  expect_true(content.find("zoom=1") != std::string::npos,
              "an explicit value equal to built-in should be persisted");
}

static void test_save_serial_settings_omits_default_values() {
  const std::filesystem::path path = temporary_config_path("save-serial-values");
  SettingsStore store =
      create_default_settings(default_terminal_display_settings(1.0),
                              "elder-terms");
  set_setting_value(&store, general_type_setting_key(),
                    elder_terms::SettingValue{std::string("serial")});
  set_setting_value(&store, serial_device_setting_key(),
                    elder_terms::SettingValue{std::string("/dev/ttyUSB0")});
  set_setting_value(&store, serial_device_match_mode_setting_key(),
                    elder_terms::SettingValue{std::string("by-path")});
  set_setting_value(&store, serial_device_usb_serial_setting_key(),
                    elder_terms::SettingValue{std::string("FT123456")});
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
                    elder_terms::SettingValue{std::string("ignore")});

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
  expect_true(content.find("device_match_mode=by-path") != std::string::npos,
              "saved settings should include non-default serial match mode");
  expect_true(content.find("device_usb_serial=FT123456") != std::string::npos,
              "saved settings should include the serial USB identity");
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
  expect_true(content.find("carrier_detect=ignore") != std::string::npos,
              "saved settings should include non-default serial carrier detect");
}

static void test_save_ssh_settings_omits_default_values() {
  const std::filesystem::path path = temporary_config_path("save-ssh-values");
  SettingsStore store =
      create_default_settings(default_terminal_display_settings(1.0),
                              "elder-terms");
  set_setting_value(&store, general_type_setting_key(),
                    elder_terms::SettingValue{std::string("ssh")});
  set_setting_value(&store, ssh_address_setting_key(),
                    elder_terms::SettingValue{
                        std::string("ssh.example.test")});
  set_setting_value(&store, ssh_port_setting_key(),
                    elder_terms::SettingValue{gint64{22}});
  set_setting_value(&store, ssh_username_setting_key(),
                    elder_terms::SettingValue{std::string("alice")});
  set_setting_value(
      &store, ssh_identity_file_setting_key(),
      elder_terms::SettingValue{std::string("~/.ssh/id_ed25519")});
  set_setting_value(&store, ssh_terminal_type_setting_key(),
                    elder_terms::SettingValue{std::string("vt220")});

  const SettingsSaveResult result = save_settings(store, path);
  expect_true(result.saved, "SSH settings save should succeed");
  const std::string content = read_config(path);
  remove_config(path);

  expect_true(content.find("type=ssh") != std::string::npos,
              "saved settings should include the SSH type");
  expect_true(content.find("[ssh]") != std::string::npos,
              "saved settings should include the SSH section");
  expect_true(content.find("address=ssh.example.test") != std::string::npos,
              "saved settings should include the SSH address");
  expect_true(content.find("username=alice") != std::string::npos,
              "saved settings should include the SSH username");
  expect_true(content.find("identity_file=~/.ssh/id_ed25519") !=
                  std::string::npos,
              "saved settings should include the SSH identity");
  expect_true(content.find("terminal_type=vt220") != std::string::npos,
              "saved settings should include the SSH terminal type");
  expect_true(content.find("port=") == std::string::npos,
              "saved settings should omit the default SSH port");
}

static void test_save_sftp_settings_omits_default_values() {
  const std::filesystem::path path =
      temporary_config_path("save-sftp-values");
  SettingsStore store =
      create_default_settings(default_terminal_display_settings(1.0),
                              "elder-terms");
  set_setting_value(&store, general_type_setting_key(),
                    elder_terms::SettingValue{std::string("sftp")});
  set_setting_value(
      &store, elder_terms::sftp_local_directory_setting_key(),
      elder_terms::SettingValue{std::string("/home/alice/uploads")});
  set_setting_value(
      &store, elder_terms::sftp_remote_directory_setting_key(),
      elder_terms::SettingValue{std::string("/srv/incoming")});

  const SettingsSaveResult result = save_settings(store, path);
  expect_true(result.saved, "SFTP settings save should succeed");
  const std::string content = read_config(path);
  remove_config(path);

  expect_true(content.find("type=sftp") != std::string::npos,
              "saved settings should include the SFTP type");
  expect_true(content.find("[sftp]") != std::string::npos,
              "saved settings should include the SFTP section");
  expect_true(content.find("local_directory=/home/alice/uploads") !=
                  std::string::npos,
              "saved settings should include the SFTP local directory");
  expect_true(content.find("remote_directory=/srv/incoming") !=
                  std::string::npos,
              "saved settings should include the SFTP remote directory");
}

static void test_save_settings_writes_empty_file_for_defaults() {
  const std::filesystem::path path = temporary_config_path("save-defaults");
  const SettingsStore store =
      create_default_settings(default_terminal_display_settings(1.0),
                              "elder-terms");

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

static void test_global_settings_layer_priority_and_sources() {
  const std::filesystem::path global_path =
      temporary_config_path("global-layer");
  const std::filesystem::path config_path =
      temporary_config_path("connection-layer");
  const std::filesystem::path startup_path =
      temporary_config_path("startup-layer");
  write_config(global_path,
               "[general]\n"
               "name=Ignored global name\n"
               "type=serial\n"
               "\n"
               "[terminal]\n"
               "width=90\n"
               "height=30\n"
               "backspace_code=del\n"
               "\n"
               "[transfer]\n"
               "zmodem_autostart=false\n");
  write_config(config_path,
               "[terminal]\n"
               "width=100\n"
               "zoom=1.25\n");
  write_config(startup_path,
               "[terminal]\n"
               "width=110\n");

  const SettingsLoadResult result = load_settings(
      SettingsLoadOptions{
          .config_path = config_path,
          .startup_config_path = startup_path,
          .global_config_path = global_path,
      },
      1.0);
  remove_config(global_path);
  remove_config(config_path);
  remove_config(startup_path);

  const TerminalDisplaySettings display =
      terminal_display_settings(result.store);
  expect_true(display.width == 110,
              "startup settings should override connection and global values");
  expect_true(display.height == 30,
              "global settings should override built-in values");
  expect_true(display.zoom == 1.25,
              "connection settings should override built-in values");
  expect_true(setting_value_source(result.store,
                                   terminal_width_setting_key()) ==
                  SettingValueSource::override,
              "startup and connection values should be reported as overrides");
  expect_true(setting_fallback_source(result.store,
                                      terminal_width_setting_key()) ==
                  SettingValueSource::global,
              "an override should retain its global fallback source");
  expect_true(std::get<gint64>(setting_fallback_value(
                  result.store, terminal_width_setting_key(),
                  elder_terms::SettingValue{gint64{0}})) == 90,
              "an override should retain its global fallback value");
  expect_true(setting_value_source(result.store,
                                   terminal_height_setting_key()) ==
                  SettingValueSource::global,
              "a global value should report the global source");
  expect_true(!setting_has_explicit_value(result.store,
                                          terminal_height_setting_key()),
              "a global fallback should not become a connection override");
  expect_true(setting_has_configured_value(result.store,
                                           terminal_height_setting_key()),
              "a global fallback should count as a configured value");
  expect_true(elder_terms::general_connection_name(result.store) ==
                  config_path.stem().string(),
              "global [general] name should not replace the connection name");
  expect_true(setting_value_source(result.store,
                                   elder_terms::general_name_setting_key()) ==
                  SettingValueSource::built_in,
              "global [general] name should remain excluded");

  const TerminalConnectionProfile profile =
      required_terminal_connection_profile(result.store);
  expect_true(profile.kind == TerminalConnectionKind::serial,
              "global connection type should select the serial profile");
  expect_true(profile.text_settings.backspace_code ==
                  TerminalBackspaceCode::del,
              "global terminal text settings should override type defaults");
  expect_true(!elder_terms::transfer_zmodem_autostart(result.store),
              "an explicit global false should override serial ZMODEM default");
}

static void test_invalid_layer_values_use_the_next_fallback() {
  const std::filesystem::path global_path =
      temporary_config_path("invalid-global-layer");
  const std::filesystem::path config_path =
      temporary_config_path("invalid-connection-layer");
  write_config(global_path,
               "[terminal]\n"
               "width=90\n"
               "height=-1\n");
  write_config(config_path,
               "[terminal]\n"
               "width=invalid\n"
               "height=31\n");

  const SettingsLoadResult result = load_settings(
      SettingsLoadOptions{
          .config_path = config_path,
          .startup_config_path = std::nullopt,
          .global_config_path = global_path,
      },
      1.0);
  remove_config(global_path);
  remove_config(config_path);

  const TerminalDisplaySettings display =
      terminal_display_settings(result.store);
  expect_true(display.width == 90,
              "an invalid connection value should use the global fallback");
  expect_true(display.height == 31,
              "a valid connection value should override an invalid global "
              "value");
  expect_true(setting_value_source(result.store,
                                   terminal_width_setting_key()) ==
                  SettingValueSource::global,
              "an invalid override should leave the global source active");
  expect_true(setting_fallback_source(result.store,
                                      terminal_height_setting_key()) ==
                  SettingValueSource::built_in,
              "an invalid global value should retain the built-in fallback");
  expect_true(
      warnings_contain(result.warnings,
                       "invalid configuration value [terminal] width"),
      "an invalid connection value should emit a warning");
  expect_true(
      warnings_contain(result.warnings,
                       "invalid configuration value [terminal] height"),
      "an invalid global value should emit a warning");
}

static void test_global_settings_do_not_flatten_into_connection_files() {
  const std::filesystem::path global_path =
      temporary_config_path("save-global-fallback");
  const std::filesystem::path connection_path =
      temporary_config_path("save-global-connection");
  write_config(global_path,
               "[terminal]\n"
               "width=90\n");

  SettingsLoadResult loaded = load_settings(
      SettingsLoadOptions{
          .config_path = std::nullopt,
          .startup_config_path = std::nullopt,
          .global_config_path = global_path,
      },
      1.0);
  remove_config(global_path);

  const SettingsSaveResult inherited_result =
      save_settings(loaded.store, connection_path);
  expect_true(inherited_result.saved,
              "saving inherited settings should succeed");
  expect_true(read_config(connection_path).empty(),
              "global fallback values should not be flattened into a "
              "connection file");

  expect_true(
      set_explicit_setting_value(
          &loaded.store, terminal_width_setting_key(),
          elder_terms::SettingValue{gint64{90}}),
      "an explicit value equal to the global fallback should be accepted");
  const SettingsSaveResult explicit_result =
      save_settings(loaded.store, connection_path);
  expect_true(explicit_result.saved,
              "saving an explicit value equal to fallback should succeed");
  expect_true(read_config(connection_path).find("width=90") !=
                  std::string::npos,
              "an explicit value equal to its fallback should be persisted");

  clear_explicit_setting_value(&loaded.store, terminal_width_setting_key());
  const SettingsSaveResult cleared_result =
      save_settings(loaded.store, connection_path);
  expect_true(cleared_result.saved,
              "saving after clearing an override should succeed");
  expect_true(read_config(connection_path).empty(),
              "clearing an override should restore non-flattened persistence");
  remove_config(connection_path);
}

static void test_global_settings_editor_excludes_connection_name() {
  const std::filesystem::path path =
      temporary_config_path("global-editor");
  write_config(path,
               "[general]\n"
               "name=Ignored global name\n"
               "type=ssh\n"
               "\n"
               "[terminal]\n"
               "width=92\n");

  SettingsLoadResult result = load_global_settings(path, 1.0);
  expect_true(result.loaded, "a readable global settings file should load");
  expect_true(elder_terms::general_connection_name(result.store) ==
                  "elder-terms",
              "the global editor should always ignore [general] name");
  expect_true(setting_value_source(result.store,
                                   terminal_width_setting_key()) ==
                  SettingValueSource::override,
              "global editor values should be editable overrides");
  expect_true(setting_has_explicit_value(result.store,
                                         terminal_width_setting_key()),
              "global editor values should retain explicit persistence");

  const SettingsSaveResult save_result =
      save_global_settings(result.store, path);
  expect_true(save_result.saved, "saving global settings should succeed");
  const std::string content = read_config(path);
  remove_config(path);
  expect_true(content.find("name=") == std::string::npos,
              "global settings should never persist [general] name");
  expect_true(content.find("type=ssh") != std::string::npos,
              "global settings should persist other general defaults");
  expect_true(content.find("width=92") != std::string::npos,
              "global settings should persist explicit defaults");
}

static void test_application_settings_are_global_only() {
  const std::filesystem::path missing_path =
      temporary_config_path("missing-application-settings");
  remove_config(missing_path);
  const SettingsLoadResult defaults = load_global_settings(missing_path, 1.0);
  expect_true(application_startup_mode(defaults.store) == StartupMode::window,
              "the built-in startup mode should preserve simple startup");
  expect_true(application_ui_language(defaults.store) ==
                  ApplicationUiLanguage::system,
              "the built-in UI language should follow the system");
  const auto default_hotkey = application_open_hotkey(defaults.store);
  expect_true(default_hotkey.has_value() &&
                  key_binding_matches(*default_hotkey, GDK_KEY_t,
                                      static_cast<GdkModifierType>(
                                          GDK_CONTROL_MASK | GDK_MOD1_MASK)),
              "the built-in open-application hotkey should be Ctrl+Alt+T");

  const std::filesystem::path global_path =
      temporary_config_path("application-global");
  write_config(global_path,
               "[general]\n"
               "ui_language=ja\n"
               "startup_mode=tray\n"
               "open_application=\n");
  SettingsLoadResult global = load_global_settings(global_path, 1.0);
  expect_true(load_application_ui_language_preference(global_path) ==
                  ApplicationUiLanguage::japanese,
              "the startup preference reader should select Japanese");
  expect_true(application_ui_language(global.store) ==
                  ApplicationUiLanguage::japanese,
              "global.ini should select Japanese UI text");
  expect_true(application_startup_mode(global.store) == StartupMode::tray,
              "global.ini should select tray-only startup");
  expect_true(!application_open_hotkey(global.store).has_value(),
              "an explicit empty global hotkey should disable registration");
  expect_true(setting_has_explicit_value(
                  global.store, application_ui_language_setting_key()) &&
                  setting_has_explicit_value(
                  global.store, application_startup_mode_setting_key()) &&
                  setting_has_explicit_value(
                      global.store, application_open_hotkey_setting_key()),
              "application settings loaded from global.ini should remain "
              "explicit");

  const std::filesystem::path background_path =
      temporary_config_path("application-background-startup");
  write_config(background_path,
               "[general]\n"
               "startup_mode=background\n");
  const SettingsLoadResult background =
      load_global_settings(background_path, 1.0);
  expect_true(
      std::string(startup_mode_to_string(
          application_startup_mode(background.store))) == "background",
      "global.ini should select background-only startup");
  expect_true(background.warnings.empty(),
              "background-only startup should load without warnings");
  expect_true(save_global_settings(background.store, background_path).saved,
              "background-only startup should save");
  expect_true(read_config(background_path).find("startup_mode=background") !=
                  std::string::npos,
              "global.ini should preserve background-only startup");
  remove_config(background_path);

  const std::vector<std::string> supported_ui_languages = {
      "ar", "es", "fr", "hi", "ja", "ko", "pt", "ru", "zh"};
  for (const std::string &language : supported_ui_languages) {
    const std::filesystem::path language_path =
        temporary_config_path("application-language-" + language);
    write_config(language_path,
                 "[general]\nui_language=" + language + "\n");
    const SettingsLoadResult language_settings =
        load_global_settings(language_path, 1.0);
    expect_true(
        std::string(application_ui_language_to_string(
            load_application_ui_language_preference(language_path))) ==
            language &&
            std::string(application_ui_language_to_string(
                application_ui_language(language_settings.store))) ==
                language,
        language + " should be accepted as a configured UI language");
    remove_config(language_path);
  }

  expect_true(set_explicit_setting_value(
                  &global.store, application_ui_language_setting_key(),
                  elder_terms::SettingValue{std::string("en")}),
              "English should be accepted as an explicit UI language");
  expect_true(set_explicit_setting_value(
                  &global.store, application_startup_mode_setting_key(),
                  elder_terms::SettingValue{
                      std::string("window_and_tray")}),
              "the combined startup mode should be accepted");
  expect_true(set_explicit_setting_value(
                  &global.store, application_open_hotkey_setting_key(),
                  elder_terms::SettingValue{
                      std::string("ctrl+shift+y")}),
              "a modified global hotkey should be accepted");
  const SettingsSaveResult global_save =
      save_global_settings(global.store, global_path);
  expect_true(global_save.saved, "application global settings should save");
  const std::string global_contents = read_config(global_path);
  expect_true(global_contents.find("ui_language=en") != std::string::npos &&
                  global_contents.find("startup_mode=window_and_tray") !=
                  std::string::npos &&
                  global_contents.find("open_application=ctrl+shift+y") !=
                      std::string::npos,
              "global.ini should persist both application settings");
  remove_config(global_path);

  const std::filesystem::path invalid_path =
      temporary_config_path("invalid-application-global");
  write_config(invalid_path,
               "[general]\n"
               "ui_language=de\n"
               "startup_mode=unsupported\n"
               "open_application=t\n");
  const SettingsLoadResult invalid = load_global_settings(invalid_path, 1.0);
  expect_true(load_application_ui_language_preference(invalid_path) ==
                  ApplicationUiLanguage::system,
              "the startup preference reader should reject unknown values");
  remove_config(invalid_path);
  expect_true(application_startup_mode(invalid.store) == StartupMode::window,
              "an invalid startup mode should use the built-in default");
  expect_true(application_ui_language(invalid.store) ==
                  ApplicationUiLanguage::system,
              "an unsupported UI language should follow the system");
  const auto invalid_hotkey = application_open_hotkey(invalid.store);
  expect_true(invalid_hotkey.has_value() &&
                  key_binding_matches(*invalid_hotkey, GDK_KEY_t,
                                      static_cast<GdkModifierType>(
                                          GDK_CONTROL_MASK | GDK_MOD1_MASK)),
              "a modifier-free application hotkey should use the default");
  expect_true(
      warnings_contain(invalid.warnings,
                       "invalid configuration value [general] ui_language") &&
          warnings_contain(invalid.warnings,
                       "invalid configuration value [general] startup_mode") &&
          warnings_contain(
              invalid.warnings,
              "invalid configuration value [general] open_application"),
      "invalid application settings should emit precise warnings");

  const std::filesystem::path connection_path =
      temporary_config_path("application-connection");
  write_config(connection_path,
               "[general]\n"
               "name=Connection\n"
               "ui_language=ja\n"
               "startup_mode=tray\n"
               "open_application=ctrl+shift+x\n");
  SettingsLoadResult connection = load_settings(
      SettingsLoadOptions{
          .config_path = connection_path,
          .startup_config_path = std::nullopt,
      },
      1.0);
  expect_true(application_startup_mode(connection.store) ==
                  StartupMode::window,
              "connection files must not configure application startup");
  expect_true(application_ui_language(connection.store) ==
                  ApplicationUiLanguage::system,
              "connection files must not configure the UI language");
  const auto connection_hotkey = application_open_hotkey(connection.store);
  expect_true(connection_hotkey.has_value() &&
                  key_binding_matches(*connection_hotkey, GDK_KEY_t,
                                      static_cast<GdkModifierType>(
                                          GDK_CONTROL_MASK | GDK_MOD1_MASK)),
              "connection files must not configure the application hotkey");
  expect_true(save_settings(connection.store, connection_path).saved,
              "a connection containing ignored application keys should save");
  const std::string connection_contents = read_config(connection_path);
  expect_true(connection_contents.find("ui_language") == std::string::npos &&
                  connection_contents.find("startup_mode") ==
                      std::string::npos &&
                  connection_contents.find("open_application") ==
                      std::string::npos,
              "connection saves must omit application-only settings");
  remove_config(connection_path);

  expect_true(std::string(application_ui_language_to_string(
                  ApplicationUiLanguage::system)) == "system" &&
                  std::string(application_ui_language_to_string(
                      ApplicationUiLanguage::english)) == "en" &&
                  std::string(application_ui_language_to_string(
                      ApplicationUiLanguage::japanese)) == "ja",
              "UI languages should expose stable configuration values");
  expect_true(load_application_ui_language_preference(missing_path) ==
                  ApplicationUiLanguage::system,
              "a missing startup preference should follow the system");
}

static void test_connection_open_hotkey_settings() {
  const SettingsStore defaults = create_default_settings(
      default_terminal_display_settings(1.0), "Connection");
  expect_true(general_open_connection_hotkey_text(defaults).empty() &&
                  !general_open_connection_hotkey(defaults).has_value(),
              "connection hotkeys should be disabled by default");
  expect_true(
      general_open_connection_hotkey_setting_key().section == "general" &&
          general_open_connection_hotkey_setting_key().name ==
              "open_connection",
      "the connection hotkey should use [general] open_connection");

  const std::filesystem::path connection_path =
      temporary_config_path("connection-open-hotkey");
  write_config(connection_path,
               "[general]\n"
               "name=Connection\n"
               "open_connection=ctrl+shift+y\n");
  SettingsLoadResult connection = load_settings(
      SettingsLoadOptions{
          .config_path = connection_path,
          .startup_config_path = std::nullopt,
      },
      1.0);
  const auto configured = general_open_connection_hotkey(connection.store);
  expect_true(
      configured.has_value() &&
          key_binding_matches(*configured, GDK_KEY_y,
                              static_cast<GdkModifierType>(
                                  GDK_CONTROL_MASK | GDK_SHIFT_MASK)),
      "a connection file should configure its launch hotkey");
  expect_true(setting_has_explicit_value(
                  connection.store,
                  general_open_connection_hotkey_setting_key()),
              "a loaded connection hotkey should remain explicit");
  expect_true(save_settings(connection.store, connection_path).saved,
              "a connection hotkey should save");
  expect_true(
      read_config(connection_path).find(
          "open_connection=ctrl+shift+y") != std::string::npos,
      "a connection save should persist its launch hotkey");
  remove_config(connection_path);

  const std::filesystem::path invalid_path =
      temporary_config_path("invalid-connection-open-hotkey");
  write_config(invalid_path,
               "[general]\n"
               "open_connection=y\n");
  const SettingsLoadResult invalid = load_settings(
      SettingsLoadOptions{
          .config_path = invalid_path,
          .startup_config_path = std::nullopt,
      },
      1.0);
  remove_config(invalid_path);
  expect_true(!general_open_connection_hotkey(invalid.store).has_value(),
              "a modifier-free connection hotkey should use the disabled "
              "default");
  expect_true(
      warnings_contain(
          invalid.warnings,
          "invalid configuration value [general] open_connection"),
      "an invalid connection hotkey should emit a precise warning");

  const std::filesystem::path global_path =
      temporary_config_path("connection-open-hotkey-global");
  write_config(global_path,
               "[general]\n"
               "open_connection=ctrl+shift+g\n");
  SettingsLoadResult global = load_global_settings(global_path, 1.0);
  expect_true(!general_open_connection_hotkey(global.store).has_value(),
              "the global settings editor should ignore connection "
              "hotkeys");
  expect_true(save_global_settings(global.store, global_path).saved,
              "global settings should save after ignoring a connection "
              "hotkey");
  expect_true(read_config(global_path).find("open_connection") ==
                  std::string::npos,
              "global settings should never persist connection hotkeys");

  const SettingsLoadResult inherited = load_settings(
      SettingsLoadOptions{
          .config_path = std::nullopt,
          .startup_config_path = std::nullopt,
          .global_config_path = global_path,
      },
      1.0);
  remove_config(global_path);
  expect_true(!general_open_connection_hotkey(inherited.store).has_value(),
              "connection hotkeys should never inherit from global.ini");
}

static void test_rebase_preserves_draft_overrides_and_dirty_state() {
  const std::filesystem::path first_global_path =
      temporary_config_path("rebase-first-global");
  const std::filesystem::path second_global_path =
      temporary_config_path("rebase-second-global");
  const std::filesystem::path connection_path =
      temporary_config_path("rebase-connection-name");
  write_config(first_global_path,
               "[terminal]\n"
               "width=90\n"
               "height=30\n");
  write_config(second_global_path,
               "[terminal]\n"
               "width=95\n"
               "height=40\n");
  write_config(connection_path, "[general]\ntype=local\n");

  SettingsLoadResult draft = load_settings(
      SettingsLoadOptions{
          .config_path = connection_path,
          .startup_config_path = std::nullopt,
          .global_config_path = first_global_path,
      },
      1.0);
  const SettingsLoadResult next_fallbacks = load_settings(
      SettingsLoadOptions{
          .config_path = std::nullopt,
          .startup_config_path = std::nullopt,
          .global_config_path = second_global_path,
      },
      1.0);
  remove_config(first_global_path);
  remove_config(second_global_path);
  remove_config(connection_path);

  set_explicit_setting_value(
      &draft.store, terminal_width_setting_key(),
      elder_terms::SettingValue{gint64{100}});
  expect_true(setting_is_dirty(draft.store, terminal_width_setting_key()),
              "edited draft override should start dirty");
  expect_true(!setting_is_dirty(draft.store, terminal_height_setting_key()),
              "inherited draft field should start clean");

  rebase_settings_store_fallbacks(&draft.store, next_fallbacks.store);

  const TerminalDisplaySettings display =
      terminal_display_settings(draft.store);
  expect_true(display.width == 100,
              "rebase should preserve an explicit draft override");
  expect_true(display.height == 40,
              "rebase should update an inherited draft value");
  expect_true(std::get<gint64>(setting_fallback_value(
                  draft.store, terminal_width_setting_key(),
                  elder_terms::SettingValue{gint64{0}})) == 95,
              "rebase should update the fallback behind an override");
  expect_true(setting_is_dirty(draft.store, terminal_width_setting_key()),
              "rebase should preserve override dirty state");
  expect_true(!setting_is_dirty(draft.store, terminal_height_setting_key()),
              "fallback-only rebase should not dirty an inherited field");
  expect_true(elder_terms::general_connection_name(draft.store) ==
                  connection_path.stem().string(),
              "rebase should preserve the connection-specific fallback name");
  expect_true(elder_terms::general_connection_kind(draft.store) ==
                  elder_terms::ConnectionKind::local_shell &&
                  setting_value_source(draft.store,
                                       general_type_setting_key()) ==
                      SettingValueSource::override &&
                  !setting_is_dirty(draft.store, general_type_setting_key()),
              "rebase should preserve a clean loaded connection override");
}

static void test_key_binding_conflicts_are_resolved_per_layer() {
  const std::filesystem::path invalid_global_path =
      temporary_config_path("conflicting-global-bindings");
  write_config(invalid_global_path,
               "[terminal]\n"
               "zoom_in_key=ctrl+plus\n"
               "zoom_out_key=ctrl+plus\n"
               "send_break_key=alt+F12\n");
  const SettingsLoadResult invalid_global = load_settings(
      SettingsLoadOptions{
          .config_path = std::nullopt,
          .startup_config_path = std::nullopt,
          .global_config_path = invalid_global_path,
      },
      1.0);
  remove_config(invalid_global_path);
  expect_true(setting_value_source(invalid_global.store,
                                   terminal_zoom_in_key_setting_key()) ==
                  SettingValueSource::built_in &&
                  setting_value_source(
                      invalid_global.store,
                      terminal_zoom_out_key_setting_key()) ==
                      SettingValueSource::built_in &&
                  setting_value_source(
                      invalid_global.store,
                      terminal_send_break_key_setting_key()) ==
                      SettingValueSource::built_in &&
                  terminal_send_break_key(invalid_global.store).empty(),
              "a conflicting global action set should fall back to built-ins");

  const std::filesystem::path valid_global_path =
      temporary_config_path("valid-global-bindings");
  const std::filesystem::path invalid_connection_path =
      temporary_config_path("conflicting-connection-bindings");
  write_config(valid_global_path,
               "[terminal]\n"
               "zoom_in_key=alt+Up\n"
               "zoom_out_key=alt+Down\n"
               "send_break_key=alt+F12\n");
  write_config(invalid_connection_path,
               "[terminal]\n"
               "zoom_in_key=ctrl+Left\n"
               "zoom_out_key=ctrl+Right\n"
               "send_break_key=ctrl+Left\n");
  const SettingsLoadResult invalid_connection = load_settings(
      SettingsLoadOptions{
          .config_path = invalid_connection_path,
          .startup_config_path = std::nullopt,
          .global_config_path = valid_global_path,
      },
      1.0);
  expect_true(elder_terms::terminal_zoom_in_key(invalid_connection.store) ==
                  "alt+Up" &&
                  elder_terms::terminal_zoom_out_key(
                      invalid_connection.store) == "alt+Down" &&
                  terminal_send_break_key(invalid_connection.store) ==
                      "alt+F12",
              "a conflicting connection action set should fall back to the "
              "valid global action set");
  expect_true(setting_value_source(
                  invalid_connection.store,
                  terminal_zoom_in_key_setting_key()) ==
                  SettingValueSource::global &&
                  setting_value_source(
                      invalid_connection.store,
                      terminal_zoom_out_key_setting_key()) ==
                      SettingValueSource::global &&
                  setting_value_source(
                      invalid_connection.store,
                      terminal_send_break_key_setting_key()) ==
                      SettingValueSource::global,
              "clearing a conflicting override should retain global sources");

  const std::filesystem::path valid_connection_path =
      temporary_config_path("valid-connection-bindings");
  const std::filesystem::path invalid_startup_path =
      temporary_config_path("conflicting-startup-bindings");
  write_config(valid_connection_path,
               "[terminal]\n"
               "zoom_in_key=ctrl+Left\n"
               "zoom_out_key=ctrl+Right\n"
               "send_break_key=ctrl+F12\n");
  write_config(invalid_startup_path,
               "[terminal]\n"
               "zoom_in_key=shift+Up\n"
               "zoom_out_key=shift+Down\n"
               "send_break_key=shift+Down\n");
  const SettingsLoadResult invalid_startup = load_settings(
      SettingsLoadOptions{
          .config_path = valid_connection_path,
          .startup_config_path = invalid_startup_path,
          .global_config_path = valid_global_path,
      },
      1.0);
  remove_config(valid_global_path);
  remove_config(invalid_connection_path);
  remove_config(valid_connection_path);
  remove_config(invalid_startup_path);
  expect_true(elder_terms::terminal_zoom_in_key(invalid_startup.store) ==
                  "ctrl+Left" &&
                  elder_terms::terminal_zoom_out_key(invalid_startup.store) ==
                      "ctrl+Right" &&
                  terminal_send_break_key(invalid_startup.store) ==
                      "ctrl+F12",
              "a conflicting startup action set should restore the "
              "connection layer");
  expect_true(setting_value_source(
                  invalid_startup.store,
                  terminal_zoom_in_key_setting_key()) ==
                  SettingValueSource::override &&
                  setting_value_source(
                      invalid_startup.store,
                      terminal_zoom_out_key_setting_key()) ==
                      SettingValueSource::override &&
                  setting_value_source(
                      invalid_startup.store,
                      terminal_send_break_key_setting_key()) ==
                      SettingValueSource::override,
              "restored connection bindings should remain explicit "
              "overrides");
}

static void test_connection_macro_settings_round_trip_and_layering() {
  const std::filesystem::path connection_path =
      temporary_config_path("connection-macros");
  const std::filesystem::path startup_path =
      temporary_config_path("startup-macros");
  const std::filesystem::path global_path =
      temporary_config_path("global-macros");
  const std::filesystem::path saved_path =
      temporary_config_path("saved-macros");

  write_config(
      connection_path,
      "[macro.reply_challenge]\n"
      "regex=^CHALLENGE: (?<token>[A-Za-z0-9]+)$\n"
      "send=RESPONSE ${token}\\r\\n\n"
      "\n"
      "[macro.notify_error]\n"
      "regex=^ERROR (?<code>\\\\d+): (?<message>.*)$\n"
      "command=notify-send\n"
      "arguments=elder-terms;Error ${code}: ${message};\n");
  write_config(global_path,
               "[macro.ignored_global]\n"
               "regex=global\n"
               "send=ignored\n");

  SettingsLoadResult loaded = load_settings(
      SettingsLoadOptions{
          .config_path = connection_path,
          .startup_config_path = std::nullopt,
          .global_config_path = global_path,
      },
      1.0);
  expect_true(loaded.loaded, "valid connection macros should load");
  expect_true(loaded.store.macro_rules.size() == 2,
              "only connection macros should be loaded");
  expect_true(loaded.store.macro_rules[0].id == "reply_challenge" &&
                  loaded.store.macro_rules[1].id == "notify_error",
              "macro section order should define priority");
  const auto *send = std::get_if<elder_terms::MacroSendAction>(
      &loaded.store.macro_rules[0].action);
  expect_true(send != nullptr && send->text == "RESPONSE ${token}\r\n",
              "send actions should decode key-file escapes");
  const auto *command = std::get_if<elder_terms::MacroCommandAction>(
      &loaded.store.macro_rules[1].action);
  expect_true(command != nullptr && command->command == "notify-send" &&
                  command->arguments ==
                      std::vector<std::string>{"elder-terms",
                                               "Error ${code}: ${message}"},
              "command actions should retain ordered arguments");
  expect_true(!elder_terms::settings_store_is_dirty(loaded.store),
              "loaded macro rules should start clean");

  const SettingsSaveResult saved = save_settings(loaded.store, saved_path);
  expect_true(saved.saved, "macro settings should save");
  const std::string saved_text = read_config(saved_path);
  const std::size_t reply_position =
      saved_text.find("[macro.reply_challenge]");
  const std::size_t notify_position =
      saved_text.find("[macro.notify_error]");
  expect_true(reply_position != std::string::npos &&
                  notify_position != std::string::npos &&
                  reply_position < notify_position,
              "saved macros should retain priority order");
  const SettingsLoadResult reloaded = load_settings(
      SettingsLoadOptions{
          .config_path = saved_path,
          .startup_config_path = std::nullopt,
          .global_config_path = std::nullopt,
      },
      1.0);
  expect_true(reloaded.store.macro_rules == loaded.store.macro_rules,
              "saved macro rules should round trip");

  elder_terms::set_macro_rules(
      &loaded.store,
      {elder_terms::MacroRule{
          .id = "replacement",
          .pattern = "READY",
          .action = elder_terms::MacroSendAction{.text = "go\\n"},
      }});
  expect_true(elder_terms::settings_store_is_dirty(loaded.store),
              "replacing macro rules should mark the store dirty");

  write_config(startup_path, "[terminal]\nwidth=91\n");
  const SettingsLoadResult startup_clears_macros = load_settings(
      SettingsLoadOptions{
          .config_path = connection_path,
          .startup_config_path = startup_path,
          .global_config_path = global_path,
      },
      1.0);
  expect_true(startup_clears_macros.store.macro_rules.empty(),
              "a valid startup profile should replace connection macros, "
              "including with an empty set");

  remove_config(connection_path);
  remove_config(startup_path);
  remove_config(global_path);
  remove_config(saved_path);
}

static void test_invalid_connection_macros_warn_and_are_ignored() {
  const std::filesystem::path path =
      temporary_config_path("invalid-connection-macros");
  write_config(path,
               "[macro.bad id]\n"
               "regex=(\n"
               "send=value\n"
               "command=missing\n"
               "\n"
               "[macro.valid]\n"
               "regex=READY\n"
               "send=go\n");

  const SettingsLoadResult loaded = load_settings(
      SettingsLoadOptions{
          .config_path = path,
          .startup_config_path = std::nullopt,
          .global_config_path = std::nullopt,
      },
      1.0);
  remove_config(path);
  expect_true(loaded.store.macro_rules.size() == 1 &&
                  loaded.store.macro_rules[0].id == "valid",
              "invalid macro rules should not disable valid rules");
  expect_true(warnings_contain(loaded.warnings,
                               "invalid macro [macro.bad id]"),
              "invalid macro rules should emit a warning");
}

static void test_global_hyperlink_actions_override_built_in_defaults() {
  const std::filesystem::path connection_path =
      temporary_config_path("connection-hyperlinks");
  const std::filesystem::path global_path =
      temporary_config_path("global-hyperlinks");
  const std::filesystem::path saved_path =
      temporary_config_path("saved-global-hyperlinks");

  write_config(connection_path,
               "[hyperlink]\n"
               "enabled=false\n"
               "\n"
               "[hyperlink.ignored_connection]\n"
               "regex=^ignored:(?<value>.+)$\n"
               "command=ignored\n"
               "arguments=${value};\n");
  write_config(global_path,
               "[hyperlink]\n"
               "enabled=true\n"
               "\n"
               "[hyperlink.second]\n"
               "regex=^open:(?<path>.+):(?<line>[0-9]+)$\n"
               "command=second-tool\n"
               "arguments=--line;${line};${path|uri-decode};\n"
               "\n"
               "[hyperlink.first]\n"
               "regex=^first:(?<value>.+)$\n"
               "command=first-tool\n"
               "arguments=${value};\n");

  const SettingsLoadResult loaded = load_settings(
      SettingsLoadOptions{
          .config_path = connection_path,
          .startup_config_path = std::nullopt,
          .global_config_path = global_path,
      },
      1.0);
  expect_true(loaded.loaded, "valid hyperlink actions should load");
  expect_true(loaded.store.hyperlink_actions_enabled,
              "connection files must not disable global hyperlink actions");
  expect_true(loaded.store.hyperlink_rules.size() == 2 &&
                  loaded.store.hyperlink_rules[0].id == "second" &&
                  loaded.store.hyperlink_rules[1].id == "first",
              "global hyperlink section order should define priority");
  expect_true(loaded.store.hyperlink_settings_configured,
              "custom global hyperlink actions should be marked configured");
  expect_true(!elder_terms::settings_store_is_dirty(loaded.store),
              "loaded hyperlink actions should start clean");

  SettingsLoadResult editable = load_global_settings(global_path, 1.0);
  const SettingsSaveResult saved =
      save_global_settings(editable.store, saved_path);
  expect_true(saved.saved, "global hyperlink actions should save");
  const SettingsLoadResult reloaded = load_global_settings(saved_path, 1.0);
  expect_true(reloaded.store.hyperlink_actions_enabled ==
                      editable.store.hyperlink_actions_enabled &&
                  reloaded.store.hyperlink_rules ==
                      editable.store.hyperlink_rules &&
                  reloaded.store.hyperlink_settings_configured,
              "global hyperlink actions should round trip");

  elder_terms::set_hyperlink_actions(
      &editable.store, false,
      {elder_terms::HyperlinkActionRule{
          .id = "replacement",
          .pattern = "^replacement:(?<value>.+)$",
          .command = "replacement-tool",
          .arguments = {"${value}"},
      }});
  expect_true(elder_terms::settings_store_is_dirty(editable.store),
              "replacing hyperlink actions should mark the store dirty");

  remove_config(connection_path);
  remove_config(global_path);
  remove_config(saved_path);
}

static void test_hyperlink_defaults_disable_and_invalid_rules() {
  const SettingsStore defaults = create_default_settings(
      default_terminal_display_settings(1.0), "defaults");
  expect_true(defaults.hyperlink_actions_enabled &&
                  defaults.hyperlink_rules.size() == 2 &&
                  !defaults.hyperlink_settings_configured,
              "VS Code hyperlink actions should be built in by default");

  const std::filesystem::path disabled_path =
      temporary_config_path("disabled-hyperlinks");
  write_config(disabled_path,
               "[hyperlink]\n"
               "enabled=false\n");
  const SettingsLoadResult disabled =
      load_global_settings(disabled_path, 1.0);
  expect_true(!disabled.store.hyperlink_actions_enabled &&
                  disabled.store.hyperlink_rules.empty() &&
                  disabled.store.hyperlink_settings_configured,
              "an explicit disabled section should suppress built-in rules");

  const std::filesystem::path invalid_path =
      temporary_config_path("invalid-hyperlinks");
  write_config(invalid_path,
               "[hyperlink]\n"
               "enabled=true\n"
               "\n"
               "[hyperlink.bad]\n"
               "regex=^bad:(?<path>.+)$\n"
               "command=bad-tool\n"
               "arguments=${path|shell};\n"
               "\n"
               "[hyperlink.valid]\n"
               "regex=^valid:(?<path>.+)$\n"
               "command=valid-tool\n"
               "arguments=${path|uri-decode};\n");
  const SettingsLoadResult invalid = load_global_settings(invalid_path, 1.0);
  expect_true(invalid.store.hyperlink_rules.size() == 1 &&
                  invalid.store.hyperlink_rules[0].id == "valid",
              "invalid custom rules should not restore built-in actions or "
              "disable valid custom rules");
  expect_true(warnings_contain(invalid.warnings,
                               "invalid hyperlink [hyperlink.bad]"),
              "invalid hyperlink rules should emit a warning");

  remove_config(disabled_path);
  remove_config(invalid_path);
}

static void test_missing_global_settings_are_optional() {
  const std::filesystem::path missing =
      temporary_config_path("missing-global");
  const SettingsLoadResult result = load_settings(
      SettingsLoadOptions{
          .config_path = std::nullopt,
          .startup_config_path = std::nullopt,
          .global_config_path = missing,
      },
      1.0);
  expect_true(result.loaded,
              "a missing optional global file should not fail settings load");
  expect_true(result.warnings.empty(),
              "a missing optional global file should not emit a warning");

  const std::filesystem::path default_path = default_global_config_path();
  expect_true(default_path.filename() == "global.ini" &&
                  default_path.parent_path().filename() == "elder-terms",
              "the default global path should be elder-terms/global.ini");
}

static void test_save_empty_global_settings_creates_parent_directory() {
  const std::filesystem::path root =
      temporary_config_path("global-parent-directory");
  const std::filesystem::path path =
      root / "elder-terms" / "global.ini";
  const SettingsStore store =
      create_default_settings(default_terminal_display_settings(1.0),
                              "elder-terms");

  const SettingsSaveResult result = save_global_settings(store, path);
  expect_true(result.saved,
              "saving empty global defaults should create missing parents");
  expect_true(std::filesystem::is_regular_file(path),
              "empty global defaults should leave a global.ini file");
  expect_true(read_config(path).empty(),
              "fully inherited global defaults should save an empty file");

  std::error_code remove_error;
  std::filesystem::remove_all(root, remove_error);
}

} // namespace elder_terms_settings_test

int main() {
  try {
    elder_terms_settings_test::test_default_settings();
    elder_terms_settings_test::
        test_terminal_scrollback_lines_range_and_round_trip();
    elder_terms_settings_test::
        test_terminal_font_family_settings_round_trip_and_layering();
    elder_terms_settings_test::
        test_terminal_font_family_defaults_override_global_fonts();
    elder_terms_settings_test::test_connection_name_settings();
    elder_terms_settings_test::test_general_color_settings();
    elder_terms_settings_test::
        test_general_color_none_overrides_global_colors();
    elder_terms_settings_test::
        test_terminal_type_defaults_follow_background_color();
    elder_terms_settings_test::
        test_configured_terminal_types_override_background_default();
    elder_terms_settings_test::
        test_invalid_general_colors_fall_back_and_warn();
    elder_terms_settings_test::test_terminal_text_defaults_follow_connection_type();
    elder_terms_settings_test::test_terminal_text_explicit_settings_override_connection_defaults();
    elder_terms_settings_test::test_terminal_backspace_auto_setting_round_trips();
    elder_terms_settings_test::test_terminal_return_code_setting_round_trips();
    elder_terms_settings_test::test_terminal_cursor_key_mode_uses_trs80_name();
    elder_terms_settings_test::test_terminal_encoding_choices_are_supported();
    elder_terms_settings_test::test_terminal_log_settings();
    elder_terms_settings_test::test_terminal_log_file_name_format_validation();
    elder_terms_settings_test::test_key_binding_parser_uses_exact_modifiers();
    elder_terms_settings_test::test_terminal_key_binding_configuration();
    elder_terms_settings_test::test_telnet_profile();
    elder_terms_settings_test::test_ssh_profile();
    elder_terms_settings_test::
        test_sftp_profile_uses_ssh_endpoint_without_terminal_profile();
    elder_terms_settings_test::test_sftp_missing_ssh_address_warns();
    elder_terms_settings_test::test_serial_profile();
    elder_terms_settings_test::test_serial_ignore_carrier_profile();
    elder_terms_settings_test::test_transfer_base_path_setting();
    elder_terms_settings_test::test_transfer_text_send_bytes_per_second_setting();
    elder_terms_settings_test::test_transfer_text_send_follow_return_code_setting();
    elder_terms_settings_test::test_transfer_zmodem_autostart_setting();
    elder_terms_settings_test::test_invalid_values_fall_back_to_defaults();
    elder_terms_settings_test::test_invalid_terminal_text_values_fall_back_to_type_defaults();
    elder_terms_settings_test::test_invalid_terminal_log_values_fall_back_to_defaults();
    elder_terms_settings_test::test_invalid_serial_values_fall_back_to_defaults();
    elder_terms_settings_test::test_invalid_ssh_values_fall_back_and_warn();
    elder_terms_settings_test::test_public_setting_keys();
    elder_terms_settings_test::test_save_settings_omits_default_values();
    elder_terms_settings_test::test_save_serial_settings_omits_default_values();
    elder_terms_settings_test::test_save_ssh_settings_omits_default_values();
    elder_terms_settings_test::test_save_sftp_settings_omits_default_values();
    elder_terms_settings_test::test_save_explicit_zmodem_autostart();
    elder_terms_settings_test::test_save_explicit_terminal_text_defaults();
    elder_terms_settings_test::test_save_explicit_value_equal_to_built_in();
    elder_terms_settings_test::test_save_terminal_log_settings();
    elder_terms_settings_test::test_save_general_color_settings();
    elder_terms_settings_test::test_save_settings_writes_empty_file_for_defaults();
    elder_terms_settings_test::test_load_settings_reports_file_read_status();
    elder_terms_settings_test::test_global_settings_layer_priority_and_sources();
    elder_terms_settings_test::test_invalid_layer_values_use_the_next_fallback();
    elder_terms_settings_test::test_global_settings_do_not_flatten_into_connection_files();
    elder_terms_settings_test::test_global_settings_editor_excludes_connection_name();
    elder_terms_settings_test::test_application_settings_are_global_only();
    elder_terms_settings_test::test_connection_open_hotkey_settings();
    elder_terms_settings_test::test_rebase_preserves_draft_overrides_and_dirty_state();
    elder_terms_settings_test::test_key_binding_conflicts_are_resolved_per_layer();
    elder_terms_settings_test::test_connection_macro_settings_round_trip_and_layering();
    elder_terms_settings_test::test_invalid_connection_macros_warn_and_are_ignored();
    elder_terms_settings_test::
        test_global_hyperlink_actions_override_built_in_defaults();
    elder_terms_settings_test::
        test_hyperlink_defaults_disable_and_invalid_rules();
    elder_terms_settings_test::test_missing_global_settings_are_optional();
    elder_terms_settings_test::test_save_empty_global_settings_creates_parent_directory();
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
