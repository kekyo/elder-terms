#include <elder-terms/application-settings-widget.h>

#include <string>
#include <utility>

#include <elder-terms/key-binding-input-widget.h>
#include <elder-terms/settings/application-settings.h>

#define GETTEXT_PACKAGE "elder-terms"
#include <glib/gi18n-lib.h>

#include "hyperlink-settings-editor.h"
#include "settings-presentation.h"

namespace elder_terms {

static constexpr char inherit_choice[] = "inherit";
static constexpr char ui_language_system[] = "system";
static constexpr char startup_window[] = "window";
static constexpr char startup_background[] = "background";
static constexpr char startup_tray[] = "tray";
static constexpr char startup_window_and_tray[] = "window_and_tray";

static constexpr const char *ui_languages[] = {
    "system", "en", "ar", "es", "fr", "hi", "ja", "ko", "pt", "ru", "zh",
};

static constexpr const char *startup_modes[] = {
    startup_window,
    startup_background,
    startup_tray,
    startup_window_and_tray,
};

struct ApplicationSettingsWidgetState {
  SettingsStore draft_store;
  std::string id_prefix;
  ApplicationSettingsWidgetChangedCallback changed;
  bool synchronizing = false;
  GtkWidget *root = nullptr;
  GtkWidget *ui_language_combo = nullptr;
  GtkWidget *startup_mode_combo = nullptr;
  KeyBindingInputWidgetState *open_application_input = nullptr;
  GtkWidget *open_application_reset_button = nullptr;
  HyperlinkSettingsEditorState *hyperlink_editor = nullptr;
};

static void assign_accessible_id(GtkWidget *widget, const char *id) {
  if (widget == nullptr || id == nullptr || id[0] == '\0') {
    return;
  }
  gtk_widget_set_name(widget, id);
  if (g_object_class_find_property(G_OBJECT_GET_CLASS(widget),
                                   "accessible-id") != nullptr) {
    g_object_set(widget, "accessible-id", id, nullptr);
  }
  AtkObject *accessible = gtk_widget_get_accessible(widget);
  if (accessible != nullptr) {
    atk_object_set_accessible_id(accessible, id);
  }
}

static std::string widget_id(const ApplicationSettingsWidgetState *state,
                             const char *suffix) {
  return state->id_prefix + "_" + suffix;
}

static GtkWidget *create_row_label(const std::string &text) {
  GtkWidget *label = gtk_label_new(text.c_str());
  gtk_label_set_xalign(GTK_LABEL(label), 0.0F);
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  return label;
}

static void attach_row(GtkWidget *grid, int row, const SettingKey &key,
                       GtkWidget *control) {
  GtkWidget *label = create_row_label(setting_label(key));
  gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);
  gtk_widget_set_hexpand(control, TRUE);
  gtk_widget_set_halign(control, GTK_ALIGN_FILL);
  gtk_grid_attach(GTK_GRID(grid), control, 1, row, 1, 1);
}

static std::string active_combo_id(GtkWidget *combo, const char *fallback) {
  const char *active = gtk_combo_box_get_active_id(GTK_COMBO_BOX(combo));
  return active == nullptr ? fallback : active;
}

static void notify_changed(ApplicationSettingsWidgetState *state) {
  if (!state->synchronizing && state->changed) {
    state->changed();
  }
}

static void sync_hotkey(ApplicationSettingsWidgetState *state) {
  const SettingKey key = application_open_hotkey_setting_key();
  const bool explicit_value =
      setting_has_explicit_value(state->draft_store, key);
  const std::string effective =
      application_open_hotkey_text(state->draft_store);
  set_key_binding_input_widget_text(
      state->open_application_input,
      explicit_value ? effective : std::string());
  set_key_binding_input_widget_empty_clear_enabled(
      state->open_application_input,
      !explicit_value && !effective.empty());
  GtkWidget *entry =
      key_binding_input_widget_root(state->open_application_input);
  if (explicit_value && effective.empty()) {
    gtk_entry_set_placeholder_text(
        GTK_ENTRY(entry), settings_ui_text(SettingsUiText::disabled));
  } else if (explicit_value) {
    gtk_entry_set_placeholder_text(
        GTK_ENTRY(entry),
        settings_ui_text(SettingsUiText::press_key_combination));
  } else {
    const std::string placeholder = inherited_setting_label(
        effective.empty() ? settings_ui_text(SettingsUiText::disabled)
                          : effective,
        setting_fallback_source(state->draft_store, key));
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), placeholder.c_str());
  }
  gtk_widget_set_sensitive(state->open_application_reset_button,
                           explicit_value ? TRUE : FALSE);
  gtk_widget_set_tooltip_text(
      state->open_application_reset_button,
      settings_ui_text(SettingsUiText::use_built_in_default));
  set_key_binding_input_widget_external_error(
      state->open_application_input, {});
}

