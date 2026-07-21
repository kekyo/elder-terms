#pragma once

#include <functional>
#include <string>

#include <gtk/gtk.h>

#include <elder-terms/export.h>

namespace elder_terms {

/**
 * Opaque state for a reusable validated key-binding input widget.
 */
struct KeyBindingInputWidgetState;

/**
 * Called when key-binding input text or validity changes.
 */
using KeyBindingInputChangedCallback = std::function<void()>;

/**
 * Options used to create a key-binding input widget.
 */
struct KeyBindingInputWidgetOptions {
  /** Initial binding text. */
  std::string text;
  /** Stable accessible identifier assigned to the entry. */
  std::string accessible_id;
  /** Optional callback invoked after input validation. */
  KeyBindingInputChangedCallback changed;
};

/**
 * Creates a validated key-binding input widget.
 *
 * @param options Initial text, accessible identifier, and callback.
 * @returns New widget state owned by the caller.
 */
ELDER_TERMS_API KeyBindingInputWidgetState *
create_key_binding_input_widget(KeyBindingInputWidgetOptions options);

/**
 * Returns the GtkEntry used by a key-binding input widget.
 *
 * @param state Widget state.
 * @returns Root GtkEntry, or nullptr when state is null.
 */
ELDER_TERMS_API GtkWidget *
key_binding_input_widget_root(KeyBindingInputWidgetState *state);

/**
 * Replaces the current key-binding text and validates it.
 *
 * @param state Widget state.
 * @param text New binding text.
 */
ELDER_TERMS_API void
set_key_binding_input_widget_text(KeyBindingInputWidgetState *state,
                                  const std::string &text);

/**
 * Returns the current key-binding text.
 *
 * @param state Widget state.
 * @returns Current entry text, or an empty string when state is null.
 */
ELDER_TERMS_API std::string key_binding_input_widget_text(
    const KeyBindingInputWidgetState *state);

/**
 * Checks whether the current text has valid key-binding syntax.
 *
 * @param state Widget state.
 * @returns True when the text is valid, including an empty disabled binding.
 */
ELDER_TERMS_API bool key_binding_input_widget_is_valid(
    const KeyBindingInputWidgetState *state);

/**
 * Sets an additional validation error such as a cross-field conflict.
 *
 * @param state Widget state.
 * @param error Error text, or an empty string to clear the external error.
 */
ELDER_TERMS_API void set_key_binding_input_widget_external_error(
    KeyBindingInputWidgetState *state, const std::string &error);

/**
 * Releases key-binding input widget state.
 *
 * @param state Widget state to destroy. The containing GTK hierarchy retains
 * ownership of the root entry.
 */
ELDER_TERMS_API void
destroy_key_binding_input_widget(KeyBindingInputWidgetState *state);

} // namespace elder_terms
