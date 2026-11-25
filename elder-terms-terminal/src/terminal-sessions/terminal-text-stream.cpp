// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "terminal-text-stream.h"

#include <iconv.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace elder_terms {

enum class TerminalTextConversionDirection {
  backend_to_utf8,
  utf8_to_backend,
};

struct TerminalTextStream {
private:
  iconv_t converter = reinterpret_cast<iconv_t>(-1);
  std::string pending;
  TerminalTextConversionDirection direction;
  bool finished = false;

  static std::size_t utf8_character_size(const std::string &input) {
    if (input.empty()) {
      return 0;
    }

    const auto lead = static_cast<unsigned char>(input.front());
    std::size_t expected = 1;
    if (lead >= 0xc2 && lead <= 0xdf) {
      expected = 2;
    } else if (lead >= 0xe0 && lead <= 0xef) {
      expected = 3;
    } else if (lead >= 0xf0 && lead <= 0xf4) {
      expected = 4;
    }
    if (expected > input.size()) {
      return 1;
    }
    for (std::size_t index = 1; index < expected; ++index) {
      const auto continuation = static_cast<unsigned char>(input[index]);
      if (continuation < 0x80 || continuation > 0xbf) {
        return 1;
      }
    }
    return expected;
  }

  static void append_output(std::vector<unsigned char> *output,
                            const char *data, std::size_t size) {
    const auto *bytes = reinterpret_cast<const unsigned char *>(data);
    output->insert(output->end(), bytes, bytes + size);
  }

  void reset_to_initial_state() {
    if (iconv(converter, nullptr, nullptr, nullptr, nullptr) ==
        static_cast<std::size_t>(-1)) {
      throw std::system_error(errno, std::generic_category(),
                              "failed to reset terminal iconv state");
    }
  }

  void append_encoded_question_mark(std::vector<unsigned char> *output) {
    char question_mark = '?';
    char *input = &question_mark;
    std::size_t input_size = 1;
    while (input_size != 0) {
      std::array<char, 32> buffer{};
      char *converted = buffer.data();
      std::size_t converted_size = buffer.size();
      errno = 0;
      const std::size_t result =
          iconv(converter, &input, &input_size, &converted, &converted_size);
      append_output(output, buffer.data(), buffer.size() - converted_size);
      if (result != static_cast<std::size_t>(-1)) {
        continue;
      }
      if (errno == E2BIG) {
        continue;
      }
      throw std::system_error(errno, std::generic_category(),
                              "failed to encode terminal replacement");
    }
  }

public:
  bool has_pending() const { return !pending.empty(); }

  TerminalTextStream(const std::string &from, const std::string &to,
                          TerminalTextConversionDirection direction)
      : converter(iconv_open(to.c_str(), from.c_str())),
        direction(direction) {
    if (converter == reinterpret_cast<iconv_t>(-1)) {
      throw std::system_error(errno, std::generic_category(),
                              "failed to initialize terminal iconv conversion");
    }
  }

  ~TerminalTextStream() {
    if (converter != reinterpret_cast<iconv_t>(-1)) {
      (void)iconv_close(converter);
    }
  }

  TerminalTextStream(const TerminalTextStream &) = delete;
  TerminalTextStream &operator=(const TerminalTextStream &) = delete;

  TerminalTextConversionResult
  convert(std::span<const unsigned char> bytes) {
    if (finished) {
      throw std::logic_error("terminal text conversion is already finished");
    }
    if (!bytes.empty()) {
      pending.append(reinterpret_cast<const char *>(bytes.data()),
                     bytes.size());
    }

    TerminalTextConversionResult conversion;
    while (!pending.empty()) {
      std::array<char, 4096> buffer{};
      char *input = pending.data();
      std::size_t input_size = pending.size();
      char *output = buffer.data();
      std::size_t output_size = buffer.size();
      errno = 0;
      const std::size_t result =
          iconv(converter, &input, &input_size, &output, &output_size);
      const std::size_t consumed = pending.size() - input_size;
      pending.erase(0, consumed);
      append_output(&conversion.bytes, buffer.data(),
                    buffer.size() - output_size);

      if (result != static_cast<std::size_t>(-1)) {
        if (consumed == 0 && !pending.empty()) {
          throw std::system_error(EIO, std::generic_category(),
                                  "terminal iconv conversion made no progress");
        }
        continue;
      }
      if (errno == E2BIG) {
        if (consumed == 0 && output_size == buffer.size()) {
          throw std::system_error(E2BIG, std::generic_category(),
                                  "terminal iconv output buffer is too small");
        }
        continue;
      }
      if (errno == EINVAL) {
        break;
      }
      if (errno == EILSEQ) {
        conversion.used_replacement = true;
        if (direction == TerminalTextConversionDirection::backend_to_utf8) {
          pending.erase(0, std::min<std::size_t>(1, pending.size()));
          conversion.bytes.insert(conversion.bytes.end(),
                                  {0xef, 0xbf, 0xbd});
          reset_to_initial_state();
        } else {
          pending.erase(0, utf8_character_size(pending));
          append_encoded_question_mark(&conversion.bytes);
        }
        continue;
      }
      throw std::system_error(errno, std::generic_category(),
                              "terminal iconv conversion failed");
    }
    return conversion;
  }

  TerminalTextConversionResult finish() {
    TerminalTextConversionResult conversion = convert({});
    finished = true;

    if (!pending.empty()) {
      conversion.used_replacement = true;
      pending.clear();
      if (direction == TerminalTextConversionDirection::backend_to_utf8) {
        conversion.bytes.insert(conversion.bytes.end(), {0xef, 0xbf, 0xbd});
        reset_to_initial_state();
      } else {
        append_encoded_question_mark(&conversion.bytes);
      }
    }

    while (true) {
      std::array<char, 32> buffer{};
      char *output = buffer.data();
      std::size_t output_size = buffer.size();
      errno = 0;
      const std::size_t result =
          iconv(converter, nullptr, nullptr, &output, &output_size);
      append_output(&conversion.bytes, buffer.data(),
                    buffer.size() - output_size);
      if (result != static_cast<std::size_t>(-1)) {
        break;
      }
      if (errno == E2BIG) {
        continue;
      }
      throw std::system_error(errno, std::generic_category(),
                              "failed to finish terminal iconv conversion");
    }
    return conversion;
  }
};

TerminalTextStreamPtr create_terminal_text_decoder(const std::string &encoding) {
  return {new TerminalTextStream(encoding, "UTF-8", TerminalTextConversionDirection::backend_to_utf8), destroy_terminal_text_stream};
}

TerminalTextStreamPtr create_terminal_text_encoder(const std::string &encoding) {
  return {new TerminalTextStream("UTF-8", encoding, TerminalTextConversionDirection::utf8_to_backend), destroy_terminal_text_stream};
}

void destroy_terminal_text_stream(TerminalTextStream *stream) { delete stream; }

TerminalTextConversionResult convert_terminal_text(TerminalTextStream *stream, std::span<const unsigned char> bytes) {
  return stream->convert(bytes);
}

TerminalTextConversionResult finish_terminal_text(TerminalTextStream *stream) { return stream->finish(); }

bool terminal_text_stream_has_pending(const TerminalTextStream *stream) { return stream->has_pending(); }

} // namespace elder_terms
