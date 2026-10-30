// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "smb-settings-editor.h"

#include "settings-presentation.h"

#include <algorithm>
#include <charconv>
#include <string_view>
#include <utility>
#include <vector>

#define GETTEXT_PACKAGE "elder-terms"
#include <glib/gi18n-lib.h>

namespace elder_terms {

static void assign_accessible_id(GtkWidget *widget, const std::string &id) {
  gtk_widget_set_name(widget, id.c_str());
  atk_object_set_accessible_id(gtk_widget_get_accessible(widget), id.c_str());
}

struct SmbField {
  const char *name;
  bool boolean = false;
  GtkWidget *entry = nullptr;
  GtkWidget *combo = nullptr;
  bool valid = true;
};

struct SmbSettingsEditor {
  SettingsStore *store;
  std::string prefix;
  bool read_only;
  std::function<void()> changed;
  GtkWidget *root;
  GtkWidget *error;
  bool synchronizing = false;
  std::vector<SmbField> fields{
      {"address"}, {"port"}, {"domain"}, {"username"},
      {"local_directory"}, {"remote_directory"},
      {"require_signing", true}, {"require_encryption", true},
      {"connect_timeout_seconds"}, {"idle_timeout_seconds"}};
};

static bool numeric_field(std::string_view name) {
  return name == "port" || name.ends_with("_timeout_seconds");
}

static std::string effective_text(const SettingsStore &store, const char *name) {
  for (const auto &entry : store.entries)
    if (entry.definition.key.section == "smb" && entry.definition.key.name == name && entry.invalid_raw_value)
      return *entry.invalid_raw_value;
  if (numeric_field(name))
    return std::to_string(setting_integer_value_or_default(store, smb_setting_key(name), 0));
  return setting_string_value_or_default(store, smb_setting_key(name), "");
}

static void update_validation(SmbSettingsEditor *editor) {
  auto errors = smb_connection_settings(*editor->store).validation_errors;
  for (auto &field : editor->fields) {
    if (field.entry) {
      gtk_widget_set_sensitive(field.entry, !editor->read_only);
      gtk_entry_set_icon_from_icon_name(GTK_ENTRY(field.entry), GTK_ENTRY_ICON_SECONDARY,
                                        field.valid ? nullptr : "dialog-error-symbolic");
    }
    if (field.combo) gtk_widget_set_sensitive(field.combo, !editor->read_only);
    if (!field.valid)
      errors.push_back(setting_label(smb_setting_key(field.name)) + ": " + _("Enter a valid value"));
  }
  gtk_label_set_text(GTK_LABEL(editor->error), errors.empty() ? "" : errors.front().c_str());
}

void sync_smb_settings_editor(SmbSettingsEditor *editor, bool preserve_invalid) {
  if (!editor) return;
  editor->synchronizing = true;
  for (auto &field : editor->fields) {
    const bool unfinished = preserve_invalid && !field.valid && field.entry;
    const std::string unfinished_text = unfinished ? gtk_entry_get_text(GTK_ENTRY(field.entry)) : "";
    const auto key = smb_setting_key(field.name);
    SettingsStore fallback = *editor->store;
    clear_explicit_setting_value(&fallback, key);
    const bool explicit_value = setting_has_explicit_value(*editor->store, key);
    if (field.boolean) {
      const bool inherited = setting_boolean_value_or_default(fallback, key, false);
      const bool effective = setting_boolean_value_or_default(*editor->store, key, false);
      gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(field.combo));
      gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(field.combo), "inherit",
          inherited_setting_label(inherited ? _("Enabled") : _("Disabled"),
                                  setting_fallback_source(*editor->store, key)).c_str());
      gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(field.combo), "enabled", _("Enabled"));
      gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(field.combo), "disabled", _("Disabled"));
      gtk_combo_box_set_active_id(GTK_COMBO_BOX(field.combo),
          explicit_value ? (effective ? "enabled" : "disabled") : "inherit");
    } else {
      const auto effective = effective_text(*editor->store, field.name);
      const auto inherited = effective_text(fallback, field.name);
      gtk_entry_set_text(GTK_ENTRY(field.entry), explicit_value ? effective.c_str() : "");
      gtk_entry_set_placeholder_text(GTK_ENTRY(field.entry),
          inherited_setting_label(inherited, setting_fallback_source(*editor->store, key)).c_str());
      field.valid = !unfinished;
      if (unfinished) gtk_entry_set_text(GTK_ENTRY(field.entry), unfinished_text.c_str());
    }
  }
  update_validation(editor);
  editor->synchronizing = false;
}

