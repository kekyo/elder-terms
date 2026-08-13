#include <algorithm>
#include <utility>

#include "telnet-protocol.h"

namespace elder_terms {

static constexpr unsigned char telnet_se = 240;
static constexpr unsigned char telnet_brk = 243;
static constexpr unsigned char telnet_sb = 250;
static constexpr unsigned char telnet_will = 251;
static constexpr unsigned char telnet_wont = 252;
static constexpr unsigned char telnet_do = 253;
static constexpr unsigned char telnet_dont = 254;
static constexpr unsigned char telnet_iac = 255;
static constexpr unsigned char telnet_nul = 0;
static constexpr unsigned char telnet_lf = 10;
static constexpr unsigned char telnet_cr = 13;
static constexpr unsigned char telnet_option_binary = 0;
static constexpr unsigned char telnet_option_echo = 1;
static constexpr unsigned char telnet_option_suppress_go_ahead = 3;
static constexpr unsigned char telnet_option_terminal_type = 24;
static constexpr unsigned char telnet_option_naws = 31;
static constexpr unsigned char telnet_terminal_type_is = 0;
static constexpr unsigned char telnet_terminal_type_send = 1;

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

TelnetProtocol::TelnetProtocol(std::string terminal_type)
    : terminal_type(std::move(terminal_type)) {
}

void TelnetProtocol::handle_data_byte(unsigned char byte,
                                      TelnetProtocolResult *result) {
  if (byte == telnet_iac) {
    state = ParseState::command;
    return;
  }

  if (remote_binary_enabled) {
    result->terminal_data.push_back(byte);
    return;
  }

  if (byte == telnet_cr) {
    state = ParseState::data_cr;
    return;
  }

  if (byte == telnet_nul) {
    return;
  }

  result->terminal_data.push_back(byte);
}

void TelnetProtocol::handle_data_cr_byte(unsigned char byte,
                                         TelnetProtocolResult *result) {
  if (byte == telnet_lf) {
    result->terminal_data.push_back(telnet_cr);
    result->terminal_data.push_back(telnet_lf);
    state = ParseState::data;
    return;
  }

  if (byte == telnet_nul) {
    result->terminal_data.push_back(telnet_cr);
    state = ParseState::data;
    return;
  }

  result->terminal_data.push_back(telnet_cr);
  state = ParseState::data;
  handle_data_byte(byte, result);
}

void TelnetProtocol::handle_negotiation(unsigned char option,
                                        TelnetProtocolResult *result) {
  if (pending_command == telnet_do) {
    if (option == telnet_option_binary) {
      if (!local_binary_enabled && !local_binary_requested) {
        result->responses.push_back(
            negotiation_response(telnet_will, telnet_option_binary));
      }
      local_binary_enabled = true;
      local_binary_requested = false;
      return;
    }
    if (option == telnet_option_terminal_type) {
      if (!terminal_type_enabled) {
        result->responses.push_back(
            negotiation_response(telnet_will, telnet_option_terminal_type));
      }
      terminal_type_enabled = true;
      return;
    }
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
    if (option == telnet_option_binary) {
      local_binary_enabled = false;
      if (local_binary_requested) {
        binary_rejected = true;
      }
      local_binary_requested = false;
    }
    if (option == telnet_option_naws) {
      naws_enabled = false;
    }
    if (option == telnet_option_terminal_type) {
      terminal_type_enabled = false;
    }
    result->responses.push_back(negotiation_response(telnet_wont, option));
    return;
  }

  if (pending_command == telnet_will) {
    if (option == telnet_option_binary) {
      if (!remote_binary_enabled && !remote_binary_requested) {
        result->responses.push_back(
            negotiation_response(telnet_do, telnet_option_binary));
      }
      remote_binary_enabled = true;
      remote_binary_requested = false;
    } else if (option == telnet_option_echo ||
        option == telnet_option_suppress_go_ahead) {
      result->responses.push_back(negotiation_response(telnet_do, option));
    } else {
      result->responses.push_back(negotiation_response(telnet_dont, option));
    }
    return;
  }

  if (pending_command == telnet_wont) {
    if (option == telnet_option_binary) {
      remote_binary_enabled = false;
      if (remote_binary_requested) {
        binary_rejected = true;
      }
      remote_binary_requested = false;
    }
    result->responses.push_back(negotiation_response(telnet_dont, option));
  }
}

void TelnetProtocol::handle_suboption(TelnetProtocolResult *result) {
  if (terminal_type_enabled && suboption_bytes.size() >= 2 &&
      suboption_bytes[0] == telnet_option_terminal_type &&
      suboption_bytes[1] == telnet_terminal_type_send) {
    TelnetBytes response{telnet_iac, telnet_sb, telnet_option_terminal_type,
                         telnet_terminal_type_is};
    response.reserve(response.size() + terminal_type.size() + 2);
    for (unsigned char byte : terminal_type) {
      append_escaped_byte(&response, byte);
    }
    response.push_back(telnet_iac);
    response.push_back(telnet_se);
    result->responses.push_back(std::move(response));
  }
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

bool TelnetProtocol::is_binary_enabled() const {
  return local_binary_enabled && remote_binary_enabled;
}

bool TelnetProtocol::is_binary_rejected() const {
  return binary_rejected;
}

TelnetProtocolResult
TelnetProtocol::receive(std::span<const unsigned char> bytes) {
  TelnetProtocolResult result;
  for (unsigned char byte : bytes) {
    switch (state) {
    case ParseState::data:
      handle_data_byte(byte, &result);
      break;

    case ParseState::data_cr:
      handle_data_cr_byte(byte, &result);
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
        handle_suboption(&result);
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
  result.reserve(bytes.size() + 1);
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    const unsigned char byte = bytes[index];
    if (!local_binary_enabled && byte == telnet_cr) {
      append_escaped_byte(&result, telnet_cr);
      if (index + 1 < bytes.size() && bytes[index + 1] == telnet_lf) {
        append_escaped_byte(&result, telnet_lf);
        ++index;
      } else {
        append_escaped_byte(&result, telnet_nul);
      }
    } else {
      append_escaped_byte(&result, byte);
    }
  }
  return result;
}

void TelnetProtocol::begin_text_send_encoding() {
  text_send_pending_cr = false;
}

TelnetBytes
TelnetProtocol::encode_text_send(std::span<const unsigned char> bytes) {
  TelnetBytes result;
  result.reserve(bytes.size() + 2);

  std::size_t index = 0;
  if (text_send_pending_cr) {
    append_escaped_byte(&result, telnet_cr);
    if (!local_binary_enabled && !bytes.empty() && bytes.front() == telnet_lf) {
      append_escaped_byte(&result, telnet_lf);
      index = 1;
    } else {
      append_escaped_byte(&result, telnet_nul);
    }
    text_send_pending_cr = false;
  }

  for (; index < bytes.size(); ++index) {
    const unsigned char byte = bytes[index];
    if (!local_binary_enabled && byte == telnet_cr) {
      if (index + 1 == bytes.size()) {
        text_send_pending_cr = true;
      } else {
        append_escaped_byte(&result, telnet_cr);
        if (bytes[index + 1] == telnet_lf) {
          append_escaped_byte(&result, telnet_lf);
          ++index;
        } else {
          append_escaped_byte(&result, telnet_nul);
        }
      }
    } else {
      append_escaped_byte(&result, byte);
    }
  }
  return result;
}

TelnetBytes TelnetProtocol::finish_text_send_encoding() {
  TelnetBytes result;
  if (text_send_pending_cr) {
    append_escaped_byte(&result, telnet_cr);
    append_escaped_byte(&result, telnet_nul);
    text_send_pending_cr = false;
  }
  return result;
}

TelnetBytes TelnetProtocol::encode_break() const {
  return {telnet_iac, telnet_brk};
}

std::vector<TelnetBytes> TelnetProtocol::encode_enable_binary() {
  binary_rejected = false;
  if (!local_binary_enabled) {
    local_binary_requested = true;
  }
  if (!remote_binary_enabled) {
    remote_binary_requested = true;
  }

  std::vector<TelnetBytes> result;
  if (!local_binary_enabled) {
    result.push_back(negotiation_response(telnet_will,
                                          telnet_option_binary));
  }
  if (!remote_binary_enabled) {
    result.push_back(negotiation_response(telnet_do, telnet_option_binary));
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
