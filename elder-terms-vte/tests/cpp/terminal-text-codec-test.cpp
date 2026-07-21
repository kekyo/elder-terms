#include "../../src/terminal-sessions/terminal-text-codec.h"

#include <initializer_list>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace elder_terms {

using TestBytes = std::vector<unsigned char>;

static TerminalTextConversionResult
decode(TerminalTextCodec *codec, std::initializer_list<unsigned char> bytes) {
  const TestBytes input(bytes);
  return codec->decode(
      std::span<const unsigned char>(input.data(), input.size()));
}

static TerminalTextConversionResult
encode(TerminalTextCodec *codec, std::initializer_list<unsigned char> bytes) {
  const TestBytes input(bytes);
  return codec->encode(
      std::span<const unsigned char>(input.data(), input.size()));
}

static TerminalTextConversionResult
encode(TerminalTextEncoder *encoder,
       std::initializer_list<unsigned char> bytes) {
  const TestBytes input(bytes);
  return encoder->encode(
      std::span<const unsigned char>(input.data(), input.size()));
}

static void expect_bytes(const TestBytes &actual,
                         std::initializer_list<unsigned char> expected,
                         const char *message) {
  if (actual != TestBytes(expected)) {
    throw std::runtime_error(message);
  }
}

static void utf8_passes_through_in_both_directions() {
  TerminalTextCodec codec({
      .encoding = "UTF-8",
      .backspace_code = TerminalBackspaceCode::del,
      .cursor_key_mode = TerminalCursorKeyMode::normal,
  });

  const auto received = decode(&codec, {'a', 0xc3, 0xa9});
  const auto sent = encode(&codec, {'a', 0xc3, 0xa9});

  expect_bytes(received.bytes, {'a', 0xc3, 0xa9},
               "UTF-8 backend input should pass through");
  expect_bytes(sent.bytes, {'a', 0xc3, 0xa9},
               "UTF-8 VTE input should pass through");
  if (received.used_replacement || sent.used_replacement) {
    throw std::runtime_error("valid UTF-8 should not use replacements");
  }
}

static void shift_jis_is_converted_across_chunk_boundaries() {
  TerminalTextCodec codec({
      .encoding = "SHIFT-JIS",
      .backspace_code = TerminalBackspaceCode::del,
      .cursor_key_mode = TerminalCursorKeyMode::normal,
  });

  const auto first = decode(&codec, {0x93});
  const auto second = decode(&codec, {0xfa, 0x96, 0x7b, 0x8c, 0xea});
  const auto sent = encode(
      &codec, {0xe6, 0x97, 0xa5, 0xe6, 0x9c, 0xac, 0xe8, 0xaa, 0x9e});

  expect_bytes(first.bytes, {},
               "an incomplete Shift-JIS character should remain pending");
  expect_bytes(second.bytes,
               {0xe6, 0x97, 0xa5, 0xe6, 0x9c, 0xac, 0xe8, 0xaa, 0x9e},
               "Shift-JIS backend input should become UTF-8");
  expect_bytes(sent.bytes, {0x93, 0xfa, 0x96, 0x7b, 0x8c, 0xea},
               "UTF-8 VTE input should become Shift-JIS");
}

static void invalid_or_unrepresentable_text_uses_directional_replacements() {
  TerminalTextCodec codec({
      .encoding = "ASCII",
      .backspace_code = TerminalBackspaceCode::del,
      .cursor_key_mode = TerminalCursorKeyMode::normal,
  });

  const auto received = decode(&codec, {0xff, 'A'});
  const auto sent = encode(&codec, {0xc3, 0xa9, 'A'});

  expect_bytes(received.bytes, {0xef, 0xbf, 0xbd, 'A'},
               "invalid backend bytes should become U+FFFD");
  expect_bytes(sent.bytes, {'?', 'A'},
               "unrepresentable VTE text should become question mark");
  if (!received.used_replacement || !sent.used_replacement) {
    throw std::runtime_error("lossy conversions should report replacement use");
  }
}

static void stateful_encoding_state_is_preserved_between_chunks() {
  TerminalTextCodec encoder({
      .encoding = "ISO-2022-JP",
      .backspace_code = TerminalBackspaceCode::del,
      .cursor_key_mode = TerminalCursorKeyMode::normal,
  });
  TerminalTextCodec decoder({
      .encoding = "ISO-2022-JP",
      .backspace_code = TerminalBackspaceCode::del,
      .cursor_key_mode = TerminalCursorKeyMode::normal,
  });

  const auto encoded_first = encode(&encoder, {0xe6, 0x97, 0xa5});
  const auto encoded_second = encode(&encoder, {0xe6, 0x9c, 0xac});
  TestBytes encoded = encoded_first.bytes;
  encoded.insert(encoded.end(), encoded_second.bytes.begin(),
                 encoded_second.bytes.end());

  const auto decoded_first = decoder.decode(
      std::span<const unsigned char>(encoded.data(), encoded_first.bytes.size()));
  const auto decoded_second = decoder.decode(std::span<const unsigned char>(
      encoded.data() + encoded_first.bytes.size(), encoded_second.bytes.size()));

  expect_bytes(decoded_first.bytes, {0xe6, 0x97, 0xa5},
               "the first stateful chunk should decode");
  expect_bytes(decoded_second.bytes, {0xe6, 0x9c, 0xac},
               "the second stateful chunk should retain iconv state");
}

