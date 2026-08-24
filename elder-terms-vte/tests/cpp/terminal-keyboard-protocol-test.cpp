#include "../../src/terminal-sessions/terminal-keyboard-protocol.h"

#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace elder_terms {

static void expect_true(bool value, std::string_view message) {
  if (!value) {
    throw std::runtime_error(std::string(message));
  }
}

static void expect_false(bool value, std::string_view message) {
  expect_true(!value, message);
}

static void expect_sequence(const std::optional<std::string> &actual,
                            std::string_view expected,
                            std::string_view message) {
  if (!actual.has_value() || *actual != expected) {
    throw std::runtime_error(std::string(message));
  }
}

static void feed(TerminalKeyboardProtocolState *state,
                 std::string_view bytes) {
  const auto *data =
      reinterpret_cast<const unsigned char *>(bytes.data());
  state->observe(std::span<const unsigned char>(data, bytes.size()));
}

static void tracks_fragmented_push_and_pop() {
  constexpr std::string_view push = "\x1b[>7u";
  for (std::size_t split = 0; split <= push.size(); ++split) {
    TerminalKeyboardProtocolState state;
    feed(&state, push.substr(0, split));
    expect_true(state.modified_special_keys_enabled() ==
                    (split == push.size()),
                "keyboard mode should activate only after complete push");
    feed(&state, push.substr(split));
    expect_true(state.modified_special_keys_enabled(),
                "fragmented keyboard push should activate mode");
  }

  TerminalKeyboardProtocolState state;
  feed(&state, push);
  feed(&state, "\x1b[<");
  expect_true(state.modified_special_keys_enabled(),
              "partial keyboard pop should preserve mode");
  feed(&state, "u");
  expect_false(state.modified_special_keys_enabled(),
               "complete keyboard pop should disable mode");
}

static void restores_nested_modes() {
  TerminalKeyboardProtocolState state;
  feed(&state, "\x1b[>7u");
  expect_true(state.modified_special_keys_enabled(),
              "flags 7 should enable modified special keys");

  feed(&state, "\x1b[>0u");
  expect_false(state.modified_special_keys_enabled(),
               "nested flags 0 should temporarily disable conversion");
  feed(&state, "\x1b[<u");
  expect_true(state.modified_special_keys_enabled(),
              "pop should restore the previous flags");

  feed(&state, "\x1b[>2u\x1b[>4u\x1b[<2u");
  expect_true(state.modified_special_keys_enabled(),
              "multi-pop should restore the earlier disambiguation mode");
  feed(&state, "\x1b[<9u");
  expect_false(state.modified_special_keys_enabled(),
               "over-pop should empty the keyboard mode stack");
}

static void activates_only_disambiguating_flags() {
  TerminalKeyboardProtocolState state;
  feed(&state, "\x1b[>2u");
  expect_false(state.modified_special_keys_enabled(),
               "event reporting alone should not enable conversion");
  feed(&state, "\x1b[<u\x1b[>4u");
  expect_false(state.modified_special_keys_enabled(),
               "alternate keys alone should not enable conversion");
  feed(&state, "\x1b[<u\x1b[>8u");
  expect_true(state.modified_special_keys_enabled(),
              "report-all should imply disambiguation");
  feed(&state, "\x1b[<u\x1b[>u");
  expect_false(state.modified_special_keys_enabled(),
               "omitted push flags should default to zero");
}

static void ignores_embedded_and_malformed_sequences() {
  TerminalKeyboardProtocolState state;
  feed(&state, "\x1b]0;ignored \x1b[>7u\x07");
  expect_false(state.modified_special_keys_enabled(),
               "OSC payload should not change keyboard mode");
  feed(&state, "\x1bPignored \x1b[>7u\x1b\\");
  expect_false(state.modified_special_keys_enabled(),
               "DCS payload should not change keyboard mode");
  feed(&state, "\x1b[>7m\x1b[>7;1u\x1b[>999999999999999999999u");
  expect_false(state.modified_special_keys_enabled(),
               "malformed keyboard pushes should be ignored");
  feed(&state, "plain text \x1b[>7u");
  expect_true(state.modified_special_keys_enabled(),
              "valid push after ignored data should still activate mode");
  feed(&state, "\x1b" "c");
  expect_false(state.modified_special_keys_enabled(),
               "full terminal reset should clear keyboard modes");
}