static void entry_changed(GtkEditable *widget, gpointer data) {
  auto *editor = static_cast<SmbSettingsEditor *>(data);
  if (editor->synchronizing || editor->read_only) return;
  for (auto &field : editor->fields) {
    if (field.entry != GTK_WIDGET(widget)) continue;
    const auto key = smb_setting_key(field.name);
    const std::string value = gtk_entry_get_text(GTK_ENTRY(widget));
    if (value.empty()) {
      clear_explicit_setting_value(editor->store, key);
      field.valid = true;
    } else if (numeric_field(field.name)) {
      gint64 number = 0;
      const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
      field.valid = parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() &&
          set_explicit_setting_value(editor->store, key, number);
    } else {
      field.valid = set_explicit_setting_value(editor->store, key, value);
    }
  }
  update_validation(editor);
  editor->changed();
}

static void combo_changed(GtkComboBox *widget, gpointer data) {
  auto *editor = static_cast<SmbSettingsEditor *>(data);
  if (editor->synchronizing || editor->read_only) return;
  for (auto &field : editor->fields) {
    if (field.combo != GTK_WIDGET(widget)) continue;
    const auto *choice = gtk_combo_box_get_active_id(widget);
    if (!choice) return;
    const auto key = smb_setting_key(field.name);
    if (std::string_view(choice) == "inherit") clear_explicit_setting_value(editor->store, key);
    else field.valid = set_explicit_setting_value(editor->store, key,
                                                  std::string_view(choice) == "enabled");
  }
  update_validation(editor);
  editor->changed();
}

SmbSettingsEditor *create_smb_settings_editor(SettingsStore *store,
    std::string prefix, bool read_only, std::function<void()> changed,
    std::function<GtkWidget *(GtkWidget *)> create_address_row) {
  auto *editor = new SmbSettingsEditor{store, std::move(prefix), read_only,
      std::move(changed), gtk_grid_new(), gtk_label_new("")};
  gtk_grid_set_row_spacing(GTK_GRID(editor->root), 8);
  gtk_grid_set_column_spacing(GTK_GRID(editor->root), 12);
  gtk_container_set_border_width(GTK_CONTAINER(editor->root), 12);
  int row = 0;
  for (auto &field : editor->fields) {
    const auto id = editor->prefix + "_smb_" + field.name;
    auto *label = gtk_label_new(setting_label(smb_setting_key(field.name)).c_str());
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    GtkWidget *control;
    if (field.boolean) {
      field.combo = gtk_combo_box_text_new();
      assign_accessible_id(field.combo, id + "_combo");
      g_signal_connect(field.combo, "changed", G_CALLBACK(combo_changed), editor);
      control = field.combo;
    } else {
      field.entry = gtk_entry_new();
      assign_accessible_id(field.entry, id + "_entry");
      g_signal_connect(field.entry, "changed", G_CALLBACK(entry_changed), editor);
      control = std::string_view(field.name) == "address"
          ? create_address_row(field.entry) : field.entry;
    }
    gtk_widget_set_hexpand(control, TRUE);
    gtk_grid_attach(GTK_GRID(editor->root), label, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(editor->root), control, 1, row++, 1, 1);
  }
  assign_accessible_id(editor->error, editor->prefix + "_smb_error_label");
  gtk_label_set_line_wrap(GTK_LABEL(editor->error), TRUE);
  gtk_grid_attach(GTK_GRID(editor->root), editor->error, 0, row, 2, 1);
  sync_smb_settings_editor(editor, false);
  return editor;
}

GtkWidget *smb_settings_editor_root(SmbSettingsEditor *editor) { return editor->root; }

bool smb_settings_editor_is_valid(const SmbSettingsEditor *editor) {
  return !editor || (smb_connection_settings(*editor->store).validation_errors.empty() &&
      std::all_of(editor->fields.begin(), editor->fields.end(),
          [](const auto &field) { return field.valid; }));
}

void destroy_smb_settings_editor(SmbSettingsEditor *editor) { delete editor; }

} // namespace elder_terms
