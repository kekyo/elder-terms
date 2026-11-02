// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "nfs-settings-editor.h"
#include "settings-presentation.h"
#include <algorithm>
#include <charconv>
#include <sstream>
#include <utility>

#define GETTEXT_PACKAGE "elder-terms"
#include <glib/gi18n-lib.h>

namespace elder_terms {

enum class NfsFieldKind { text, integer, list, boolean, version };

struct NfsField {
  const char *name;
  NfsFieldKind kind = NfsFieldKind::text;
  NfsSettingsEditor *owner = nullptr;
  GtkWidget *control = nullptr;
  GtkWidget *inherit = nullptr;
  bool valid = true;
};

struct NfsSettingsEditor {
  SettingsStore *store;
  bool runtime;
  std::function<void()> changed;
  GtkWidget *root;
  GtkWidget *error;
  bool synchronizing = false;
  std::vector<NfsField> fields{
      {"address"}, {"version", NfsFieldKind::version}, {"filename_encoding"}, {"export_path"},
      {"known_exports", NfsFieldKind::list}, {"discover_exports", NfsFieldKind::boolean},
      {"namespace_path"}, {"known_namespace_paths", NfsFieldKind::list},
      {"discover_namespace", NfsFieldKind::boolean}, {"local_directory"},
      {"nfs_port", NfsFieldKind::integer}, {"mount_port", NfsFieldKind::integer},
      {"uid", NfsFieldKind::integer}, {"gid", NfsFieldKind::integer}, {"supplementary_gids", NfsFieldKind::list},
      {"connect_timeout_seconds", NfsFieldKind::integer}, {"request_timeout_seconds", NfsFieldKind::integer}};
};

static void assign_id(GtkWidget *widget, const std::string &id) {
  gtk_widget_set_name(widget, id.c_str());
  atk_object_set_accessible_id(gtk_widget_get_accessible(widget), id.c_str());
}

static std::string value_text(const SettingValue &value) {
  if (const auto *text = std::get_if<std::string>(&value)) return *text;
  if (const auto *integer = std::get_if<gint64>(&value)) return std::to_string(*integer);
  if (const auto *boolean = std::get_if<bool>(&value)) return *boolean ? _("Enabled") : _("Disabled");
  std::string result;
  for (const auto &part : std::get<std::vector<std::string>>(value)) {
    if (!result.empty()) result += '\n';
    result += part;
  }
  return result;
}

static void update_validation(NfsSettingsEditor *editor) {
  const auto settings = nfs_connection_settings(*editor->store);
  auto errors = settings.validation_errors;
  for (auto &field : editor->fields) {
    const std::string_view name(field.name);
    const bool applicable = settings.version == "4.0" ?
        name != "export_path" && name != "known_exports" && name != "discover_exports" && name != "mount_port" :
        name != "namespace_path" && name != "known_namespace_paths" && name != "discover_namespace";
    const bool inherited = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(field.inherit));
    const bool editable = applicable && (!editor->runtime || name == "filename_encoding");
    gtk_widget_set_sensitive(field.control, editable && !inherited);
    gtk_widget_set_sensitive(field.inherit, editable);
    if (GTK_IS_ENTRY(field.control))
      gtk_entry_set_icon_from_icon_name(GTK_ENTRY(field.control), GTK_ENTRY_ICON_SECONDARY,
          field.valid ? nullptr : "dialog-error-symbolic");
    if (!field.valid) errors.push_back(setting_label(nfs_setting_key(field.name)) + ": " + _("Enter a valid value"));
  }
  gtk_label_set_text(GTK_LABEL(editor->error), errors.empty() ? "" : errors.front().c_str());
}

void sync_nfs_settings_editor(NfsSettingsEditor *editor, bool preserve_invalid) {
  if (!editor) return;
  editor->synchronizing = true;
  for (auto &field : editor->fields) {
    const auto key = nfs_setting_key(field.name);
    const auto found = std::find_if(editor->store->entries.begin(), editor->store->entries.end(),
        [&](const auto &entry) { return entry.definition.key.section == key.section && entry.definition.key.name == key.name; });
    if (found == editor->store->entries.end()) continue;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(field.inherit), !setting_has_explicit_value(*editor->store, key));
    gtk_widget_set_tooltip_text(field.inherit,
        inherited_setting_label(value_text(found->fallback_value), setting_fallback_source(*editor->store, key)).c_str());
    if (preserve_invalid && !field.valid) continue;
    const auto text = found->invalid_raw_value.value_or(value_text(found->value));
    if (field.kind == NfsFieldKind::boolean)
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(field.control), std::get<bool>(found->value));
    else if (field.kind == NfsFieldKind::version)
      gtk_combo_box_set_active_id(GTK_COMBO_BOX(field.control), text.c_str());
    else if (field.kind == NfsFieldKind::list)
      gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(field.control)), text.c_str(), -1);
    else gtk_entry_set_text(GTK_ENTRY(field.control), text.c_str());
    field.valid = found->validation_error.empty();
  }
  update_validation(editor);
  editor->synchronizing = false;
}

