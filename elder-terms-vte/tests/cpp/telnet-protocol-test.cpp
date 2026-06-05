#include "../../src/terminal-sessions/telnet-session/telnet-protocol.h"

#include <initializer_list>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>

namespace elder_terms {

static TelnetProtocolResult receive_bytes(
    TelnetProtocol *protocol, std::initializer_list<unsigned char> bytes) {
  const TelnetBytes input(bytes);
  return protocol->receive(std::span<const unsigned char>(input.data(),
                                                          input.size()));
}

static void expect_bytes(std::span<const unsigned char> actual,
                         std::initializer_list<unsigned char> expected,
                         const char *message) {
  const TelnetBytes expected_bytes(expected);
  const TelnetBytes actual_bytes(actual.begin(), actual.end());
  if (actual_bytes != expected_bytes) {
    throw std::runtime_error(message);
  }
}

static void expect_bytes(const TelnetBytes &actual,
                         std::initializer_list<unsigned char> expected,
                         const char *message) {
  expect_bytes(std::span<const unsigned char>(actual.data(), actual.size()),
               expected, message);
}

static void expect_response(const TelnetProtocolResult &result,
                            std::size_t index,
                            std::initializer_list<unsigned char> expected,
                            const char *message) {
  if (index >= result.responses.size()) {
    throw std::runtime_error(message);
  }
  expect_bytes(result.responses[index], expected, message);
}

static void plain_data_passes_to_terminal() {
  TelnetProtocol protocol;
  const TelnetProtocolResult result = receive_bytes(&protocol, {'h', 'i'});

  expect_bytes(result.terminal_data, {'h', 'i'},
               "plain TELNET data should be terminal data");
  if (!result.responses.empty()) {
    throw std::runtime_error("plain TELNET data should not produce responses");
  }
}

static void iac_escaped_data_passes_to_terminal() {
  TelnetProtocol protocol;
  (void)receive_bytes(&protocol, {255});
  const TelnetProtocolResult result = receive_bytes(&protocol, {255});

  expect_bytes(result.terminal_data, {255},
               "IAC IAC should produce one terminal IAC byte");
}

static void do_naws_enables_and_sends_window_size() {
  TelnetProtocol protocol;
  protocol.set_window_size(80, 24);
  const TelnetProtocolResult result = receive_bytes(&protocol, {255, 253, 31});

  if (!protocol.is_naws_enabled()) {
    throw std::runtime_error("DO NAWS should enable NAWS");
  }
  if (result.responses.size() != 2) {
    throw std::runtime_error("DO NAWS should produce WILL and NAWS responses");
  }
  expect_response(result, 0, {255, 251, 31}, "DO NAWS should send WILL NAWS");
  expect_response(result, 1, {255, 250, 31, 0, 80, 0, 24, 255, 240},
                  "DO NAWS should send the current window size");
}

static void naws_escapes_iac_sized_fields() {
  TelnetProtocol protocol;
  protocol.set_window_size(255, 255);
  const TelnetBytes response = protocol.encode_naws();

  expect_bytes(response, {255, 250, 31, 0, 255, 255, 0, 255, 255, 255, 240},
               "NAWS fields containing IAC should be escaped");
}

static void supported_will_options_are_accepted() {
  TelnetProtocol protocol;
  const TelnetProtocolResult echo = receive_bytes(&protocol, {255, 251, 1});
  const TelnetProtocolResult suppress_go_ahead =
      receive_bytes(&protocol, {255, 251, 3});

  expect_response(echo, 0, {255, 253, 1}, "WILL ECHO should send DO ECHO");
  expect_response(suppress_go_ahead, 0, {255, 253, 3},
                  "WILL SUPPRESS-GO-AHEAD should be accepted");
}

static void unsupported_options_are_rejected() {
  TelnetProtocol protocol;
  const TelnetProtocolResult do_option = receive_bytes(&protocol, {255, 253, 42});
  const TelnetProtocolResult will_option =
      receive_bytes(&protocol, {255, 251, 42});

  expect_response(do_option, 0, {255, 252, 42},
                  "Unsupported DO should send WONT");
  expect_response(will_option, 0, {255, 254, 42},
                  "Unsupported WILL should send DONT");
}

static void binary_negotiation_request_enables_both_directions() {
  TelnetProtocol protocol;
  const std::vector<TelnetBytes> requests = protocol.encode_enable_binary();

  if (requests.size() != 2) {
    throw std::runtime_error("BINARY negotiation should request both directions");
  }
  expect_bytes(requests[0], {255, 251, 0},
               "BINARY negotiation should send WILL BINARY");
  expect_bytes(requests[1], {255, 253, 0},
               "BINARY negotiation should send DO BINARY");

  const TelnetProtocolResult do_binary =
      receive_bytes(&protocol, {255, 253, 0});
  const TelnetProtocolResult will_binary =
      receive_bytes(&protocol, {255, 251, 0});
  if (!do_binary.responses.empty() || !will_binary.responses.empty()) {
    throw std::runtime_error(
        "Requested BINARY acknowledgements should not be echoed");
  }
  if (!protocol.is_binary_enabled()) {
    throw std::runtime_error("DO/WILL BINARY should enable both directions");
  }
}

static void binary_negotiation_rejects_dont_or_wont() {
  TelnetProtocol dont_protocol;
  (void)dont_protocol.encode_enable_binary();
  (void)receive_bytes(&dont_protocol, {255, 254, 0});
  if (!dont_protocol.is_binary_rejected()) {
    throw std::runtime_error("DONT BINARY should reject requested BINARY");
  }

  TelnetProtocol wont_protocol;
  (void)wont_protocol.encode_enable_binary();
  (void)receive_bytes(&wont_protocol, {255, 252, 0});
  if (!wont_protocol.is_binary_rejected()) {
    throw std::runtime_error("WONT BINARY should reject requested BINARY");
  }
}

static void user_input_escapes_iac() {
  TelnetProtocol protocol;
  const TelnetBytes input{'a', 255, 'b'};
  const TelnetBytes encoded = protocol.encode_user_input(
      std::span<const unsigned char>(input.data(), input.size()));

  expect_bytes(encoded, {'a', 255, 255, 'b'},
               "User input should escape IAC bytes");
}

static void user_input_preserves_ascii_del() {
  TelnetProtocol protocol;
  const TelnetBytes input{127};
  const TelnetBytes encoded = protocol.encode_user_input(
      std::span<const unsigned char>(input.data(), input.size()));

  expect_bytes(encoded, {127}, "User input should preserve ASCII DEL");
}

} // namespace elder_terms

int main() {
  try {
    elder_terms::plain_data_passes_to_terminal();
    elder_terms::iac_escaped_data_passes_to_terminal();
    elder_terms::do_naws_enables_and_sends_window_size();
    elder_terms::naws_escapes_iac_sized_fields();
    elder_terms::supported_will_options_are_accepted();
    elder_terms::unsupported_options_are_rejected();
    elder_terms::binary_negotiation_request_enables_both_directions();
    elder_terms::binary_negotiation_rejects_dont_or_wont();
    elder_terms::user_input_escapes_iac();
    elder_terms::user_input_preserves_ascii_del();
  } catch (const std::exception &error) {
    std::cerr << "telnet-protocol-test: FAIL: " << error.what() << '\n';
    return 1;
  }

  std::cout << "telnet-protocol-test: PASS" << '\n';
  return 0;
}
