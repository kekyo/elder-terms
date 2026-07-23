#include "terminal-text-send.h"

#include <type_traits>
#include <variant>

namespace elder_terms {

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
