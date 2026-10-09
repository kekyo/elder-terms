#include "ssh-proxy-settings-editor.h"
#include "settings-presentation.h"

#include <algorithm>
#include <charconv>

#define GETTEXT_PACKAGE "elder-terms"
#include <glib/gi18n-lib.h>

namespace elder_terms {

struct SshProxyField {
  const char *name;
  GtkWidget *entry = nullptr;
  bool valid = true;
};

struct SshProxySettingsEditor {
  SettingsStore *store;
  std::string prefix;
  bool read_only;
  bool global;
  std::function<void()> changed;
  GtkWidget *root;
  GtkWidget *enabled;
  GtkWidget *error;
  bool synchronizing = false;
  std::vector<SshProxyField> fields{{"address"}, {"port"}, {"username"}, {"identity_file"}};
};

static void assign_id(GtkWidget *widget, const std::string &id) {
  gtk_widget_set_name(widget, id.c_str());
  atk_object_set_accessible_id(gtk_widget_get_accessible(widget), id.c_str());
}

static std::string value_text(const SettingsStore &store, const char *name) {
  for (const auto &entry : store.entries)
    if (entry.definition.key.section == "ssh_proxy" && entry.definition.key.name == name && entry.invalid_raw_value)
      return *entry.invalid_raw_value;
  const auto key = ssh_proxy_setting_key(name);
  if (std::string_view(name) == "port") return std::to_string(setting_integer_value_or_default(store, key, 22));
  if (std::string_view(name) == "enabled") return setting_boolean_value_or_default(store, key, false) ? "true" : "false";
  return setting_string_value_or_default(store, key, "");
}

static void update_validation(SshProxySettingsEditor *editor) {
  const auto settings = ssh_proxy_settings(*editor->store);
  auto errors = settings.validation_errors;
  gtk_widget_set_sensitive(editor->enabled, !editor->read_only);
  for (auto &field : editor->fields) {
    gtk_widget_set_sensitive(field.entry, !editor->read_only && settings.enabled);
    gtk_entry_set_icon_from_icon_name(GTK_ENTRY(field.entry), GTK_ENTRY_ICON_SECONDARY,
        field.valid ? nullptr : "dialog-error-symbolic");
    if (settings.enabled && !field.valid)
      errors.push_back(setting_label(ssh_proxy_setting_key(field.name)) + ": " + _("Enter a valid value"));
  }
  gtk_label_set_text(GTK_LABEL(editor->error), errors.empty() ? "" : errors.front().c_str());
}

void sync_ssh_proxy_settings_editor(SshProxySettingsEditor *editor, bool preserve_invalid) {
  if (!editor) return;
  editor->synchronizing = true;
  const auto enabled_key = ssh_proxy_setting_key("enabled");
  auto fallback = *editor->store;
  clear_explicit_setting_value(&fallback, enabled_key);
  const auto enabled_text = value_text(*editor->store, "enabled");
  const auto inherited = inherited_setting_label(
      value_text(fallback, "enabled") == "true" ? _("Enabled") : _("Disabled"),
      setting_fallback_source(*editor->store, enabled_key));
  auto *combo = GTK_COMBO_BOX_TEXT(editor->enabled);
  gtk_combo_box_text_remove_all(combo);
  gtk_combo_box_text_append(combo, "inherit", inherited.c_str());
  gtk_combo_box_text_append(combo, "false", _("Disabled"));
  gtk_combo_box_text_append(combo, "true", _("Enabled"));
  if (enabled_text != "true" && enabled_text != "false")
    gtk_combo_box_text_append(combo, enabled_text.c_str(), (std::string(_("Invalid value:")) + " " + enabled_text).c_str());
  gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo),
      setting_has_explicit_value(*editor->store, enabled_key) ? enabled_text.c_str() : "inherit");
  for (auto &field : editor->fields) {
    const auto key = ssh_proxy_setting_key(field.name);
    fallback = *editor->store;
    clear_explicit_setting_value(&fallback, key);
    const auto placeholder = inherited_setting_label(value_text(fallback, field.name),
        setting_fallback_source(*editor->store, key));
    gtk_entry_set_placeholder_text(GTK_ENTRY(field.entry), placeholder.c_str());
    if (preserve_invalid && !field.valid) continue;
    gtk_entry_set_text(GTK_ENTRY(field.entry), setting_has_explicit_value(*editor->store, key)
        ? value_text(*editor->store, field.name).c_str() : "");
    field.valid = true;
  }
  update_validation(editor);
  editor->synchronizing = false;
}

