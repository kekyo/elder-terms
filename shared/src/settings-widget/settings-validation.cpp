// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "settings-validation.h"
#include "settings-presentation.h"
#include <optional>

namespace elder_terms {

struct InputValidation {
  std::string message;
  std::optional<std::string> original_tooltip;
  std::optional<std::string> original_description;
};

static constexpr char validation_key[] = "elder-terms-input-validation";

void set_settings_input_error(GtkWidget *widget, const std::string &reason) {
  if (!widget) return;
  auto *state = static_cast<InputValidation *>(g_object_get_data(G_OBJECT(widget), validation_key));
  if (!state) {
    if (reason.empty()) return;
    state = new InputValidation;
    g_object_set_data_full(G_OBJECT(widget), validation_key, state,
        +[](gpointer value) { delete static_cast<InputValidation *>(value); });
  }
  const auto message = settings_validation_message(reason);
  if (state->message == message) return;
  auto *accessible = gtk_widget_get_accessible(widget);
  if (state->message.empty()) {
    auto *tooltip = gtk_widget_get_tooltip_text(widget);
    state->original_tooltip = tooltip ? std::optional<std::string>(tooltip) : std::nullopt;
    g_free(tooltip);
    const auto *description = atk_object_get_description(accessible);
    state->original_description = description ? std::optional<std::string>(description) : std::nullopt;
  }
  state->message = message;
  auto *context = gtk_widget_get_style_context(widget);
  if (message.empty()) {
    gtk_style_context_remove_class(context, GTK_STYLE_CLASS_ERROR);
    gtk_widget_set_tooltip_text(widget, state->original_tooltip ? state->original_tooltip->c_str() : nullptr);
    atk_object_set_description(accessible, state->original_description ? state->original_description->c_str() : "");
  } else {
    gtk_style_context_add_class(context, GTK_STYLE_CLASS_ERROR);
    gtk_widget_set_tooltip_text(widget, message.c_str());
    atk_object_set_description(accessible, message.c_str());
  }
  if (GTK_IS_ENTRY(widget)) {
    gtk_entry_set_icon_from_icon_name(GTK_ENTRY(widget), GTK_ENTRY_ICON_SECONDARY,
        message.empty() ? nullptr : "dialog-error-symbolic");
    gtk_entry_set_icon_activatable(GTK_ENTRY(widget), GTK_ENTRY_ICON_SECONDARY, FALSE);
    gtk_entry_set_icon_tooltip_text(GTK_ENTRY(widget), GTK_ENTRY_ICON_SECONDARY,
        message.empty() ? nullptr : message.c_str());
  }
}

std::string settings_input_error(GtkWidget *widget) {
  const auto *state = widget ? static_cast<InputValidation *>(
      g_object_get_data(G_OBJECT(widget), validation_key)) : nullptr;
  return state ? state->message : std::string();
}

} // namespace elder_terms