static void sync_widgets(ApplicationSettingsWidgetState *state) {
  const bool previous_synchronizing = state->synchronizing;
  state->synchronizing = true;

  gtk_combo_box_text_remove_all(
      GTK_COMBO_BOX_TEXT(state->ui_language_combo));
  for (const char *language : ui_languages) {
    const std::string label = setting_choice_label(
        application_ui_language_setting_key(), language);
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(state->ui_language_combo),
                              language, label.c_str());
  }
  gtk_combo_box_set_active_id(
      GTK_COMBO_BOX(state->ui_language_combo),
      application_ui_language_to_string(
          application_ui_language(state->draft_store)));

  gtk_combo_box_text_remove_all(
      GTK_COMBO_BOX_TEXT(state->startup_mode_combo));
  const std::string fallback = std::get<std::string>(setting_fallback_value(
      state->draft_store, application_startup_mode_setting_key(),
      SettingValue{std::string(startup_window)}));
  const std::string fallback_label = inherited_setting_label(
      setting_choice_label(application_startup_mode_setting_key(), fallback),
      setting_fallback_source(state->draft_store,
                              application_startup_mode_setting_key()));
  gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(state->startup_mode_combo),
                            inherit_choice, fallback_label.c_str());
  for (const char *mode : startup_modes) {
    const std::string label = setting_choice_label(
        application_startup_mode_setting_key(), mode);
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(state->startup_mode_combo),
                              mode, label.c_str());
  }
  const char *startup_active = inherit_choice;
  if (setting_has_explicit_value(
          state->draft_store, application_startup_mode_setting_key())) {
    startup_active = startup_mode_to_string(
        application_startup_mode(state->draft_store));
  }
  gtk_combo_box_set_active_id(GTK_COMBO_BOX(state->startup_mode_combo),
                              startup_active);
  sync_hotkey(state);

  state->synchronizing = previous_synchronizing;
}

static void on_ui_language_changed(GtkComboBox *, gpointer data) {
  auto *state = static_cast<ApplicationSettingsWidgetState *>(data);
  if (state->synchronizing) {
    return;
  }
  const std::string language =
      active_combo_id(state->ui_language_combo, ui_language_system);
  if (language == ui_language_system) {
    clear_explicit_setting_value(&state->draft_store,
                                 application_ui_language_setting_key());
  } else {
    set_explicit_setting_value(&state->draft_store,
                               application_ui_language_setting_key(),
                               SettingValue{language});
  }
  notify_changed(state);
}

static void on_startup_mode_changed(GtkComboBox *, gpointer data) {
  auto *state = static_cast<ApplicationSettingsWidgetState *>(data);
  if (state->synchronizing) {
    return;
  }
  const std::string mode =
      active_combo_id(state->startup_mode_combo, inherit_choice);
  if (mode == inherit_choice) {
    clear_explicit_setting_value(&state->draft_store,
                                 application_startup_mode_setting_key());
  } else {
    set_explicit_setting_value(&state->draft_store,
                               application_startup_mode_setting_key(),
                               SettingValue{mode});
  }
  notify_changed(state);
}

static void on_hotkey_changed(ApplicationSettingsWidgetState *state) {
  if (state->synchronizing) {
    return;
  }
  const std::string text =
      key_binding_input_widget_text(state->open_application_input);
  std::string reason;
  const bool valid = application_hotkey_text_is_valid(text, &reason);
  set_key_binding_input_widget_external_error(
      state->open_application_input, valid ? std::string() : reason);
  if (valid) {
    set_explicit_setting_value(&state->draft_store,
                               application_open_hotkey_setting_key(),
                               SettingValue{text});
    set_key_binding_input_widget_empty_clear_enabled(
        state->open_application_input, false);
    GtkWidget *entry =
        key_binding_input_widget_root(state->open_application_input);
    gtk_entry_set_placeholder_text(
        GTK_ENTRY(entry),
        text.empty() ? settings_ui_text(SettingsUiText::disabled)
                     : settings_ui_text(
                           SettingsUiText::press_key_combination));
    gtk_widget_set_sensitive(state->open_application_reset_button, TRUE);
  }
  notify_changed(state);
}

static void on_hotkey_reset_clicked(GtkButton *, gpointer data) {
  auto *state = static_cast<ApplicationSettingsWidgetState *>(data);
  clear_explicit_setting_value(&state->draft_store,
                               application_open_hotkey_setting_key());
  const bool previous_synchronizing = state->synchronizing;
  state->synchronizing = true;
  sync_hotkey(state);
  state->synchronizing = previous_synchronizing;
  notify_changed(state);
}

