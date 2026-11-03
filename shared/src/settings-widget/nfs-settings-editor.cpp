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
  GtkWidget *mode = nullptr;
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
      {"require_reserved_port", NfsFieldKind::boolean},
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

static std::string choice_label(const NfsField &field, const SettingValue &value) {
  const auto text = value_text(value);
  return field.kind == NfsFieldKind::version ? "NFS v" + text + " (TCP)" : text;
}

static void update_validation(NfsSettingsEditor *editor) {
  const auto settings = nfs_connection_settings(*editor->store);
  auto errors = settings.validation_errors;
  for (auto &field : editor->fields) {
    const std::string_view name(field.name);
    const bool applicable = settings.version == "4.0" ?
        name != "export_path" && name != "known_exports" && name != "discover_exports" && name != "mount_port" :
        name != "namespace_path" && name != "known_namespace_paths" && name != "discover_namespace";
    const bool editable = applicable && (!editor->runtime || name == "filename_encoding");
    const bool inherited_list = field.kind == NfsFieldKind::list &&
        !setting_has_explicit_value(*editor->store, nfs_setting_key(field.name));
    gtk_widget_set_sensitive(field.control, editable && !inherited_list);
    if (field.mode) gtk_widget_set_sensitive(field.mode, editable);
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
    const bool explicit_value = setting_has_explicit_value(*editor->store, key);
    const auto source = setting_fallback_source(*editor->store, key);
    if (GTK_IS_ENTRY(field.control))
      gtk_entry_set_placeholder_text(GTK_ENTRY(field.control),
          inherited_setting_label(found->fallback_invalid_raw_value.value_or(value_text(found->fallback_value)), source).c_str());
    if (preserve_invalid && !field.valid) continue;
    const auto text = found->invalid_raw_value.value_or(value_text(found->value));
    if (field.kind == NfsFieldKind::boolean || field.kind == NfsFieldKind::version) {
      auto *combo = GTK_COMBO_BOX_TEXT(field.control);
      gtk_combo_box_text_remove_all(combo);
      gtk_combo_box_text_append(combo, "inherit", inherited_setting_label(
          found->fallback_invalid_raw_value.value_or(choice_label(field, found->fallback_value)), source).c_str());
      if (field.kind == NfsFieldKind::boolean) {
        gtk_combo_box_text_append(combo, "enabled", _("Enabled"));
        gtk_combo_box_text_append(combo, "disabled", _("Disabled"));
      } else {
        for (const auto *version : {"2", "3", "4.0"})
          gtk_combo_box_text_append(combo, version, choice_label(field, std::string(version)).c_str());
      }
      const auto selected = field.kind == NfsFieldKind::boolean
          ? (std::get<bool>(found->value) ? std::string("enabled") : std::string("disabled")) : text;
      if (found->invalid_raw_value)
        gtk_combo_box_text_append(combo, text.c_str(), (std::string(_("Invalid value:")) + " " + text).c_str());
      gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), !explicit_value ? "inherit" :
          found->invalid_raw_value ? text.c_str() : selected.c_str());
    } else if (field.kind == NfsFieldKind::list) {
      auto *combo = GTK_COMBO_BOX_TEXT(field.mode);
      gtk_combo_box_text_remove_all(combo);
      gtk_combo_box_text_append(combo, "inherit", inherited_setting_label("", source).c_str());
      gtk_combo_box_text_append(combo, "custom", _("Custom"));
      gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), explicit_value ? "custom" : "inherit");
      gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(field.control)), text.c_str(), -1);
    } else gtk_entry_set_text(GTK_ENTRY(field.control), explicit_value ? text.c_str() : "");
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
  if (field->kind == NfsFieldKind::boolean || field->kind == NfsFieldKind::version) {
    const auto *choice = gtk_combo_box_get_active_id(GTK_COMBO_BOX(field->control));
    if (!choice) return;
    if (std::string_view(choice) == "inherit") {
      clear_explicit_setting_value(editor->store, key);
      field->valid = true;
    } else field->valid = set_explicit_setting_value(editor->store, key,
        field->kind == NfsFieldKind::boolean ? SettingValue{std::string_view(choice) == "enabled"} : SettingValue{std::string(choice)});
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
    if (text.empty()) {
      clear_explicit_setting_value(editor->store, key);
      field->valid = true;
    } else if (field->kind == NfsFieldKind::integer) {
      gint64 value = 0;
      const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
      field->valid = parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() &&
          set_explicit_setting_value(editor->store, key, value);
    } else field->valid = set_explicit_setting_value(editor->store, key, text);
  }
  update_validation(editor);
  editor->changed();
}

