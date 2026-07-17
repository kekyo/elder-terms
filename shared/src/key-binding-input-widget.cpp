#include <elder-terms/key-binding-input-widget.h>

#include <string>
#include <utility>

#include <atk/atk.h>

#include <elder-terms/key-binding.h>

namespace elder_terms {

struct KeyBindingInputWidgetState {
  GtkWidget *entry = nullptr;
  KeyBindingInputChangedCallback changed;
  std::string syntax_error;
  std::string external_error;
};

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
  GtkStyleContext *context = gtk_widget_get_style_context(state->entry);
  if (error.empty()) {
    gtk_style_context_remove_class(context, GTK_STYLE_CLASS_ERROR);
    gtk_entry_set_icon_from_icon_name(GTK_ENTRY(state->entry),
                                      GTK_ENTRY_ICON_SECONDARY, nullptr);
    gtk_entry_set_icon_tooltip_text(GTK_ENTRY(state->entry),
                                    GTK_ENTRY_ICON_SECONDARY, nullptr);
    return;
  }

  gtk_style_context_add_class(context, GTK_STYLE_CLASS_ERROR);
  gtk_entry_set_icon_from_icon_name(GTK_ENTRY(state->entry),
                                    GTK_ENTRY_ICON_SECONDARY,
                                    "dialog-error-symbolic");
  gtk_entry_set_icon_tooltip_text(GTK_ENTRY(state->entry),
                                  GTK_ENTRY_ICON_SECONDARY, error.c_str());
}

static void validate_input(KeyBindingInputWidgetState *state) {
  const char *text = gtk_entry_get_text(GTK_ENTRY(state->entry));
  const KeyBindingParseResult parsed =
      parse_key_binding(text == nullptr ? std::string() : std::string(text));
  state->syntax_error = parsed.error;
  update_validation_presentation(state);
}

static void on_entry_changed(GtkEditable *, gpointer data) {
  auto *state = static_cast<KeyBindingInputWidgetState *>(data);
  state->external_error.clear();
  validate_input(state);
  if (state->changed) {
    state->changed();
  }
}

KeyBindingInputWidgetState *
create_key_binding_input_widget(KeyBindingInputWidgetOptions options) {
  auto *state = new KeyBindingInputWidgetState();
  state->changed = std::move(options.changed);
  state->entry = gtk_entry_new();
  g_object_add_weak_pointer(G_OBJECT(state->entry),
                            reinterpret_cast<gpointer *>(&state->entry));
  assign_accessible_id(state->entry, options.accessible_id);
  gtk_entry_set_placeholder_text(
      GTK_ENTRY(state->entry),
      "ctrl+plus (empty disables the binding)");
  gtk_entry_set_text(GTK_ENTRY(state->entry), options.text.c_str());
  validate_input(state);
  g_signal_connect(state->entry, "changed", G_CALLBACK(on_entry_changed),
                   state);
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
  gtk_entry_set_text(GTK_ENTRY(state->entry), text.c_str());
  validate_input(state);
}

std::string key_binding_input_widget_text(
    const KeyBindingInputWidgetState *state) {
  if (state == nullptr) {
    return {};
  }
  const char *text = gtk_entry_get_text(GTK_ENTRY(state->entry));
  return text == nullptr ? std::string() : std::string(text);
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
