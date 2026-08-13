#pragma once

#include <optional>
#include <string>

#include <gdk/gdk.h>

#include <elder-terms/export.h>

namespace elder_terms {

/**
 * One parsed keyboard binding.
 */
struct KeyBinding {
  /** GDK key value for the non-modifier key. */
  guint keyval = GDK_KEY_VoidSymbol;
  /** Exact set of supported modifiers required by the binding. */
  GdkModifierType modifiers = static_cast<GdkModifierType>(0);
};

/**
 * Result of parsing one keyboard binding string.
 */
struct KeyBindingParseResult {
  /** Parsed binding, or no value when the empty string disables the binding. */
  std::optional<KeyBinding> binding;
  /** Validation error, empty when parsing succeeded. */
  std::string error;
};

/**
 * Parses a keyboard binding written as modifier and key tokens.
 *
 * @param text Binding text using `+` or `-` separators.
 * @returns Parsed binding or a validation error. Empty text is a valid disabled
 * binding.
 *
 * @remarks Supported modifiers are Ctrl, Shift, Alt, and Super. Tokens are
 * matched case-insensitively and may be separated with mixed separators.
 */
ELDER_TERMS_API KeyBindingParseResult
parse_key_binding(const std::string &text);

/**
 * Validates a key binding suitable for desktop-wide registration.
 *
 * @param text Candidate binding text. An empty value disables the hotkey.
 * @param reason Receives a human-readable reason when validation fails.
 * @returns True for an empty value or a key binding with at least one
 * supported modifier.
 */
ELDER_TERMS_API bool
global_hotkey_text_is_valid(const std::string &text,
                            std::string *reason);

/**
 * Checks whether two parsed keyboard bindings are identical.
 *
 * @param left First binding.
 * @param right Second binding.
 * @returns True when key and exact modifier set are equal.
 */
ELDER_TERMS_API bool key_bindings_equal(const KeyBinding &left,
                                        const KeyBinding &right);

/**
 * Checks a key event against one parsed keyboard binding.
 *
 * @param binding Expected key and modifiers.
 * @param keyval GDK key value from the event.
 * @param modifiers Raw modifier state from the event.
 * @returns True only when the case-normalized key and supported modifier set
 * match exactly.
 *
 * @remarks Lock and pointer-button state is ignored. Keyboard-layout-consumed
 * modifiers remain part of the exact comparison. Alphabetic key values are
 * compared case-insensitively so Shift and Caps Lock do not prevent the
 * corresponding lowercase binding from matching.
 */
ELDER_TERMS_API bool key_binding_matches(const KeyBinding &binding,
                                         guint keyval,
                                         GdkModifierType modifiers);

} // namespace elder_terms