static void enabled_changed(GtkComboBox *combo, gpointer data) {
  auto *editor = static_cast<SshProxySettingsEditor *>(data);
  if (editor->synchronizing || editor->read_only) return;
  const auto *id = gtk_combo_box_get_active_id(combo);
  if (!id) return;
  const auto key = ssh_proxy_setting_key("enabled");
  if (std::string_view(id) == "inherit") clear_explicit_setting_value(editor->store, key);
  else if (std::string_view(id) == "true" || std::string_view(id) == "false")
    set_explicit_setting_value(editor->store, key, std::string_view(id) == "true");
  update_validation(editor);
  editor->changed();
}

static void entry_changed(GtkEditable *widget, gpointer data) {
  auto *editor = static_cast<SshProxySettingsEditor *>(data);
  if (editor->synchronizing || editor->read_only) return;
  for (auto &field : editor->fields) {
    if (field.entry != GTK_WIDGET(widget)) continue;
    const auto key = ssh_proxy_setting_key(field.name);
    const std::string text = gtk_entry_get_text(GTK_ENTRY(widget));
    if (text.empty()) {
      clear_explicit_setting_value(editor->store, key);
      field.valid = true;
    } else if (std::string_view(field.name) == "port") {
      gint64 port = 0;
      const auto parsed = std::from_chars(text.data(), text.data() + text.size(), port);
      field.valid = parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() &&
          set_explicit_setting_value(editor->store, key, port);
    } else field.valid = set_explicit_setting_value(editor->store, key, text);
  }
  update_validation(editor);
  editor->changed();
}

SshProxySettingsEditor *create_ssh_proxy_settings_editor(SettingsStore *store,
    std::string prefix, bool read_only, bool global, std::function<void()> changed) {
  auto *editor = new SshProxySettingsEditor{store, std::move(prefix), read_only, global,
      std::move(changed), gtk_grid_new(), gtk_combo_box_text_new(), gtk_label_new("")};
  gtk_grid_set_row_spacing(GTK_GRID(editor->root), 8);
  gtk_grid_set_column_spacing(GTK_GRID(editor->root), 12);
  gtk_container_set_border_width(GTK_CONTAINER(editor->root), 12);
  assign_id(editor->root, editor->prefix + "_ssh_proxy_page");
  assign_id(editor->enabled, editor->prefix + "_ssh_proxy_enabled_combo");
  auto *label = gtk_label_new(_("Use SSH proxy"));
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  gtk_grid_attach(GTK_GRID(editor->root), label, 0, 0, 1, 1);
  gtk_grid_attach(GTK_GRID(editor->root), editor->enabled, 1, 0, 1, 1);
  g_signal_connect(editor->enabled, "changed", G_CALLBACK(enabled_changed), editor);
  int row = 1;
  for (auto &field : editor->fields) {
    label = gtk_label_new(setting_label(ssh_proxy_setting_key(field.name)).c_str());
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    field.entry = gtk_entry_new();
    gtk_widget_set_hexpand(field.entry, TRUE);
    assign_id(field.entry, editor->prefix + "_ssh_proxy_" + field.name + "_entry");
    gtk_grid_attach(GTK_GRID(editor->root), label, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(editor->root), field.entry, 1, row++, 1, 1);
    g_signal_connect(field.entry, "changed", G_CALLBACK(entry_changed), editor);
  }
  gtk_label_set_line_wrap(GTK_LABEL(editor->error), TRUE);
  gtk_grid_attach(GTK_GRID(editor->root), editor->error, 0, row, 2, 1);
  sync_ssh_proxy_settings_editor(editor, false);
  return editor;
}

GtkWidget *ssh_proxy_settings_editor_root(SshProxySettingsEditor *editor) { return editor->root; }

bool ssh_proxy_settings_editor_is_valid(const SshProxySettingsEditor *editor) {
  if (!editor) return true;
  const auto settings = editor->global ? ssh_proxy_settings(*editor->store) : ssh_proxy_connection_settings(*editor->store);
  return settings.validation_errors.empty() && (!settings.enabled ||
      std::all_of(editor->fields.begin(), editor->fields.end(), [](const auto &field) { return field.valid; }));
}

void destroy_ssh_proxy_settings_editor(SshProxySettingsEditor *editor) { delete editor; }

} // namespace elder_terms
