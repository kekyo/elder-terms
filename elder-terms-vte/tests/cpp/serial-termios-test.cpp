#include "../../src/terminal-sessions/serial-session/serial-termios.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace elder_terms_serial_termios_test {

static elder_terms::SerialConnectionSettings default_settings() {
  return {
      .device = "/dev/ttyUSB0",
      .baudrate = 115200,
      .bits = 8,
      .parity = elder_terms::SerialParity::none,
      .stop_bit = 1,
      .flow_control = elder_terms::SerialFlowControl::none,
      .carrier_detect = elder_terms::SerialCarrierDetect::cd,
  };
}

static void expect_true(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

static void test_default_configuration_is_8n1_without_flow_control() {
  const elder_terms::SerialTermiosConfiguration configuration =
      elder_terms::serial_termios_configuration(default_settings());

  expect_true((configuration.control_set & CS8) != 0,
              "default serial bits should set CS8");
  expect_true((configuration.control_set & PARENB) == 0,
              "default serial parity should not set PARENB");
  expect_true((configuration.control_set & CSTOPB) == 0,
              "default serial stop bit should not set CSTOPB");
  expect_true((configuration.input_set & (IXON | IXOFF)) == 0,
              "default serial flow control should not set XON/XOFF");
  expect_true(!configuration.custom_baudrate,
              "default serial baudrate should use a standard termios speed");
  expect_true(configuration.standard_speed == B115200,
              "default serial baudrate should be B115200");
}

static void test_even_7e2_xon_configuration() {
  elder_terms::SerialConnectionSettings settings = default_settings();
  settings.bits = 7;
  settings.parity = elder_terms::SerialParity::even;
  settings.stop_bit = 2;
  settings.flow_control = elder_terms::SerialFlowControl::xon;

  const elder_terms::SerialTermiosConfiguration configuration =
      elder_terms::serial_termios_configuration(settings);

  expect_true((configuration.control_set & CS7) != 0,
              "7 data bits should set CS7");
  expect_true((configuration.control_set & PARENB) != 0,
              "even parity should set PARENB");
  expect_true((configuration.control_set & PARODD) == 0,
              "even parity should not set PARODD");
  expect_true((configuration.control_set & CSTOPB) != 0,
              "two stop bits should set CSTOPB");
  expect_true((configuration.input_set & (IXON | IXOFF)) == (IXON | IXOFF),
              "XON flow control should set IXON and IXOFF");
}

static void test_odd_parity_and_custom_baudrate_configuration() {
  elder_terms::SerialConnectionSettings settings = default_settings();
  settings.baudrate = 1234567;
  settings.parity = elder_terms::SerialParity::odd;

  const elder_terms::SerialTermiosConfiguration configuration =
      elder_terms::serial_termios_configuration(settings);

  expect_true((configuration.control_set & PARENB) != 0,
              "odd parity should set PARENB");
  expect_true((configuration.control_set & PARODD) != 0,
              "odd parity should set PARODD");
  expect_true(configuration.custom_baudrate,
              "non-standard baudrate should use custom termios2 speed");
  expect_true(configuration.baudrate == 1234567,
              "custom baudrate should preserve the requested value");
}

static void test_hardware_flow_control_configuration() {
#ifdef CRTSCTS
  elder_terms::SerialConnectionSettings settings = default_settings();
  settings.flow_control = elder_terms::SerialFlowControl::hard;

  const elder_terms::SerialTermiosConfiguration configuration =
      elder_terms::serial_termios_configuration(settings);

  expect_true((configuration.control_set & CRTSCTS) != 0,
              "hardware flow control should set CRTSCTS when available");
#else
  bool failed = false;
  try {
    elder_terms::SerialConnectionSettings settings = default_settings();
    settings.flow_control = elder_terms::SerialFlowControl::hard;
    (void)elder_terms::serial_termios_configuration(settings);
  } catch (const std::runtime_error &) {
    failed = true;
  }
  expect_true(failed,
              "hardware flow control should fail when CRTSCTS is unavailable");
#endif
}

} // namespace elder_terms_serial_termios_test

int main() {
  try {
    elder_terms_serial_termios_test::
        test_default_configuration_is_8n1_without_flow_control();
    elder_terms_serial_termios_test::test_even_7e2_xon_configuration();
    elder_terms_serial_termios_test::
        test_odd_parity_and_custom_baudrate_configuration();
    elder_terms_serial_termios_test::test_hardware_flow_control_configuration();
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
