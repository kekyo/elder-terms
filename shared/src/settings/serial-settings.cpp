#include <elder-terms/settings/serial-settings.h>

#include <algorithm>
#include <cctype>
#include <utility>

namespace elder_terms {

static constexpr gint64 default_serial_baudrate = 115200;
static constexpr gint64 default_serial_bits = 8;
static constexpr gint64 default_serial_stop_bit = 1;
static constexpr char serial_section[] = "serial";
static constexpr char serial_device_key[] = "device";
static constexpr char serial_baudrate_key[] = "baudrate";
static constexpr char serial_bits_key[] = "bits";
static constexpr char serial_parity_key[] = "parity";
static constexpr char serial_stop_bit_key[] = "stop_bit";
static constexpr char serial_flow_control_key[] = "flow_control";
static constexpr char serial_carrier_detect_key[] = "carrier_detect";
static constexpr char serial_parity_none[] = "n";
static constexpr char serial_parity_even[] = "e";
static constexpr char serial_parity_odd[] = "o";
static constexpr char serial_flow_control_none[] = "none";
static constexpr char serial_flow_control_xon[] = "xon";
static constexpr char serial_flow_control_hard[] = "hard";
static constexpr char serial_carrier_detect_cd[] = "cd";
static constexpr char serial_carrier_detect_cts[] = "cts";
static constexpr char serial_carrier_detect_dsr[] = "dsr";

static bool string_is_blank(const std::string &value) {
  return std::all_of(value.begin(), value.end(), [](unsigned char character) {
    return std::isspace(character) != 0;
  });
}

static bool validate_baudrate(const SettingValue &value, std::string *reason) {
  const auto *integer = std::get_if<gint64>(&value);
  if (integer == nullptr || *integer < 150 || *integer > 8000000) {
    *reason = "must be an integer between 150 and 8000000";
    return false;
  }
  return true;
}

static bool validate_bits(const SettingValue &value, std::string *reason) {
  const auto *integer = std::get_if<gint64>(&value);
  if (integer == nullptr ||
      (*integer != 5 && *integer != 6 && *integer != 7 && *integer != 8)) {
    *reason = "must be 5, 6, 7, or 8";
    return false;
  }
  return true;
}

static bool validate_stop_bit(const SettingValue &value, std::string *reason) {
  const auto *integer = std::get_if<gint64>(&value);
  if (integer == nullptr || (*integer != 1 && *integer != 2)) {
    *reason = "must be 1 or 2";
    return false;
  }
  return true;
}

static bool validate_parity(const SettingValue &value, std::string *reason) {
  const auto *text = std::get_if<std::string>(&value);
  if (text == nullptr ||
      (*text != serial_parity_none && *text != serial_parity_even &&
       *text != serial_parity_odd)) {
    *reason = "must be n, e, or o";
    return false;
  }
  return true;
}

static bool validate_flow_control(const SettingValue &value,
                                  std::string *reason) {
  const auto *text = std::get_if<std::string>(&value);
  if (text == nullptr ||
      (*text != serial_flow_control_none &&
       *text != serial_flow_control_xon &&
       *text != serial_flow_control_hard)) {
    *reason = "must be none, xon, or hard";
    return false;
  }
  return true;
}

static bool validate_carrier_detect(const SettingValue &value,
                                    std::string *reason) {
  const auto *text = std::get_if<std::string>(&value);
  if (text == nullptr ||
      (*text != serial_carrier_detect_cd &&
       *text != serial_carrier_detect_cts &&
       *text != serial_carrier_detect_dsr)) {
    *reason = "must be cd, cts, or dsr";
    return false;
  }
  return true;
}

static SettingKey serial_key(const char *name) {
  return make_setting_key(serial_section, name);
}

static SerialParity serial_parity_from_string(const std::string &value) {
  if (value == serial_parity_even) {
    return SerialParity::even;
  }
  if (value == serial_parity_odd) {
    return SerialParity::odd;
  }
  return SerialParity::none;
}

static SerialFlowControl
serial_flow_control_from_string(const std::string &value) {
  if (value == serial_flow_control_xon) {
    return SerialFlowControl::xon;
  }
  if (value == serial_flow_control_hard) {
    return SerialFlowControl::hard;
  }
  return SerialFlowControl::none;
}

static SerialCarrierDetect
serial_carrier_detect_from_string(const std::string &value) {
  if (value == serial_carrier_detect_cts) {
    return SerialCarrierDetect::cts;
  }
  if (value == serial_carrier_detect_dsr) {
    return SerialCarrierDetect::dsr;
  }
  return SerialCarrierDetect::cd;
}

SettingKey serial_device_setting_key() {
  return serial_key(serial_device_key);
}

SettingKey serial_baudrate_setting_key() {
  return serial_key(serial_baudrate_key);
}

SettingKey serial_bits_setting_key() {
  return serial_key(serial_bits_key);
}

SettingKey serial_parity_setting_key() {
  return serial_key(serial_parity_key);
}

SettingKey serial_stop_bit_setting_key() {
  return serial_key(serial_stop_bit_key);
}

SettingKey serial_flow_control_setting_key() {
  return serial_key(serial_flow_control_key);
}

SettingKey serial_carrier_detect_setting_key() {
  return serial_key(serial_carrier_detect_key);
}

std::vector<SettingDefinition> serial_connection_setting_definitions() {
  return {
      {
          .key = serial_device_setting_key(),
          .default_value = SettingValue{std::string()},
          .validate = nullptr,
      },
      {
          .key = serial_baudrate_setting_key(),
          .default_value = SettingValue{default_serial_baudrate},
          .validate = validate_baudrate,
      },
      {
          .key = serial_bits_setting_key(),
          .default_value = SettingValue{default_serial_bits},
          .validate = validate_bits,
      },
      {
          .key = serial_parity_setting_key(),
          .default_value = SettingValue{std::string(serial_parity_none)},
          .validate = validate_parity,
      },
      {
          .key = serial_stop_bit_setting_key(),
          .default_value = SettingValue{default_serial_stop_bit},
          .validate = validate_stop_bit,
      },
      {
          .key = serial_flow_control_setting_key(),
          .default_value =
              SettingValue{std::string(serial_flow_control_none)},
          .validate = validate_flow_control,
      },
      {
          .key = serial_carrier_detect_setting_key(),
          .default_value =
              SettingValue{std::string(serial_carrier_detect_cd)},
          .validate = validate_carrier_detect,
      },
  };
}

SerialConnectionSettings
serial_connection_settings(const SettingsStore &store) {
  std::string device = setting_string_value_or_default(
      store, serial_device_setting_key(), std::string());
  if (string_is_blank(device)) {
    device.clear();
  }

  return {
      .device = std::move(device),
      .baudrate = setting_integer_value_or_default(
          store, serial_baudrate_setting_key(), default_serial_baudrate),
      .bits = setting_integer_value_or_default(
          store, serial_bits_setting_key(), default_serial_bits),
      .parity = serial_parity_from_string(setting_string_value_or_default(
          store, serial_parity_setting_key(), serial_parity_none)),
      .stop_bit = setting_integer_value_or_default(
          store, serial_stop_bit_setting_key(), default_serial_stop_bit),
      .flow_control =
          serial_flow_control_from_string(setting_string_value_or_default(
              store, serial_flow_control_setting_key(),
              serial_flow_control_none)),
      .carrier_detect =
          serial_carrier_detect_from_string(setting_string_value_or_default(
              store, serial_carrier_detect_setting_key(),
              serial_carrier_detect_cd)),
  };
}

std::string serial_parity_to_string(SerialParity parity) {
  if (parity == SerialParity::even) {
    return serial_parity_even;
  }
  if (parity == SerialParity::odd) {
    return serial_parity_odd;
  }
  return serial_parity_none;
}

std::string serial_flow_control_to_string(SerialFlowControl flow_control) {
  if (flow_control == SerialFlowControl::xon) {
    return serial_flow_control_xon;
  }
  if (flow_control == SerialFlowControl::hard) {
    return serial_flow_control_hard;
  }
  return serial_flow_control_none;
}

std::string
serial_carrier_detect_to_string(SerialCarrierDetect carrier_detect) {
  if (carrier_detect == SerialCarrierDetect::cts) {
    return serial_carrier_detect_cts;
  }
  if (carrier_detect == SerialCarrierDetect::dsr) {
    return serial_carrier_detect_dsr;
  }
  return serial_carrier_detect_cd;
}

void append_serial_connection_warnings(const SettingsStore &store,
                                       std::vector<std::string> *warnings) {
  const std::string device = setting_string_value_or_default(
      store, serial_device_setting_key(), std::string());
  if (string_is_blank(device)) {
    warnings->push_back(
        "Warning: missing required configuration value [serial] device; "
        "serial session will not connect");
  }
}

} // namespace elder_terms
