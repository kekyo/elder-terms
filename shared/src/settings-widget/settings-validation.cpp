// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "settings-validation.h"
#include "settings-presentation.h"
#include <algorithm>
#include <charconv>
#include <optional>

namespace elder_terms {

struct InputValidation {
  std::string message;
  std::string input_error;
  std::string constraint_error;
  std::optional<SettingKey> field;
  GtkWidget *indicator = nullptr;
  bool enabled = true;
  std::optional<std::string> original_tooltip;
  std::optional<std::string> original_description;
};

static constexpr char validation_key[] = "elder-terms-input-validation";

static InputValidation *validation_state(GtkWidget *widget) {
  auto *state = static_cast<InputValidation *>(g_object_get_data(G_OBJECT(widget), validation_key));
  if (!state) {
    state = new InputValidation;
    g_object_set_data_full(G_OBJECT(widget), validation_key, state,
        +[](gpointer value) { delete static_cast<InputValidation *>(value); });
  }
  return state;
}

static void present_validation(GtkWidget *widget, InputValidation *state) {
  // A rejected draft takes precedence over constraints on the last accepted value.
  const auto message = state->enabled ? settings_validation_message(
      state->input_error.empty() ? state->constraint_error : state->input_error) : std::string();
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

void set_settings_input_error(GtkWidget *widget, const std::string &reason) {
  if (!widget) return;
  auto *state = validation_state(widget);
  state->input_error = reason;
  present_validation(widget, state);
}

void set_settings_validation_enabled(GtkWidget *root, bool enabled) {
  if (!root) return;
  auto *state = static_cast<InputValidation *>(g_object_get_data(G_OBJECT(root), validation_key));
  if (state) {
    state->enabled = enabled;
    present_validation(root, state);
  }
  if (GTK_IS_CONTAINER(root)) {
    auto *children = gtk_container_get_children(GTK_CONTAINER(root));
    for (auto *child = children; child; child = child->next)
      set_settings_validation_enabled(GTK_WIDGET(child->data), enabled);
    g_list_free(children);
  }
}

void bind_settings_field(GtkWidget *widget, const SettingKey &key) {
  if (GTK_IS_ENTRY(widget) || GTK_IS_COMBO_BOX(widget) || GTK_IS_TEXT_VIEW(widget)) {
    validation_state(widget)->field = key;
  } else if (GTK_IS_CONTAINER(widget) && !GTK_IS_BUTTON(widget)) {
    auto *children = gtk_container_get_children(GTK_CONTAINER(widget));
    for (auto *child = children; child; child = child->next)
      bind_settings_field(GTK_WIDGET(child->data), key);
    g_list_free(children);
  }
}

GtkWidget *decorate_settings_control(GtkWidget *control) {
  if (GTK_IS_ENTRY(control)) return control;
  auto *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  auto *indicator = gtk_image_new_from_icon_name("dialog-error-symbolic", GTK_ICON_SIZE_MENU);
  gtk_style_context_add_class(gtk_widget_get_style_context(indicator), GTK_STYLE_CLASS_ERROR);
  gtk_widget_set_no_show_all(indicator, TRUE);
  gtk_widget_set_valign(indicator, GTK_ALIGN_START);
  gtk_box_pack_start(GTK_BOX(row), control, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(row), indicator, FALSE, FALSE, 0);
  validation_state(control)->indicator = indicator;
  return row;
}

static void collect_validation(GtkWidget *root, std::vector<std::string> &messages, bool include_entries) {
  if (!root) return;
  const auto message = settings_input_error(root);
  if ((include_entries || !GTK_IS_ENTRY(root)) && !message.empty() &&
      std::find(messages.begin(), messages.end(), message) == messages.end())
    messages.push_back(message);
  if (GTK_IS_CONTAINER(root)) {
    auto *children = gtk_container_get_children(GTK_CONTAINER(root));
    for (auto *child = children; child; child = child->next)
      collect_validation(GTK_WIDGET(child->data), messages, include_entries);
    g_list_free(children);
  }
}

std::string settings_validation_summary(GtkWidget *root) {
  std::vector<std::string> messages;
  collect_validation(root, messages, true);
  std::string summary;
  for (const auto &message : messages) {
    if (!summary.empty()) summary += '\n';
    summary += message;
  }
  return summary;
}

void refresh_settings_validation(GtkWidget *root, const std::vector<SettingValidationIssue> &issues) {
  if (!root) return;
  auto *state = static_cast<InputValidation *>(g_object_get_data(G_OBJECT(root), validation_key));
  if (state && state->field) {
    state->constraint_error.clear();
    for (const auto &issue : issues) {
      if (std::any_of(issue.fields.begin(), issue.fields.end(), [&](const auto &key) {
            return key.section == state->field->section && key.name == state->field->name;
          })) {
        if (!state->constraint_error.empty()) state->constraint_error += '\n';
        state->constraint_error += settings_validation_message(issue.message);
      }
    }
    present_validation(root, state);
  }
  if (GTK_IS_CONTAINER(root)) {
    auto *children = gtk_container_get_children(GTK_CONTAINER(root));
    for (auto *child = children; child; child = child->next)
      refresh_settings_validation(GTK_WIDGET(child->data), issues);
    g_list_free(children);
  }
  if (state && state->indicator) {
    // Entries already have an inline icon; the adjacent icon is for other controls.
    std::vector<std::string> messages;
    collect_validation(root, messages, false);
    std::string summary;
    for (const auto &message : messages) {
      if (!summary.empty()) summary += '\n';
      summary += message;
    }
    gtk_widget_set_visible(state->indicator, !summary.empty());
    gtk_widget_set_tooltip_text(state->indicator, summary.empty() ? nullptr : summary.c_str());
  }
}

std::string settings_input_error(GtkWidget *widget) {
  const auto *state = widget ? static_cast<InputValidation *>(
      g_object_get_data(G_OBJECT(widget), validation_key)) : nullptr;
  return state ? state->message : std::string();
}

void set_settings_argument_error(GtkWidget *rows, std::size_t index, const std::string &reason) {
  auto *children = gtk_container_get_children(GTK_CONTAINER(rows));
  std::size_t position = 0;
  for (auto *child = children; child; child = child->next, ++position) {
    auto *controls = gtk_container_get_children(GTK_CONTAINER(child->data));
    if (controls && GTK_IS_ENTRY(controls->data))
      set_settings_input_error(GTK_WIDGET(controls->data), position == index ? reason : "");
    g_list_free(controls);
  }
  g_list_free(children);
}

std::string settings_rejected_input_reason(const SettingsStore &store, const SettingKey &key, GtkWidget *widget) {
  for (const auto &entry : store.entries) {
    if (entry.definition.key.section != key.section || entry.definition.key.name != key.name) continue;
    if (!GTK_IS_ENTRY(widget)) return entry.validation_error.empty() ? "Enter a valid value" : entry.validation_error;
    const std::string text = gtk_entry_get_text(GTK_ENTRY(widget));
    if (text.empty()) return "must not be empty";
    SettingValue value = text;
    if (std::holds_alternative<gint64>(entry.definition.default_value)) {
      gint64 number = 0;
      const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
      if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return "Enter a whole number";
      value = number;
    }
    std::string reason;
    if (entry.definition.validate && !entry.definition.validate(value, &reason)) return reason;
    if (!entry.validation_error.empty()) return entry.validation_error;
  }
  return "Enter a valid value";
}

} // namespace elder_terms