ApplicationSettingsWidgetState *
create_application_settings_widget(ApplicationSettingsWidgetOptions options) {
  auto *state = new ApplicationSettingsWidgetState();
  state->draft_store = std::move(options.store);
  state->id_prefix = std::move(options.id_prefix);
  state->changed = std::move(options.changed);

  state->root = gtk_notebook_new();
  assign_accessible_id(state->root,
                       widget_id(state, "notebook").c_str());
  GtkWidget *general_page = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(general_page), 12);
  gtk_grid_set_column_spacing(GTK_GRID(general_page), 16);
  gtk_widget_set_margin_top(general_page, 20);
  gtk_widget_set_margin_bottom(general_page, 20);
  gtk_widget_set_margin_start(general_page, 20);
  gtk_widget_set_margin_end(general_page, 20);
  assign_accessible_id(general_page,
                       widget_id(state, "general_page").c_str());

  state->ui_language_combo = gtk_combo_box_text_new();
  assign_accessible_id(state->ui_language_combo,
                       widget_id(state, "ui_language_combo").c_str());
  g_signal_connect(state->ui_language_combo, "changed",
                   G_CALLBACK(on_ui_language_changed), state);
  attach_row(general_page, 0, application_ui_language_setting_key(),
             state->ui_language_combo);

  state->startup_mode_combo = gtk_combo_box_text_new();
  assign_accessible_id(state->startup_mode_combo,
                       widget_id(state, "startup_mode_combo").c_str());
  g_signal_connect(state->startup_mode_combo, "changed",
                   G_CALLBACK(on_startup_mode_changed), state);
  attach_row(general_page, 1, application_startup_mode_setting_key(),
             state->startup_mode_combo);

  state->open_application_input = create_key_binding_input_widget({
      .text = "",
      .accessible_id = widget_id(state, "open_application_entry"),
      .changed = [state]() { on_hotkey_changed(state); },
  });
  GtkWidget *hotkey_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_pack_start(
      GTK_BOX(hotkey_row),
      key_binding_input_widget_root(state->open_application_input), TRUE,
      TRUE, 0);
  state->open_application_reset_button =
      gtk_button_new_with_label(settings_ui_text(SettingsUiText::reset));
  assign_accessible_id(
      state->open_application_reset_button,
      widget_id(state, "open_application_reset_button").c_str());
  g_signal_connect(state->open_application_reset_button, "clicked",
                   G_CALLBACK(on_hotkey_reset_clicked), state);
  gtk_box_pack_start(GTK_BOX(hotkey_row),
                     state->open_application_reset_button, FALSE, FALSE, 0);
  attach_row(general_page, 2, application_open_hotkey_setting_key(),
             hotkey_row);

  gtk_notebook_append_page(GTK_NOTEBOOK(state->root), general_page,
                           gtk_label_new(_("General")));
  state->hyperlink_editor = create_hyperlink_settings_editor({
      .store = &state->draft_store,
      .id_prefix = state->id_prefix,
      .changed = [state]() { notify_changed(state); },
  });
  gtk_notebook_append_page(
      GTK_NOTEBOOK(state->root),
      hyperlink_settings_editor_root(state->hyperlink_editor),
      gtk_label_new(_("Links")));

  sync_widgets(state);
  return state;
}

SettingsStore application_settings_widget_draft_store(
    const ApplicationSettingsWidgetState *state) {
  return state == nullptr ? SettingsStore{} : state->draft_store;
}

bool application_settings_widget_is_dirty(
    const ApplicationSettingsWidgetState *state) {
  return state != nullptr &&
         (settings_store_is_dirty(state->draft_store) ||
          !application_settings_widget_is_valid(state));
}

bool application_settings_widget_is_valid(
    const ApplicationSettingsWidgetState *state) {
  return state != nullptr && key_binding_input_widget_is_valid(
                                 state->open_application_input) &&
         hyperlink_settings_editor_is_valid(state->hyperlink_editor);
}

GtkWidget *application_settings_widget_root(
    ApplicationSettingsWidgetState *state) {
  return state == nullptr ? nullptr : state->root;
}

void destroy_application_settings_widget(
    ApplicationSettingsWidgetState *state) {
  if (state == nullptr) {
    return;
  }
  destroy_key_binding_input_widget(state->open_application_input);
  destroy_hyperlink_settings_editor(state->hyperlink_editor);
  if (state->root != nullptr && gtk_widget_get_parent(state->root) == nullptr) {
    gtk_widget_destroy(state->root);
  }
  delete state;
}

} // namespace elder_terms