static void field_changed(GObject *, gpointer data) {
  auto *field = static_cast<NfsField *>(data);
  auto *editor = field->owner;
  if (editor->synchronizing || (editor->runtime && std::string_view(field->name) != "filename_encoding")) return;
  const auto key = nfs_setting_key(field->name);
  if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(field->inherit))) {
    clear_explicit_setting_value(editor->store, key);
    field->valid = true;
    sync_nfs_settings_editor(editor, true);
  } else if (field->kind == NfsFieldKind::boolean) {
    field->valid = set_explicit_setting_value(editor->store, key,
        static_cast<bool>(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(field->control))));
  } else if (field->kind == NfsFieldKind::version) {
    const auto *choice = gtk_combo_box_get_active_id(GTK_COMBO_BOX(field->control));
    field->valid = choice && set_explicit_setting_value(editor->store, key, std::string(choice));
  } else if (field->kind == NfsFieldKind::list) {
    auto *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(field->control));
    GtkTextIter start{}, end{};
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    auto *text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
    std::istringstream input(text);
    g_free(text);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line)) lines.push_back(line);
    field->valid = set_explicit_setting_value(editor->store, key, std::move(lines));
  } else {
    const std::string text = gtk_entry_get_text(GTK_ENTRY(field->control));
    if (field->kind == NfsFieldKind::integer) {
      gint64 value = 0;
      const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
      field->valid = parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() &&
          set_explicit_setting_value(editor->store, key, value);
    } else field->valid = set_explicit_setting_value(editor->store, key, text);
  }
  update_validation(editor);
  editor->changed();
}

NfsSettingsEditor *create_nfs_settings_editor(SettingsStore *store, std::string prefix,
    bool runtime, std::function<void()> changed) {
  auto *editor = new NfsSettingsEditor{store, runtime, std::move(changed), gtk_grid_new(), gtk_label_new("")};
  editor->synchronizing = true;
  gtk_grid_set_row_spacing(GTK_GRID(editor->root), 8);
  gtk_grid_set_column_spacing(GTK_GRID(editor->root), 12);
  gtk_container_set_border_width(GTK_CONTAINER(editor->root), 12);
  int row = 0;
  for (auto &field : editor->fields) {
    field.owner = editor;
    const auto id = prefix + "_nfs_" + field.name;
    auto *label = gtk_label_new(setting_label(nfs_setting_key(field.name)).c_str());
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    const char *signal = "changed";
    const char *suffix = "_entry";
    if (field.kind == NfsFieldKind::boolean) {
      field.control = gtk_check_button_new_with_label(_("Enabled"));
      signal = "toggled"; suffix = "_check";
    } else if (field.kind == NfsFieldKind::version) {
      field.control = gtk_combo_box_text_new();
      gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(field.control), "2", "NFS v2 (TCP)");
      gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(field.control), "3", "NFS v3 (TCP)");
      gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(field.control), "4.0", "NFS v4.0 (TCP)");
      suffix = "_combo";
    } else if (field.kind == NfsFieldKind::list) {
      field.control = gtk_text_view_new();
      gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(field.control), GTK_WRAP_NONE);
      suffix = "_list";
    } else field.control = gtk_entry_new();
    assign_id(field.control, id + suffix);
    auto *signal_source = field.kind == NfsFieldKind::list
        ? G_OBJECT(gtk_text_view_get_buffer(GTK_TEXT_VIEW(field.control))) : G_OBJECT(field.control);
    g_signal_connect(signal_source, signal, G_CALLBACK(field_changed), &field);
    auto *container = field.control;
    if (field.kind == NfsFieldKind::list) {
      container = gtk_scrolled_window_new(nullptr, nullptr);
      gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(container), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
      gtk_widget_set_size_request(container, -1, 70);
      gtk_container_add(GTK_CONTAINER(container), field.control);
    }
    field.inherit = gtk_check_button_new_with_label(_("Inherit"));
    assign_id(field.inherit, id + "_inherit");
    g_signal_connect(field.inherit, "toggled", G_CALLBACK(field_changed), &field);
    gtk_widget_set_hexpand(container, TRUE);
    gtk_grid_attach(GTK_GRID(editor->root), label, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(editor->root), container, 1, row, 1, 1);
    gtk_grid_attach(GTK_GRID(editor->root), field.inherit, 2, row++, 1, 1);
  }
  assign_id(editor->error, prefix + "_nfs_error_label");
  gtk_label_set_line_wrap(GTK_LABEL(editor->error), TRUE);
  gtk_grid_attach(GTK_GRID(editor->root), editor->error, 0, row, 3, 1);
  if (runtime) {
    auto *note = gtk_label_new(_("Filename encoding changes apply after reconnecting."));
    gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
    gtk_grid_attach(GTK_GRID(editor->root), note, 0, row + 1, 3, 1);
  }
  sync_nfs_settings_editor(editor, false);
  return editor;
}

GtkWidget *nfs_settings_editor_root(NfsSettingsEditor *editor) { return editor->root; }
bool nfs_settings_editor_is_valid(const NfsSettingsEditor *editor) {
  return !editor || (nfs_connection_settings(*editor->store).validation_errors.empty() &&
      std::all_of(editor->fields.begin(), editor->fields.end(), [](const auto &field) { return field.valid; }));
}
void destroy_nfs_settings_editor(NfsSettingsEditor *editor) { delete editor; }

} // namespace elder_terms
