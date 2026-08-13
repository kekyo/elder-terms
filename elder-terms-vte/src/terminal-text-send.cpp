#include "terminal-text-send.h"

#include <type_traits>
#include <variant>
#include <vector>

namespace elder_terms {

TerminalTextLineEndingNormalizer::TerminalTextLineEndingNormalizer(
    bool follow_return_code, TerminalReturnCode return_code)
    : follow_return_code(follow_return_code), return_code(return_code) {}

void TerminalTextLineEndingNormalizer::append_newline(
    std::vector<unsigned char> *output) const {
  if (return_code == TerminalReturnCode::lf) {
    output->push_back('\n');
    return;
  }
  output->push_back('\r');
  if (return_code == TerminalReturnCode::crlf) {
    output->push_back('\n');
  }
}

std::vector<unsigned char> TerminalTextLineEndingNormalizer::normalize(
    std::span<const unsigned char> input) {
  if (!follow_return_code) {
    return std::vector<unsigned char>(input.begin(), input.end());
  }

  std::vector<unsigned char> output;
  output.reserve(input.size());
  for (unsigned char byte : input) {
    if (pending_carriage_return) {
      append_newline(&output);
      pending_carriage_return = false;
      if (byte == '\n') {
        continue;
      }
    }
    if (byte == '\r') {
      pending_carriage_return = true;
    } else if (byte == '\n') {
      append_newline(&output);
    } else {
      output.push_back(byte);
    }
  }
  return output;
}

std::vector<unsigned char> TerminalTextLineEndingNormalizer::finish() {
  std::vector<unsigned char> output;
  if (follow_return_code && pending_carriage_return) {
    append_newline(&output);
    pending_carriage_return = false;
  }
  return output;
}

std::string normalize_terminal_text_line_endings(
    const std::string &utf8_text, bool follow_return_code,
    TerminalReturnCode return_code) {
  TerminalTextLineEndingNormalizer normalizer(follow_return_code, return_code);
  const auto *input =
      reinterpret_cast<const unsigned char *>(utf8_text.data());
  std::vector<unsigned char> normalized = normalizer.normalize(
      std::span<const unsigned char>(input, utf8_text.size()));
  std::vector<unsigned char> tail = normalizer.finish();
  normalized.insert(normalized.end(), tail.begin(), tail.end());
  if (normalized.empty()) {
    return {};
  }
  return std::string(reinterpret_cast<const char *>(normalized.data()),
                     normalized.size());
}

bool terminal_text_send_source_is_valid(const TerminalTextSendSource &source) {
  return std::visit(
      [](const auto &value) {
        using Source = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Source, TerminalTextSendFileSource>) {
          return !value.uri.empty();
        } else {
          return !value.utf8_text.empty();
        }
      },
      source);
}

} // namespace elder_terms
