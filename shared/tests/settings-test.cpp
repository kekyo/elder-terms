#include <elder-terms/settings.h>

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
using elder_terms::default_terminal_display_settings;
using elder_terms::general_type_setting_key;
using elder_terms::load_settings;
using elder_terms::LocalShellConnectionSettings;
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
using elder_terms::set_setting_value;
using elder_terms::SettingsLoadOptions;
using elder_terms::SettingsLoadResult;
using elder_terms::SettingsSaveResult;
using elder_terms::SettingsStore;
using elder_terms::TelnetConnectionSettings;
using elder_terms::telnet_address_setting_key;
using elder_terms::telnet_port_setting_key;
using elder_terms::terminal_auto_close_setting_key;
using elder_terms::TerminalConnectionKind;
using elder_terms::TerminalConnectionProfile;
using elder_terms::TerminalDisplaySettings;
using elder_terms::terminal_auto_close;
using elder_terms::terminal_connection_profile;
using elder_terms::terminal_display_settings;
using elder_terms::terminal_height_setting_key;
using elder_terms::terminal_width_setting_key;
using elder_terms::terminal_zoom_setting_key;
using elder_terms::transfer_base_path;
using elder_terms::transfer_base_path_setting_key;

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
  expect_true(transfer_base_path(store).empty(),
              "default transfer base path should be empty");

  const TerminalConnectionProfile profile = terminal_connection_profile(store);
  expect_true(profile.kind == TerminalConnectionKind::local_shell,
              "default connection should be local shell");
  expect_true(std::holds_alternative<LocalShellConnectionSettings>(
                  profile.settings),
              "default connection settings should be local shell settings");
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
  set_setting_value(&store, telnet_address_setting_key(),
                    elder_terms::SettingValue{std::string("host.example")});
  set_setting_value(&store, telnet_port_setting_key(),
                    elder_terms::SettingValue{gint64{23}});
  set_setting_value(
      &store, transfer_base_path_setting_key(),
      elder_terms::SettingValue{std::string("file:///tmp/downloads")});

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
  expect_true(content.find("[telnet]") != std::string::npos,
              "saved settings should include non-default TELNET section");
  expect_true(content.find("address=host.example") != std::string::npos,
              "saved settings should include non-default TELNET address");
  expect_true(content.find("[transfer]") != std::string::npos,
              "saved settings should include non-default transfer section");
  expect_true(content.find("base_path=file:///tmp/downloads") !=
                  std::string::npos,
              "saved settings should include non-default transfer base_path");
  expect_true(content.find("height=") == std::string::npos,
              "saved settings should omit default terminal height");
  expect_true(content.find("zoom=") == std::string::npos,
              "saved settings should omit default terminal zoom");
  expect_true(content.find("port=") == std::string::npos,
              "saved settings should omit default TELNET port");
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

} // namespace elder_terms_settings_test

int main() {
  try {
    elder_terms_settings_test::test_default_settings();
    elder_terms_settings_test::test_telnet_profile();
    elder_terms_settings_test::test_serial_profile();
    elder_terms_settings_test::test_transfer_base_path_setting();
    elder_terms_settings_test::test_invalid_values_fall_back_to_defaults();
    elder_terms_settings_test::test_invalid_serial_values_fall_back_to_defaults();
    elder_terms_settings_test::test_public_setting_keys();
    elder_terms_settings_test::test_save_settings_omits_default_values();
    elder_terms_settings_test::test_save_serial_settings_omits_default_values();
    elder_terms_settings_test::test_save_settings_writes_empty_file_for_defaults();
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
