#include "terminal-zmodem-auto-start.h"

#include <cstddef>
#include <cstdint>

namespace elder_terms {

static constexpr char zmodem_pad = '*';
static constexpr char zmodem_zdle = 0x18;
static constexpr char zmodem_zhex = 'B';
static constexpr std::uint8_t zmodem_zrqinit = 0;
static constexpr std::uint8_t zmodem_zrinit = 1;
static constexpr std::size_t zhex_header_digit_count = 14;
static constexpr std::size_t detector_tail_limit = 32;

static std::optional<std::uint8_t> decode_hex_digit(char value) {
  if (value >= '0' && value <= '9') {
    return static_cast<std::uint8_t>(value - '0');
  }
  if (value >= 'a' && value <= 'f') {
    return static_cast<std::uint8_t>(10 + (value - 'a'));
  }
  if (value >= 'A' && value <= 'F') {
    return static_cast<std::uint8_t>(10 + (value - 'A'));
  }
  return std::nullopt;
}

static std::optional<std::uint8_t>
decode_hex_byte(std::string_view digits, std::size_t offset) {
  if (offset + 1 >= digits.size()) {
    return std::nullopt;
  }

  const std::optional<std::uint8_t> high = decode_hex_digit(digits[offset]);
  const std::optional<std::uint8_t> low = decode_hex_digit(digits[offset + 1]);
  if (!high.has_value() || !low.has_value()) {
    return std::nullopt;
  }
  return static_cast<std::uint8_t>((*high << 4) | *low);
}

static std::uint16_t zmodem_crc16_entry(std::uint8_t index) {
  std::uint16_t crc = static_cast<std::uint16_t>(index) << 8;
  for (unsigned int bit = 0; bit < 8; ++bit) {
    crc = (crc & 0x8000) != 0
              ? static_cast<std::uint16_t>((crc << 1) ^ 0x1021)
              : static_cast<std::uint16_t>(crc << 1);
  }
  return crc;
}

static std::uint16_t update_zmodem_crc16(std::uint16_t crc,
                                         std::uint8_t value) {
  return static_cast<std::uint16_t>(
      zmodem_crc16_entry(static_cast<std::uint8_t>(crc >> 8)) ^
      static_cast<std::uint16_t>(crc << 8) ^ value);
}

static std::uint16_t compute_zmodem_header_crc16(
    std::uint8_t type, const std::uint8_t header[4]) {
  std::uint16_t crc =
      update_zmodem_crc16(0, static_cast<std::uint8_t>(type & 0x7f));
  for (std::size_t index = 0; index < 4; ++index) {
    crc = update_zmodem_crc16(crc, header[index]);
  }
  crc = update_zmodem_crc16(crc, 0);
  crc = update_zmodem_crc16(crc, 0);
  return crc;
}

static std::optional<std::uint8_t>
decode_zhex_header_type(std::string_view digits) {
  if (digits.size() < zhex_header_digit_count) {
    return std::nullopt;
  }

  const std::optional<std::uint8_t> type = decode_hex_byte(digits, 0);
  if (!type.has_value()) {
    return std::nullopt;
  }

  std::uint8_t header[4]{};
  for (std::size_t index = 0; index < 4; ++index) {
    const std::optional<std::uint8_t> header_byte =
        decode_hex_byte(digits, 2 + index * 2);
    if (!header_byte.has_value()) {
      return std::nullopt;
    }
    header[index] = *header_byte;
  }

  const std::optional<std::uint8_t> high_crc = decode_hex_byte(digits, 10);
  const std::optional<std::uint8_t> low_crc = decode_hex_byte(digits, 12);
  if (!high_crc.has_value() || !low_crc.has_value()) {
    return std::nullopt;
  }

  const std::uint16_t actual_crc =
      static_cast<std::uint16_t>((static_cast<std::uint16_t>(*high_crc) << 8) |
                                 *low_crc);
  if (actual_crc != compute_zmodem_header_crc16(*type, header)) {
    return std::nullopt;
  }
  return type;
}

static std::optional<TerminalTransferDirection>
find_zmodem_auto_start_direction(std::string_view bytes) {
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    if (bytes[index] != zmodem_pad) {
      continue;
    }

    std::size_t marker = index + 1;
    if (marker < bytes.size() && bytes[marker] == zmodem_pad) {
      ++marker;
    }
    if (marker + 2 + zhex_header_digit_count > bytes.size()) {
      break;
    }
    if (bytes[marker] != zmodem_zdle ||
        bytes[marker + 1] != zmodem_zhex) {
      continue;
    }

    const std::string_view digits =
        bytes.substr(marker + 2, zhex_header_digit_count);
    const std::optional<std::uint8_t> type = decode_zhex_header_type(digits);
    if (type == std::optional<std::uint8_t>(zmodem_zrqinit)) {
      return TerminalTransferDirection::receive;
    }
    if (type == std::optional<std::uint8_t>(zmodem_zrinit)) {
      return TerminalTransferDirection::send;
    }
  }
  return std::nullopt;
}

static void trim_detector_tail(TerminalZmodemAutoStartDetectorState *state) {
  if (state->tail.size() <= detector_tail_limit) {
    return;
  }
  state->tail.erase(0, state->tail.size() - detector_tail_limit);
}

std::optional<TerminalTransferDirection>
feed_terminal_zmodem_auto_start_detector(
    TerminalZmodemAutoStartDetectorState *state, std::string_view payload) {
  if (state == nullptr) {
    return std::nullopt;
  }

  state->tail.append(payload.data(), payload.size());
  const std::optional<TerminalTransferDirection> direction =
      find_zmodem_auto_start_direction(state->tail);
  if (direction.has_value()) {
    state->tail.clear();
    return direction;
  }
  trim_detector_tail(state);
  return std::nullopt;
}

} // namespace elder_terms