static void list_mode_changed(GtkComboBox *combo, gpointer data) {
  auto *field = static_cast<NfsField *>(data);
  auto *editor = field->owner;
  if (editor->synchronizing || editor->runtime) return;
  const auto *choice = gtk_combo_box_get_active_id(combo);
  if (!choice) return;
  if (std::string_view(choice) == "inherit") {
    clear_explicit_setting_value(editor->store, nfs_setting_key(field->name));
    field->valid = true;
    sync_nfs_settings_editor(editor, true);
    editor->changed();
  } else field_changed(nullptr, data);
}

NfsSettingsEditor *create_nfs_settings_editor(SettingsStore *store, std::string prefix,
    bool runtime, std::function<void()> changed,
    std::function<GtkWidget *(GtkWidget *)> create_address_row) {
  auto *editor = new NfsSettingsEditor{store, runtime, std::move(changed), gtk_grid_new(), gtk_label_new("")};
  editor->synchronizing = true;
  gtk_grid_set_row_spacing(GTK_GRID(editor->root), 8);
  gtk_grid_set_column_spacing(GTK_GRID(editor->root), 16);
  gtk_container_set_border_width(GTK_CONTAINER(editor->root), 12);
  int row = 0;
  for (auto &field : editor->fields) {
    field.owner = editor;
    const auto id = prefix + "_nfs_" + field.name;
    auto *label = gtk_label_new(setting_label(nfs_setting_key(field.name)).c_str());
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    const char *signal = "changed";
    const char *suffix = "_entry";
    if (field.kind == NfsFieldKind::boolean || field.kind == NfsFieldKind::version) {
      field.control = gtk_combo_box_text_new();
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
      gtk_widget_set_valign(label, GTK_ALIGN_START);
      container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
      field.mode = gtk_combo_box_text_new();
      assign_id(field.mode, id + "_combo");
      g_signal_connect(field.mode, "changed", G_CALLBACK(list_mode_changed), &field);
      gtk_box_pack_start(GTK_BOX(container), field.mode, FALSE, FALSE, 0);
      auto *scroll = gtk_scrolled_window_new(nullptr, nullptr);
      gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
      gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
      gtk_widget_set_size_request(scroll, -1, 70);
      gtk_container_add(GTK_CONTAINER(scroll), field.control);
      gtk_box_pack_start(GTK_BOX(container), scroll, TRUE, TRUE, 0);
    } else if (std::string_view(field.name) == "address") container = create_address_row(field.control);
    gtk_widget_set_hexpand(container, TRUE);
    gtk_grid_attach(GTK_GRID(editor->root), label, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(editor->root), container, 1, row++, 1, 1);
  }
  assign_id(editor->error, prefix + "_nfs_error_label");
  gtk_label_set_line_wrap(GTK_LABEL(editor->error), TRUE);
  gtk_grid_attach(GTK_GRID(editor->root), editor->error, 0, row, 2, 1);
  if (runtime) {
    auto *note = gtk_label_new(_("Filename encoding changes apply after reconnecting."));
    gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
    gtk_grid_attach(GTK_GRID(editor->root), note, 0, row + 1, 2, 1);
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
