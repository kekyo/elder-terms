#include "terminal-text-codec.h"

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

class TerminalStreamConverter {
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
  TerminalStreamConverter(const std::string &from, const std::string &to,
                          TerminalTextConversionDirection direction)
      : converter(iconv_open(to.c_str(), from.c_str())),
        direction(direction) {
    if (converter == reinterpret_cast<iconv_t>(-1)) {
      throw std::system_error(errno, std::generic_category(),
                              "failed to initialize terminal iconv conversion");
    }
  }

  ~TerminalStreamConverter() {
    if (converter != reinterpret_cast<iconv_t>(-1)) {
      (void)iconv_close(converter);
    }
  }

  TerminalStreamConverter(const TerminalStreamConverter &) = delete;
  TerminalStreamConverter &operator=(const TerminalStreamConverter &) = delete;

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

class TerminalCursorKeyMapper {
private:
  TerminalCursorKeyMode mode;
  std::string pending;

  static unsigned char map_cursor_sequence(unsigned char introducer,
                                           unsigned char final_byte) {
    if (introducer != '[' && introducer != 'O') {
      return 0;
    }
    switch (final_byte) {
    case 'A':
      return 0x1e;
    case 'B':
      return 0x1f;
    case 'C':
      return 0x1c;
    case 'D':
      return 0x1d;
    default:
      return 0;
    }
  }

public:
  explicit TerminalCursorKeyMapper(TerminalCursorKeyMode mode)
      : mode(mode) {
  }

  std::vector<unsigned char>
  transform(std::span<const unsigned char> bytes) {
    if (mode == TerminalCursorKeyMode::normal) {
      return {bytes.begin(), bytes.end()};
    }

    if (!bytes.empty()) {
      pending.append(reinterpret_cast<const char *>(bytes.data()),
                     bytes.size());
    }
    std::vector<unsigned char> output;
    std::size_t index = 0;
    while (index < pending.size()) {
      const auto current = static_cast<unsigned char>(pending[index]);
      if (current != 0x1b) {
        output.push_back(current);
        ++index;
        continue;
      }
      if (index + 1 >= pending.size()) {
        break;
      }

      const auto introducer =
          static_cast<unsigned char>(pending[index + 1]);
      if (introducer != '[' && introducer != 'O') {
        output.push_back(current);
        ++index;
        continue;
      }
      if (index + 2 >= pending.size()) {
        break;
      }

      const auto final_byte =
          static_cast<unsigned char>(pending[index + 2]);
      const unsigned char mapped =
          map_cursor_sequence(introducer, final_byte);
      if (mapped != 0) {
        output.push_back(mapped);
        index += 3;
        continue;
      }

      output.push_back(current);
      ++index;
    }
    pending.erase(0, index);
    return output;
  }
};

class TerminalTextCodec::Impl {
public:
  TerminalStreamConverter decoder;
  TerminalTextEncoder encoder;
  TerminalCursorKeyMapper cursor_key_mapper;

  explicit Impl(const TerminalTextSettings &settings)
      : decoder(settings.encoding, "UTF-8",
                TerminalTextConversionDirection::backend_to_utf8),
        encoder(settings),
        cursor_key_mapper(settings.cursor_key_mode) {
  }
};

class TerminalTextEncoder::Impl {
public:
  TerminalStreamConverter encoder;

  explicit Impl(const TerminalTextSettings &settings)
      : encoder("UTF-8", settings.encoding,
                TerminalTextConversionDirection::utf8_to_backend) {
  }
};

TerminalTextEncoder::TerminalTextEncoder(const TerminalTextSettings &settings)
    : impl(std::make_unique<Impl>(settings)) {
}

TerminalTextEncoder::~TerminalTextEncoder() = default;

TerminalTextConversionResult
TerminalTextEncoder::encode(std::span<const unsigned char> bytes) {
  return impl->encoder.convert(bytes);
}

TerminalTextConversionResult TerminalTextEncoder::finish() {
  return impl->encoder.finish();
}

TerminalTextCodec::TerminalTextCodec(const TerminalTextSettings &settings)
    : impl(std::make_unique<Impl>(settings)) {
}

TerminalTextCodec::~TerminalTextCodec() = default;

TerminalTextConversionResult
TerminalTextCodec::decode(std::span<const unsigned char> bytes) {
  return impl->decoder.convert(bytes);
}

TerminalTextConversionResult
TerminalTextCodec::encode(std::span<const unsigned char> bytes) {
  const std::vector<unsigned char> mapped =
      impl->cursor_key_mapper.transform(bytes);
  return impl->encoder.encode(
      std::span<const unsigned char>(mapped.data(), mapped.size()));
}

} // namespace elder_terms
