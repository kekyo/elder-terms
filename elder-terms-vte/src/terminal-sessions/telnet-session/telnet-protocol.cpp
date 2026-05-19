#include <algorithm>

#include "telnet-protocol.h"

namespace elder_terms {

static constexpr unsigned char telnet_se = 240;
static constexpr unsigned char telnet_sb = 250;
static constexpr unsigned char telnet_will = 251;
static constexpr unsigned char telnet_wont = 252;
static constexpr unsigned char telnet_do = 253;
static constexpr unsigned char telnet_dont = 254;
static constexpr unsigned char telnet_iac = 255;
static constexpr unsigned char telnet_option_echo = 1;
static constexpr unsigned char telnet_option_suppress_go_ahead = 3;
static constexpr unsigned char telnet_option_naws = 31;

static void append_escaped_byte(TelnetBytes *bytes, unsigned char value) {
  bytes->push_back(value);
  if (value == telnet_iac) {
    bytes->push_back(telnet_iac);
  }
}

static TelnetBytes negotiation_response(unsigned char command,
                                        unsigned char option) {
  return TelnetBytes{telnet_iac, command, option};
}

void TelnetProtocol::handle_negotiation(unsigned char option,
                                        TelnetProtocolResult *result) {
  if (pending_command == telnet_do) {
    if (option == telnet_option_naws) {
      if (!naws_enabled) {
        result->responses.push_back(
            negotiation_response(telnet_will, telnet_option_naws));
      }
      naws_enabled = true;
      result->responses.push_back(encode_naws());
    } else {
      result->responses.push_back(negotiation_response(telnet_wont, option));
    }
    return;
  }

  if (pending_command == telnet_dont) {
    if (option == telnet_option_naws) {
      naws_enabled = false;
    }
    result->responses.push_back(negotiation_response(telnet_wont, option));
    return;
  }

  if (pending_command == telnet_will) {
    if (option == telnet_option_echo ||
        option == telnet_option_suppress_go_ahead) {
      result->responses.push_back(negotiation_response(telnet_do, option));
    } else {
      result->responses.push_back(negotiation_response(telnet_dont, option));
    }
    return;
  }

  if (pending_command == telnet_wont) {
    result->responses.push_back(negotiation_response(telnet_dont, option));
  }
}

void TelnetProtocol::handle_suboption() {
  suboption_bytes.clear();
}

void TelnetProtocol::set_window_size(std::uint16_t next_columns,
                                     std::uint16_t next_rows) {
  columns = next_columns;
  rows = next_rows;
}

bool TelnetProtocol::is_naws_enabled() const {
  return naws_enabled;
}

TelnetProtocolResult
TelnetProtocol::receive(std::span<const unsigned char> bytes) {
  TelnetProtocolResult result;
  for (unsigned char byte : bytes) {
    switch (state) {
    case ParseState::data:
      if (byte == telnet_iac) {
        state = ParseState::command;
      } else {
        result.terminal_data.push_back(byte);
      }
      break;

    case ParseState::command:
      if (byte == telnet_iac) {
        result.terminal_data.push_back(telnet_iac);
        state = ParseState::data;
      } else if (byte == telnet_will || byte == telnet_wont ||
                 byte == telnet_do || byte == telnet_dont) {
        pending_command = byte;
        state = ParseState::negotiation;
      } else if (byte == telnet_sb) {
        suboption_bytes.clear();
        state = ParseState::suboption;
      } else {
        state = ParseState::data;
      }
      break;

    case ParseState::negotiation:
      handle_negotiation(byte, &result);
      state = ParseState::data;
      break;

    case ParseState::suboption:
      if (byte == telnet_iac) {
        state = ParseState::suboption_command;
      } else {
        suboption_bytes.push_back(byte);
      }
      break;

    case ParseState::suboption_command:
      if (byte == telnet_iac) {
        suboption_bytes.push_back(telnet_iac);
        state = ParseState::suboption;
      } else if (byte == telnet_se) {
        handle_suboption();
        state = ParseState::data;
      } else {
        state = ParseState::data;
      }
      break;
    }
  }

  return result;
}

TelnetBytes
TelnetProtocol::encode_user_input(std::span<const unsigned char> bytes) const {
  TelnetBytes result;
  result.reserve(bytes.size());
  for (unsigned char byte : bytes) {
    append_escaped_byte(&result, byte);
  }
  return result;
}

TelnetBytes TelnetProtocol::encode_naws() const {
  TelnetBytes result{telnet_iac, telnet_sb, telnet_option_naws};
  append_escaped_byte(&result, static_cast<unsigned char>(columns >> 8U));
  append_escaped_byte(&result, static_cast<unsigned char>(columns & 0xffU));
  append_escaped_byte(&result, static_cast<unsigned char>(rows >> 8U));
  append_escaped_byte(&result, static_cast<unsigned char>(rows & 0xffU));
  result.push_back(telnet_iac);
  result.push_back(telnet_se);
  return result;
}

} // namespace elder_terms
