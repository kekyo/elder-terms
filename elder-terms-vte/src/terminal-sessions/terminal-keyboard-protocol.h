#pragma once

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace elder_terms {

/**
 * Special key whose legacy VTE encoding can lose modifier information.
 */
enum class TerminalSpecialKey {
  /** Main keyboard Enter key. */
  enter,
  /** Numeric keypad Enter key. */
  keypad_enter,
  /** Tab key, including ISO Left Tab after normalization. */
  tab,
  /** Backspace key. */
  backspace,
  /** Escape key. */
  escape,
  /** Space key. */
  space,
};

/**
 * Modifier state used by the enhanced terminal keyboard protocol.
 */
struct TerminalKeyModifiers {
  /** Shift modifier. */
  bool shift = false;
  /** Alt modifier. */
  bool alt = false;
  /** Control modifier. */
  bool control = false;
  /** Super modifier. */
  bool super = false;
  /** Hyper modifier. */
  bool hyper = false;
  /** Meta modifier. */
  bool meta = false;
};

/**
 * Incrementally observes terminal output keyboard enhancement modes.
 *
 * @remarks The observer never consumes or modifies output bytes. It tracks the
 * bounded push/pop mode stacks independently for the main and alternate
 * terminal screens.
 */
class TerminalKeyboardProtocolState {
private:
  class Impl;
  std::unique_ptr<Impl> impl;

public:
  /** Creates an observer with both keyboard mode stacks disabled. */
  TerminalKeyboardProtocolState();

  /** Releases the incremental parser state. */
  ~TerminalKeyboardProtocolState();

  TerminalKeyboardProtocolState(const TerminalKeyboardProtocolState &) =
      delete;
  TerminalKeyboardProtocolState &
  operator=(const TerminalKeyboardProtocolState &) = delete;

  /**
   * Observes one cooked terminal output chunk.
   *
   * @param bytes Bytes that will also be fed to VTE.
   */
  void observe(std::span<const unsigned char> bytes);

  /**
   * Returns whether modified special keys need unambiguous encoding.
   *
   * @returns True when the active screen's current mode disambiguates keys.
   */
  bool modified_special_keys_enabled() const;
};

/**
 * Encodes a modified special-key press using the CSI-u keyboard protocol.
 *
 * @param key Normalized special key.
 * @param modifiers Active keyboard modifiers.
 * @returns Encoded sequence, or no value when the key has no modifier.
 */
std::optional<std::string>
encode_modified_special_key(TerminalSpecialKey key,
                            const TerminalKeyModifiers &modifiers);

/**
 * Tests whether VTE commit data is a legacy encoding of one special key.
 *
 * @param key Normalized special key.
 * @param commit Complete VTE commit bytes.
 * @returns True when the commit can safely be replaced for the pending key.
 */
bool is_legacy_modified_special_key_commit(TerminalSpecialKey key,
                                           std::string_view commit);

} // namespace elder_terms