static void adm3_cursor_keys_use_gtk_oldtype_legacy_codes() {
  TerminalTextCodec codec({
      .encoding = "UTF-8",
      .backspace_code = TerminalBackspaceCode::bs,
      .cursor_key_mode = TerminalCursorKeyMode::adm3,
  });

  const auto prefix = encode(&codec, {0x1b});
  const auto introducer = encode(&codec, {'['});
  const auto up = encode(&codec, {'A'});
  const auto remaining = encode(
      &codec, {0x1b, '[', 'B', 0x1b, 'O', 'C', 0x1b, 'O', 'D'});

  expect_bytes(prefix.bytes, {}, "a split escape prefix should remain pending");
  expect_bytes(introducer.bytes, {},
               "a split cursor introducer should remain pending");
  expect_bytes(up.bytes, {0x1e}, "ADM3 up should use the legacy code");
  expect_bytes(remaining.bytes, {0x1f, 0x1c, 0x1d},
               "ADM3 cursor directions should use legacy codes");
}

static void normal_and_unknown_cursor_sequences_pass_through() {
  TerminalTextCodec normal({
      .encoding = "UTF-8",
      .backspace_code = TerminalBackspaceCode::del,
      .cursor_key_mode = TerminalCursorKeyMode::normal,
  });
  TerminalTextCodec adm3({
      .encoding = "UTF-8",
      .backspace_code = TerminalBackspaceCode::del,
      .cursor_key_mode = TerminalCursorKeyMode::adm3,
  });

  const auto normal_up = encode(&normal, {0x1b, '[', 'A'});
  const auto unknown = encode(&adm3, {0x1b, '[', 'Z'});

  expect_bytes(normal_up.bytes, {0x1b, '[', 'A'},
               "normal cursor mode should preserve VTE input");
  expect_bytes(unknown.bytes, {0x1b, '[', 'Z'},
               "unknown escape sequences should pass through");
}

static void finite_text_encoding_uses_only_the_character_encoding() {
  TerminalTextEncoder encoder({
      .encoding = "SHIFT-JIS",
      .backspace_code = TerminalBackspaceCode::bs,
      .cursor_key_mode = TerminalCursorKeyMode::adm3,
  });

  const auto first = encode(&encoder, {0xe6, 0x97});
  const auto second = encode(
      &encoder, {0xa5, 0x1b, '[', 'A', 0xe6, 0x9c, 0xac});
  const auto finished = encoder.finish();

  expect_bytes(first.bytes, {},
               "an incomplete UTF-8 character should remain pending");
  expect_bytes(second.bytes,
               {0x93, 0xfa, 0x1b, '[', 'A', 0x96, 0x7b},
               "finite text should be encoded without cursor-key mapping");
  expect_bytes(finished.bytes, {},
               "a stateless encoding should have no final shift bytes");
}

static void finite_text_encoding_finishes_pending_input_and_shift_state() {
  TerminalTextEncoder incomplete({
      .encoding = "UTF-8",
      .backspace_code = TerminalBackspaceCode::del,
      .cursor_key_mode = TerminalCursorKeyMode::normal,
  });
  const auto pending = encode(&incomplete, {0xe6, 0x97});
  const auto incomplete_finished = incomplete.finish();

  expect_bytes(pending.bytes, {},
               "incomplete UTF-8 should wait until finite input ends");
  expect_bytes(incomplete_finished.bytes, {'?'},
               "incomplete UTF-8 at EOF should use a question mark");
  if (!incomplete_finished.used_replacement) {
    throw std::runtime_error(
        "incomplete UTF-8 at EOF should report replacement use");
  }

  TerminalTextEncoder stateful({
      .encoding = "ISO-2022-JP",
      .backspace_code = TerminalBackspaceCode::del,
      .cursor_key_mode = TerminalCursorKeyMode::normal,
  });
  const auto encoded = encode(
      &stateful,
      {0xe6, 0x97, 0xa5, 0xe6, 0x9c, 0xac});
  const auto stateful_finished = stateful.finish();

  expect_bytes(encoded.bytes,
               {0x1b, '$', 'B', 0x46, 0x7c, 0x4b, 0x5c},
               "stateful finite text should enter the target shift state");
  expect_bytes(stateful_finished.bytes, {0x1b, '(', 'B'},
               "finite text should flush the final shift state");
}

} // namespace elder_terms

int main() {
  elder_terms::utf8_passes_through_in_both_directions();
  elder_terms::shift_jis_is_converted_across_chunk_boundaries();
  elder_terms::invalid_or_unrepresentable_text_uses_directional_replacements();
  elder_terms::stateful_encoding_state_is_preserved_between_chunks();
  elder_terms::adm3_cursor_keys_use_gtk_oldtype_legacy_codes();
  elder_terms::normal_and_unknown_cursor_sequences_pass_through();
  elder_terms::finite_text_encoding_uses_only_the_character_encoding();
  elder_terms::finite_text_encoding_finishes_pending_input_and_shift_state();
  return 0;
}