static void keeps_screen_mode_stacks_independent() {
  TerminalKeyboardProtocolState state;
  feed(&state, "\x1b[>7u");
  feed(&state, "\x1b[?1049h");
  expect_false(state.modified_special_keys_enabled(),
               "alternate screen should start with its own mode stack");
  feed(&state, "\x1b[>1u");
  expect_true(state.modified_special_keys_enabled(),
              "alternate screen should accept an independent push");
  feed(&state, "\x1b[?1049l");
  expect_true(state.modified_special_keys_enabled(),
              "returning to main screen should restore its mode");
  feed(&state, "\x1b[<u\x1b[?1049h");
  expect_true(state.modified_special_keys_enabled(),
              "main-screen pop should not alter alternate-screen mode");
  feed(&state, "\x1b[<u");
  expect_false(state.modified_special_keys_enabled(),
               "alternate-screen pop should disable only that screen");
}

static void encodes_modified_special_keys() {
  TerminalKeyModifiers control;
  control.control = true;
  expect_sequence(encode_modified_special_key(TerminalSpecialKey::enter,
                                               control),
                  "\x1b[13;5u", "Ctrl+Enter should use CSI 13;5u");
  expect_sequence(encode_modified_special_key(
                      TerminalSpecialKey::keypad_enter, control),
                  "\x1b[57414;5u",
                  "Ctrl+keypad Enter should preserve keypad identity");

  TerminalKeyModifiers shift_control;
  shift_control.shift = true;
  shift_control.control = true;
  expect_sequence(encode_modified_special_key(TerminalSpecialKey::enter,
                                               shift_control),
                  "\x1b[13;6u",
                  "Ctrl+Shift+Enter should combine modifier bits");

  TerminalKeyModifiers alt;
  alt.alt = true;
  expect_sequence(encode_modified_special_key(TerminalSpecialKey::tab, alt),
                  "\x1b[9;3u", "Alt+Tab should use the Tab key code");

  TerminalKeyModifiers extended;
  extended.super = true;
  extended.hyper = true;
  extended.meta = true;
  expect_sequence(encode_modified_special_key(TerminalSpecialKey::space,
                                               extended),
                  "\x1b[32;57u",
                  "extended modifiers should use protocol modifier bits");

  TerminalKeyModifiers none;
  expect_false(encode_modified_special_key(TerminalSpecialKey::escape, none)
                   .has_value(),
               "unmodified special keys should remain under VTE control");
}

static void recognizes_only_matching_legacy_commits() {
  expect_true(is_legacy_modified_special_key_commit(
                  TerminalSpecialKey::enter, "\r"),
              "Enter CR should be replaceable");
  expect_true(is_legacy_modified_special_key_commit(
                  TerminalSpecialKey::enter, "\x1b\r"),
              "Alt+Enter legacy commit should be replaceable");
  expect_true(is_legacy_modified_special_key_commit(
                  TerminalSpecialKey::tab, "\x1b[Z"),
              "back-tab legacy commit should be replaceable");
  expect_true(is_legacy_modified_special_key_commit(
                  TerminalSpecialKey::backspace, "\x7f"),
              "DEL Backspace commit should be replaceable");
  expect_true(is_legacy_modified_special_key_commit(
                  TerminalSpecialKey::space, std::string_view("\0", 1)),
              "Ctrl+Space NUL commit should be replaceable");
  expect_false(is_legacy_modified_special_key_commit(
                   TerminalSpecialKey::enter, "日本語"),
               "IME text should never be replaced as Enter");
}

} // namespace elder_terms

int main() {
  elder_terms::tracks_fragmented_push_and_pop();
  elder_terms::restores_nested_modes();
  elder_terms::activates_only_disambiguating_flags();
  elder_terms::ignores_embedded_and_malformed_sequences();
  elder_terms::keeps_screen_mode_stacks_independent();
  elder_terms::encodes_modified_special_keys();
  elder_terms::recognizes_only_matching_legacy_commits();
  return 0;
}
