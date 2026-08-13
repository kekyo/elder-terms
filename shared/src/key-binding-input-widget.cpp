#include <elder-terms/key-binding-input-widget.h>

#include <optional>
#include <set>
#include <string>
#include <utility>

#include <atk/atk.h>

#include <elder-terms/key-binding.h>

#include "settings-widget/settings-presentation.h"

namespace elder_terms {

enum class KeyBindingInputDisplayMode {
  confirmed,
  waiting,
  modifiers,
};

struct KeyBindingInputWidgetState {
  GtkWidget *entry = nullptr;
  KeyBindingInputChangedCallback changed;
  std::string confirmed_text;
  std::string syntax_error;
  std::string external_error;
  std::set<guint> pressed_modifier_keys;
  KeyBindingInputDisplayMode display_mode =
      KeyBindingInputDisplayMode::confirmed;
  bool empty_clear_enabled = false;
  bool focused = false;
};

static constexpr auto supported_modifier_mask =
    static_cast<GdkModifierType>(GDK_CONTROL_MASK | GDK_SHIFT_MASK |
                                 GDK_MOD1_MASK | GDK_SUPER_MASK);

static void assign_accessible_id(GtkWidget *widget, const std::string &id) {
  if (widget == nullptr || id.empty()) {
    return;
  }

  gtk_widget_set_name(widget, id.c_str());
  if (g_object_class_find_property(G_OBJECT_GET_CLASS(widget),
                                   "accessible-id") != nullptr) {
    g_object_set(widget, "accessible-id", id.c_str(), nullptr);
  }
  AtkObject *accessible = gtk_widget_get_accessible(widget);
  if (accessible != nullptr) {
    atk_object_set_accessible_id(accessible, id.c_str());
  }
}

static std::string validation_error(KeyBindingInputWidgetState *state) {
  return state->syntax_error.empty() ? state->external_error
                                     : state->syntax_error;
}

static void update_validation_presentation(
    KeyBindingInputWidgetState *state) {
  const std::string error = validation_error(state);
  const std::string message = settings_validation_message(error);
  GtkStyleContext *context = gtk_widget_get_style_context(state->entry);
  if (!error.empty()) {
    gtk_style_context_add_class(context, GTK_STYLE_CLASS_ERROR);
    gtk_entry_set_icon_from_icon_name(GTK_ENTRY(state->entry),
                                      GTK_ENTRY_ICON_SECONDARY,
                                      "dialog-error-symbolic");
    gtk_entry_set_icon_activatable(GTK_ENTRY(state->entry),
                                   GTK_ENTRY_ICON_SECONDARY, FALSE);
    gtk_entry_set_icon_tooltip_text(GTK_ENTRY(state->entry),
                                    GTK_ENTRY_ICON_SECONDARY,
                                    message.c_str());
    return;
  }

  gtk_style_context_remove_class(context, GTK_STYLE_CLASS_ERROR);
  const bool show_clear =
      state->focused &&
      (!state->confirmed_text.empty() || state->empty_clear_enabled);
  gtk_entry_set_icon_from_icon_name(
      GTK_ENTRY(state->entry), GTK_ENTRY_ICON_SECONDARY,
      show_clear ? "edit-clear-symbolic" : nullptr);
  gtk_entry_set_icon_activatable(GTK_ENTRY(state->entry),
                                 GTK_ENTRY_ICON_SECONDARY,
                                 show_clear ? TRUE : FALSE);
  gtk_entry_set_icon_tooltip_text(
      GTK_ENTRY(state->entry), GTK_ENTRY_ICON_SECONDARY,
      show_clear ? settings_ui_text(SettingsUiText::clear_key_binding)
                 : nullptr);
}

static void set_entry_presentation(KeyBindingInputWidgetState *state,
                                   const std::string &text) {
  const char *current = gtk_entry_get_text(GTK_ENTRY(state->entry));
  if (current != nullptr && text == current) {
    return;
  }
  gtk_entry_set_text(GTK_ENTRY(state->entry), text.c_str());
}

static void validate_confirmed_text(KeyBindingInputWidgetState *state) {
  const KeyBindingParseResult parsed =
      parse_key_binding(state->confirmed_text);
  state->syntax_error = parsed.error;
  update_validation_presentation(state);
}

static void append_token(std::string *text, const char *token) {
  if (!text->empty()) {
    text->append("+");
  }
  text->append(token);
}

static std::string modifier_text(GdkModifierType modifiers) {
  std::string text;
  if ((modifiers & GDK_CONTROL_MASK) != 0) {
    append_token(&text, "ctrl");
  }
  if ((modifiers & GDK_SHIFT_MASK) != 0) {
    append_token(&text, "shift");
  }
  if ((modifiers & GDK_MOD1_MASK) != 0) {
    append_token(&text, "alt");
  }
  if ((modifiers & GDK_SUPER_MASK) != 0) {
    append_token(&text, "super");
  }
  return text;
}

static std::optional<GdkModifierType> modifier_for_keyval(guint keyval) {
  switch (keyval) {
  case GDK_KEY_Control_L:
  case GDK_KEY_Control_R:
    return GDK_CONTROL_MASK;
  case GDK_KEY_Shift_L:
  case GDK_KEY_Shift_R:
    return GDK_SHIFT_MASK;
  case GDK_KEY_Alt_L:
  case GDK_KEY_Alt_R:
    return GDK_MOD1_MASK;
  case GDK_KEY_Super_L:
  case GDK_KEY_Super_R:
    return GDK_SUPER_MASK;
  default:
    return std::nullopt;
  }
}

static GdkModifierType pressed_modifier_mask(
    const KeyBindingInputWidgetState *state) {
  auto modifiers = static_cast<GdkModifierType>(0);
  for (const guint keyval : state->pressed_modifier_keys) {
    const auto modifier = modifier_for_keyval(keyval);
    if (modifier.has_value()) {
      modifiers = static_cast<GdkModifierType>(modifiers | *modifier);
    }
  }
  return modifiers;
}

static std::optional<std::string>
format_binding(guint keyval, GdkModifierType modifiers) {
  const guint normalized_keyval = gdk_keyval_to_lower(keyval);
  if (normalized_keyval == GDK_KEY_VoidSymbol) {
    return std::nullopt;
  }
  const char *key_name = gdk_keyval_name(normalized_keyval);
  if (key_name == nullptr) {
    return std::nullopt;
  }

  std::string text = modifier_text(static_cast<GdkModifierType>(
      modifiers & supported_modifier_mask));
  append_token(&text, key_name);
  return text;
}

static void show_modifier_presentation(KeyBindingInputWidgetState *state) {
  const GdkModifierType modifiers = pressed_modifier_mask(state);
  if (modifiers == 0) {
    state->display_mode = KeyBindingInputDisplayMode::waiting;
    set_entry_presentation(state, {});
    return;
  }

  state->display_mode = KeyBindingInputDisplayMode::modifiers;
  set_entry_presentation(state, modifier_text(modifiers));
}

static void commit_confirmed_text(KeyBindingInputWidgetState *state,
                                  std::string text) {
  const bool changed = state->confirmed_text != text;
  if (changed) {
    state->external_error.clear();
  }
  state->confirmed_text = std::move(text);
  state->display_mode = KeyBindingInputDisplayMode::confirmed;
  set_entry_presentation(state, state->confirmed_text);
  validate_confirmed_text(state);
  if (changed && state->changed) {
    state->changed();
  }
}

static gboolean on_entry_focus_in(GtkWidget *, GdkEventFocus *,
                                  gpointer data) {
  auto *state = static_cast<KeyBindingInputWidgetState *>(data);
  state->focused = true;
  state->pressed_modifier_keys.clear();
  state->display_mode = KeyBindingInputDisplayMode::waiting;
  set_entry_presentation(state, {});
  update_validation_presentation(state);
  return GDK_EVENT_PROPAGATE;
}

static gboolean on_entry_focus_out(GtkWidget *, GdkEventFocus *,
                                   gpointer data) {
  auto *state = static_cast<KeyBindingInputWidgetState *>(data);
  state->focused = false;
  state->pressed_modifier_keys.clear();
  state->display_mode = KeyBindingInputDisplayMode::confirmed;
  set_entry_presentation(state, state->confirmed_text);
  update_validation_presentation(state);
  return GDK_EVENT_PROPAGATE;
}

static gboolean on_entry_key_press(GtkWidget *, GdkEventKey *event,
                                   gpointer data) {
  auto *state = static_cast<KeyBindingInputWidgetState *>(data);
  if (event == nullptr) {
    return GDK_EVENT_PROPAGATE;
  }

  const auto modifier = modifier_for_keyval(event->keyval);
  if (modifier.has_value() || event->is_modifier != 0) {
    if (modifier.has_value()) {
      const bool inserted =
          state->pressed_modifier_keys.insert(event->keyval).second;
      if (inserted) {
        state->display_mode = KeyBindingInputDisplayMode::modifiers;
      }
      if (state->display_mode == KeyBindingInputDisplayMode::modifiers) {
        show_modifier_presentation(state);
      }
    }
    return GDK_EVENT_STOP;
  }

  const auto binding = format_binding(
      event->keyval,
      static_cast<GdkModifierType>(
          (event->state & supported_modifier_mask) |
          pressed_modifier_mask(state)));
  if (binding.has_value()) {
    commit_confirmed_text(state, *binding);
  }
  return GDK_EVENT_STOP;
}

static gboolean on_entry_key_release(GtkWidget *, GdkEventKey *event,
                                     gpointer data) {
  auto *state = static_cast<KeyBindingInputWidgetState *>(data);
  if (event == nullptr) {
    return GDK_EVENT_PROPAGATE;
  }

  const auto modifier = modifier_for_keyval(event->keyval);
  if (modifier.has_value()) {
    state->pressed_modifier_keys.erase(event->keyval);
    if (state->display_mode == KeyBindingInputDisplayMode::modifiers) {
      show_modifier_presentation(state);
    }
  }
  return GDK_EVENT_STOP;
}

static void on_entry_icon_press(GtkEntry *,
                                GtkEntryIconPosition icon_position,
                                GdkEvent *, gpointer data) {
  auto *state = static_cast<KeyBindingInputWidgetState *>(data);
  if (icon_position != GTK_ENTRY_ICON_SECONDARY ||
      !validation_error(state).empty()) {
    return;
  }
  if (state->confirmed_text.empty() && state->empty_clear_enabled) {
    state->empty_clear_enabled = false;
    update_validation_presentation(state);
    if (state->changed) {
      state->changed();
    }
    return;
  }
  commit_confirmed_text(state, {});
}

KeyBindingInputWidgetState *
create_key_binding_input_widget(KeyBindingInputWidgetOptions options) {
  auto *state = new KeyBindingInputWidgetState();
  state->changed = std::move(options.changed);
  state->confirmed_text = std::move(options.text);
  state->entry = gtk_entry_new();
  g_object_add_weak_pointer(G_OBJECT(state->entry),
                            reinterpret_cast<gpointer *>(&state->entry));
  assign_accessible_id(state->entry, options.accessible_id);
  gtk_editable_set_editable(GTK_EDITABLE(state->entry), FALSE);
  gtk_entry_set_placeholder_text(
      GTK_ENTRY(state->entry),
      settings_ui_text(SettingsUiText::press_key_combination));
  gtk_widget_add_events(state->entry, GDK_KEY_PRESS_MASK |
                                          GDK_KEY_RELEASE_MASK |
                                          GDK_FOCUS_CHANGE_MASK);
  set_entry_presentation(state, state->confirmed_text);
  validate_confirmed_text(state);
  g_signal_connect(state->entry, "focus-in-event",
                   G_CALLBACK(on_entry_focus_in), state);
  g_signal_connect(state->entry, "focus-out-event",
                   G_CALLBACK(on_entry_focus_out), state);
  g_signal_connect(state->entry, "key-press-event",
                   G_CALLBACK(on_entry_key_press), state);
  g_signal_connect(state->entry, "key-release-event",
                   G_CALLBACK(on_entry_key_release), state);
  g_signal_connect(state->entry, "icon-press",
                   G_CALLBACK(on_entry_icon_press), state);
  return state;
}

GtkWidget *key_binding_input_widget_root(KeyBindingInputWidgetState *state) {
  return state == nullptr ? nullptr : state->entry;
}

void set_key_binding_input_widget_text(KeyBindingInputWidgetState *state,
                                       const std::string &text) {
  if (state == nullptr) {
    return;
  }

  state->pressed_modifier_keys.clear();
  commit_confirmed_text(state, text);
  if (state->focused) {
    state->display_mode = KeyBindingInputDisplayMode::waiting;
    set_entry_presentation(state, {});
  }
}

void set_key_binding_input_widget_empty_clear_enabled(
    KeyBindingInputWidgetState *state, bool enabled) {
  if (state == nullptr) {
    return;
  }
  state->empty_clear_enabled = enabled;
  update_validation_presentation(state);
}

std::string key_binding_input_widget_text(
    const KeyBindingInputWidgetState *state) {
  return state == nullptr ? std::string() : state->confirmed_text;
}

bool key_binding_input_widget_is_valid(
    const KeyBindingInputWidgetState *state) {
  return state != nullptr && state->syntax_error.empty() &&
         state->external_error.empty();
}

void set_key_binding_input_widget_external_error(
    KeyBindingInputWidgetState *state, const std::string &error) {
  if (state == nullptr) {
    return;
  }
  state->external_error = error;
  update_validation_presentation(state);
}

void destroy_key_binding_input_widget(KeyBindingInputWidgetState *state) {
  if (state == nullptr) {
    return;
  }
  if (state->entry != nullptr) {
    g_signal_handlers_disconnect_by_data(state->entry, state);
    g_object_remove_weak_pointer(
        G_OBJECT(state->entry),
        reinterpret_cast<gpointer *>(&state->entry));
  }
  delete state;
}

} // namespace elder_terms
