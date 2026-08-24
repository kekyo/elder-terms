#include "terminal-keyboard-protocol.h"

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace elder_terms {

class TerminalKeyboardProtocolState::Impl {
private:
  static constexpr std::size_t maximum_stack_depth = 64;
  static constexpr std::size_t maximum_csi_size = 64;
  static constexpr std::uint32_t disambiguate_escape_codes = 1;
  static constexpr std::uint32_t report_all_keys_as_escape_codes = 8;

  enum class ParserState {
    ground,
    escape,
    escape_intermediate,
    csi,
    csi_ignore,
    osc,
    osc_escape,
    control_string,
    control_string_escape,
  };

  ParserState parser_state = ParserState::ground;
  std::string csi_parameters;
  std::string csi_intermediates;
  std::vector<std::uint32_t> main_keyboard_modes;
  std::vector<std::uint32_t> alternate_keyboard_modes;
  bool alternate_screen = false;

  static std::optional<std::uint32_t>
  parse_decimal(std::string_view text, std::uint32_t default_value) {
    if (text.empty()) {
      return default_value;
    }

    std::uint32_t value = 0;
    for (const char character : text) {
      if (character < '0' || character > '9') {
        return std::nullopt;
      }
      const std::uint32_t digit =
          static_cast<std::uint32_t>(character - '0');
      if (value > (std::numeric_limits<std::uint32_t>::max() - digit) / 10) {
        return std::nullopt;
      }
      value = value * 10 + digit;
    }
    return value;
  }

  static bool contains_alternate_screen_mode(std::string_view parameters) {
    if (parameters.empty()) {
      return false;
    }

    std::size_t offset = 0;
    while (offset <= parameters.size()) {
      const std::size_t separator = parameters.find(';', offset);
      const std::size_t end = separator == std::string_view::npos
                                  ? parameters.size()
                                  : separator;
      const std::optional<std::uint32_t> value =
          parse_decimal(parameters.substr(offset, end - offset), 0);
      if (!value.has_value()) {
        return false;
      }
      if (*value == 47 || *value == 1047 || *value == 1049) {
        return true;
      }
      if (separator == std::string_view::npos) {
        return false;
      }
      offset = separator + 1;
    }
    return false;
  }

  std::vector<std::uint32_t> &active_keyboard_modes() {
    return alternate_screen ? alternate_keyboard_modes : main_keyboard_modes;
  }

  const std::vector<std::uint32_t> &active_keyboard_modes() const {
    return alternate_screen ? alternate_keyboard_modes : main_keyboard_modes;
  }

  void begin_csi() {
    csi_parameters.clear();
    csi_intermediates.clear();
    parser_state = ParserState::csi;
  }

  void push_keyboard_mode(std::uint32_t flags) {
    std::vector<std::uint32_t> &modes = active_keyboard_modes();
    if (modes.size() >= maximum_stack_depth) {
      modes.erase(modes.begin());
    }
    modes.push_back(flags);
  }

  void pop_keyboard_modes(std::uint32_t count) {
    std::vector<std::uint32_t> &modes = active_keyboard_modes();
    if (count >= modes.size()) {
      modes.clear();
      return;
    }
    modes.resize(modes.size() - count);
  }

  void reset_keyboard_modes() {
    main_keyboard_modes.clear();
    alternate_keyboard_modes.clear();
    alternate_screen = false;
  }

  void process_csi(unsigned char final_byte) {
    if (!csi_intermediates.empty() || csi_parameters.empty()) {
      return;
    }

    const char private_marker = csi_parameters.front();
    const std::string_view parameters(csi_parameters.data() + 1,
                                      csi_parameters.size() - 1);
    if (final_byte == 'u' && private_marker == '>') {
      const std::optional<std::uint32_t> flags = parse_decimal(parameters, 0);
      if (flags.has_value()) {
        push_keyboard_mode(*flags);
      }
      return;
    }
    if (final_byte == 'u' && private_marker == '<') {
      const std::optional<std::uint32_t> count = parse_decimal(parameters, 1);
      if (count.has_value()) {
        pop_keyboard_modes(*count);
      }
      return;
    }
    if ((final_byte == 'h' || final_byte == 'l') &&
        private_marker == '?' &&
        contains_alternate_screen_mode(parameters)) {
      alternate_screen = final_byte == 'h';
    }
  }

  void observe_ground(unsigned char byte) {
    switch (byte) {
    case 0x1b:
      parser_state = ParserState::escape;
      break;
    case 0x90:
    case 0x98:
    case 0x9e:
    case 0x9f:
      parser_state = ParserState::control_string;
      break;
    case 0x9b:
      begin_csi();
      break;
    case 0x9d:
      parser_state = ParserState::osc;
      break;
    default:
      break;
    }
  }

  void observe_escape(unsigned char byte) {
    switch (byte) {
    case 0x18:
    case 0x1a:
      parser_state = ParserState::ground;
      break;
    case 0x1b:
      break;
    case '[':
      begin_csi();
      break;
    case ']':
      parser_state = ParserState::osc;
      break;
    case 'P':
    case 'X':
    case '^':
    case '_':
      parser_state = ParserState::control_string;
      break;
    case 'c':
      reset_keyboard_modes();
      parser_state = ParserState::ground;
      break;
    default:
      if (byte >= 0x20 && byte <= 0x2f) {
        parser_state = ParserState::escape_intermediate;
      } else if (byte >= 0x30 && byte <= 0x7e) {
        parser_state = ParserState::ground;
      }
      break;
    }
  }

  void observe_escape_intermediate(unsigned char byte) {
    if (byte == 0x18 || byte == 0x1a) {
      parser_state = ParserState::ground;
    } else if (byte == 0x1b) {
      parser_state = ParserState::escape;
    } else if (byte >= 0x30 && byte <= 0x7e) {
      parser_state = ParserState::ground;
    }
  }

  void observe_csi(unsigned char byte) {
    if (byte == 0x18 || byte == 0x1a) {
      parser_state = ParserState::ground;
      return;
    }
    if (byte == 0x1b) {
      parser_state = ParserState::escape;
      return;
    }
    if (byte <= 0x1f) {
      return;
    }
    if (byte >= 0x30 && byte <= 0x3f) {
      if (!csi_intermediates.empty() ||
          csi_parameters.size() >= maximum_csi_size) {
        parser_state = ParserState::csi_ignore;
      } else {
        csi_parameters.push_back(static_cast<char>(byte));
      }
      return;
    }
    if (byte >= 0x20 && byte <= 0x2f) {
      if (csi_intermediates.size() >= maximum_csi_size) {
        parser_state = ParserState::csi_ignore;
      } else {
        csi_intermediates.push_back(static_cast<char>(byte));
      }
      return;
    }
    if (byte >= 0x40 && byte <= 0x7e) {
      process_csi(byte);
      parser_state = ParserState::ground;
      return;
    }
    parser_state = ParserState::csi_ignore;
  }

  void observe_csi_ignore(unsigned char byte) {
    if (byte == 0x18 || byte == 0x1a ||
        (byte >= 0x40 && byte <= 0x7e)) {
      parser_state = ParserState::ground;
    } else if (byte == 0x1b) {
      parser_state = ParserState::escape;
    }
  }

  void observe_osc(unsigned char byte) {
    if (byte == 0x07 || byte == 0x18 || byte == 0x1a || byte == 0x9c) {
      parser_state = ParserState::ground;
    } else if (byte == 0x1b) {
      parser_state = ParserState::osc_escape;
    }
  }

  void observe_osc_escape(unsigned char byte) {
    if (byte == '\\' || byte == 0x07 || byte == 0x18 || byte == 0x1a ||
        byte == 0x9c) {
      parser_state = ParserState::ground;
    } else if (byte != 0x1b) {
      parser_state = ParserState::osc;
    }
  }

  void observe_control_string(unsigned char byte) {
    if (byte == 0x18 || byte == 0x1a || byte == 0x9c) {
      parser_state = ParserState::ground;
    } else if (byte == 0x1b) {
      parser_state = ParserState::control_string_escape;
    }
  }

  void observe_control_string_escape(unsigned char byte) {
    if (byte == '\\' || byte == 0x18 || byte == 0x1a || byte == 0x9c) {
      parser_state = ParserState::ground;
    } else if (byte != 0x1b) {
      parser_state = ParserState::control_string;
    }
  }

public:
  void observe(std::span<const unsigned char> bytes) {
    for (const unsigned char byte : bytes) {
      switch (parser_state) {
      case ParserState::ground:
        observe_ground(byte);
        break;
      case ParserState::escape:
        observe_escape(byte);
        break;
      case ParserState::escape_intermediate:
        observe_escape_intermediate(byte);
        break;
      case ParserState::csi:
        observe_csi(byte);
        break;
      case ParserState::csi_ignore:
        observe_csi_ignore(byte);
        break;
      case ParserState::osc:
        observe_osc(byte);
        break;
      case ParserState::osc_escape:
        observe_osc_escape(byte);
        break;
      case ParserState::control_string:
        observe_control_string(byte);
        break;
      case ParserState::control_string_escape:
        observe_control_string_escape(byte);
        break;
      }
    }
  }

  bool modified_special_keys_enabled() const {
    const std::vector<std::uint32_t> &modes = active_keyboard_modes();
    if (modes.empty()) {
      return false;
    }
    const std::uint32_t flags = modes.back();
    return (flags & (disambiguate_escape_codes |
                     report_all_keys_as_escape_codes)) != 0;
  }
};

static std::uint32_t modifier_bits(const TerminalKeyModifiers &modifiers) {
  std::uint32_t bits = 0;
  if (modifiers.shift) {
    bits |= 1;
  }
  if (modifiers.alt) {
    bits |= 2;
  }
  if (modifiers.control) {
    bits |= 4;
  }
  if (modifiers.super) {
    bits |= 8;
  }
  if (modifiers.hyper) {
    bits |= 16;
  }
  if (modifiers.meta) {
    bits |= 32;
  }
  return bits;
}

static std::uint32_t special_key_code(TerminalSpecialKey key) {
  switch (key) {
  case TerminalSpecialKey::enter:
    return 13;
  case TerminalSpecialKey::keypad_enter:
    return 57414;
  case TerminalSpecialKey::tab:
    return 9;
  case TerminalSpecialKey::backspace:
    return 127;
  case TerminalSpecialKey::escape:
    return 27;
  case TerminalSpecialKey::space:
    return 32;
  }
  return 0;
}

TerminalKeyboardProtocolState::TerminalKeyboardProtocolState()
    : impl(std::make_unique<Impl>()) {
}

TerminalKeyboardProtocolState::~TerminalKeyboardProtocolState() = default;

void TerminalKeyboardProtocolState::observe(
    std::span<const unsigned char> bytes) {
  impl->observe(bytes);
}

bool TerminalKeyboardProtocolState::modified_special_keys_enabled() const {
  return impl->modified_special_keys_enabled();
}

std::optional<std::string>
encode_modified_special_key(TerminalSpecialKey key,
                            const TerminalKeyModifiers &modifiers) {
  const std::uint32_t bits = modifier_bits(modifiers);
  if (bits == 0) {
    return std::nullopt;
  }
  if (key == TerminalSpecialKey::space && bits == 1) {
    return std::nullopt;
  }

  return "\x1b[" + std::to_string(special_key_code(key)) + ";" +
         std::to_string(bits + 1) + "u";
}

bool is_legacy_modified_special_key_commit(TerminalSpecialKey key,
                                           std::string_view commit) {
  switch (key) {
  case TerminalSpecialKey::enter:
  case TerminalSpecialKey::keypad_enter:
    return commit == "\r" || commit == "\n" || commit == "\r\n" ||
           commit == "\x1b\r" || commit == "\x1b\n" ||
           commit == "\x1b\r\n";
  case TerminalSpecialKey::tab:
    return commit == "\t" || commit == "\x1b\t" || commit == "\x1b[Z";
  case TerminalSpecialKey::backspace:
    return commit == "\x08" || commit == "\x7f" ||
           commit == "\x1b\x08" || commit == "\x1b\x7f";
  case TerminalSpecialKey::escape:
    return commit == "\x1b" || commit == "\x1b\x1b";
  case TerminalSpecialKey::space:
    return commit == " " || commit == "\x1b " ||
           commit == std::string_view("\0", 1) ||
           commit == std::string_view("\x1b\0", 2);
  }
  return false;
}

} // namespace elder_terms
