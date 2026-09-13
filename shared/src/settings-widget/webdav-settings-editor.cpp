#include "webdav-settings-editor.h"
#include "settings-presentation.h"
#include <elder-terms/modal-dialog.h>

#include <algorithm>
#include <charconv>
#include <memory>
#define GETTEXT_PACKAGE "elder-terms"
#include <glib/gi18n-lib.h>

namespace elder_terms {

static void assign_accessible_id(GtkWidget *widget, const std::string &id) {
  gtk_widget_set_name(widget, id.c_str());
  atk_object_set_accessible_id(gtk_widget_get_accessible(widget), id.c_str());
}

struct WebdavField {
  const char *name;
  std::vector<const char *> choices;
  GtkWidget *entry = nullptr;
  GtkWidget *combo = nullptr;
  bool valid = true;
};

struct WebdavSettingsEditor {
  SettingsStore *store;
  std::string prefix;
  bool read_only;
  std::function<void()> changed;
  GtkWidget *root;
  GtkWidget *error;
  GtkWidget *preview = nullptr;
  GtkWidget *browse = nullptr;
  GtkWidget *dialog = nullptr;
  bool synchronizing = false;
  std::vector<WebdavField> fields{
      {"scheme", {"https", "http"}}, {"address", {}}, {"port", {}},
      {"base_path", {}}, {"authentication", {"auto", "basic", "digest", "none"}},
      {"username", {}}, {"local_directory", {}}, {"remote_directory", {}},
      {"ca_file", {"default", "custom"}},
      {"certificate_error_action", {"reject", "prompt"}},
      {"connect_timeout_seconds", {}}, {"idle_timeout_seconds", {}}};
};

static bool numeric_field(const std::string_view name) {
  return name == "port" || name.ends_with("_timeout_seconds");
}

static std::string effective_text(const SettingsStore &store, const char *name) {
  for (const auto &entry : store.entries)
    if (entry.definition.key.section == "webdav" && entry.definition.key.name == name && entry.invalid_raw_value)
      return *entry.invalid_raw_value;
  if (std::string_view(name) == "port") return std::to_string(webdav_connection_settings(store).port);
  if (numeric_field(name)) return std::to_string(setting_integer_value_or_default(store, webdav_setting_key(name), 0));
  return setting_string_value_or_default(store, webdav_setting_key(name), "");
}

static std::string connection_url_preview(const WebdavConnectionSettings &settings) {
  if (settings.address.empty() || !settings.validation_errors.empty()) return {};
  auto host = settings.address;
  if (host.find(':') != std::string::npos && !host.starts_with('[')) host = "[" + host + "]";
  const auto base = std::unique_ptr<gchar, decltype(&g_free)>(
      g_uri_unescape_string(settings.base_path.c_str(), "/\\"), g_free);
  if (!base) return {};
  std::string path(base.get());
  if (path.ends_with('/')) path.pop_back();
  path += settings.remote_directory;
  if (settings.remote_directory.size() > 1 && path.ends_with('/')) path.pop_back();
  const auto encoded = std::unique_ptr<gchar, decltype(&g_free)>(
      g_uri_escape_string(path.c_str(), "/", FALSE), g_free);
  if (!encoded) return {};
  const auto url = settings.scheme + "://" + host + ":" + std::to_string(settings.port) + encoded.get();
  const auto uri = std::unique_ptr<GUri, decltype(&g_uri_unref)>(
      g_uri_parse(url.c_str(), G_URI_FLAGS_ENCODED, nullptr), g_uri_unref);
  if (!uri || !g_uri_get_host(uri.get()) || std::string_view(g_uri_get_host(uri.get())).find('%') != std::string_view::npos)
    return {};
  return url;
}

static void update_validation(WebdavSettingsEditor *editor) {
  const auto settings = webdav_connection_settings(*editor->store);
  auto errors = settings.validation_errors;
  for (auto &field : editor->fields) {
    const std::string_view name(field.name);
    const bool tls = name == "ca_file" || name == "certificate_error_action";
    const bool enabled = !editor->read_only && (!tls || settings.scheme == "https") &&
        (name != "username" || settings.authentication != WebdavAuthentication::none);
    if (field.combo) gtk_widget_set_sensitive(field.combo, enabled);
    if (field.entry) {
      bool custom = true;
      if (name == "ca_file") {
        const auto *choice = gtk_combo_box_get_active_id(GTK_COMBO_BOX(field.combo));
        custom = choice && std::string_view(choice) == "custom";
        gtk_widget_set_sensitive(editor->browse, enabled && custom);
      }
      gtk_widget_set_sensitive(field.entry, enabled && custom);
      gtk_entry_set_icon_from_icon_name(GTK_ENTRY(field.entry), GTK_ENTRY_ICON_SECONDARY,
                                        field.valid ? nullptr : "dialog-error-symbolic");
    }
    if (!field.valid) errors.push_back(setting_label(webdav_setting_key(field.name)) + ": " + _("Enter a valid value"));
  }
  gtk_label_set_text(GTK_LABEL(editor->error), errors.empty() ? "" : errors.front().c_str());
  const auto url = errors.empty() ? connection_url_preview(settings) : std::string();
  gtk_label_set_text(GTK_LABEL(editor->preview), url.empty()
      ? _("Complete the connection settings to preview the URL") : url.c_str());
}

void sync_webdav_settings_editor(WebdavSettingsEditor *editor, bool preserve_invalid) {
  if (!editor) return;
  editor->synchronizing = true;
  for (auto &field : editor->fields) {
    const bool unfinished = preserve_invalid && !field.valid && field.entry;
    const std::string unfinished_text = unfinished ? gtk_entry_get_text(GTK_ENTRY(field.entry)) : "";
    const auto key = webdav_setting_key(field.name);
    const auto effective = effective_text(*editor->store, field.name);
    auto fallback = *editor->store;
    clear_explicit_setting_value(&fallback, key);
    const auto inherited = effective_text(fallback, field.name);
    const bool explicit_value = setting_has_explicit_value(*editor->store, key);
    if (field.combo) {
      gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(field.combo));
      const auto inherited_label = inherited_setting_label(
          setting_choice_label(key, inherited), setting_fallback_source(*editor->store, key));
      gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(field.combo), "inherit", inherited_label.c_str());
      for (const auto *choice : field.choices)
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(field.combo), choice, setting_choice_label(key, choice).c_str());
      const auto selected = std::string_view(field.name) == "ca_file" ? (effective.empty() ? "default" : "custom") : effective;
      if (std::none_of(field.choices.begin(), field.choices.end(), [&](const char *choice) { return selected == choice; }))
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(field.combo), selected.c_str(), (std::string(_("Invalid value:")) + " " + selected).c_str());
      gtk_combo_box_set_active_id(GTK_COMBO_BOX(field.combo), explicit_value ? selected.c_str() : "inherit");
    }
    if (field.entry) {
      gtk_entry_set_text(GTK_ENTRY(field.entry), explicit_value ? effective.c_str() : "");
      gtk_entry_set_placeholder_text(GTK_ENTRY(field.entry), inherited_setting_label(inherited, setting_fallback_source(*editor->store, key)).c_str());
    }
    field.valid = !unfinished;
    if (unfinished) {
      gtk_entry_set_text(GTK_ENTRY(field.entry), unfinished_text.c_str());
      if (field.combo) gtk_combo_box_set_active_id(GTK_COMBO_BOX(field.combo), "custom");
    }
  }
  update_validation(editor);
  editor->synchronizing = false;
}

static void entry_changed(GtkEditable *widget, gpointer data) {
  auto *editor = static_cast<WebdavSettingsEditor *>(data);
  if (editor->synchronizing || editor->read_only) return;
  for (auto &field : editor->fields) {
    if (field.entry != GTK_WIDGET(widget)) continue;
    const auto key = webdav_setting_key(field.name);
    const std::string text = gtk_entry_get_text(GTK_ENTRY(widget));
    if (text.empty() && std::string_view(field.name) != "ca_file") {
      clear_explicit_setting_value(editor->store, key);
      field.valid = true;
    } else if (numeric_field(field.name)) {
      gint64 number = 0;
      const auto result = std::from_chars(text.data(), text.data() + text.size(), number);
      field.valid = result.ec == std::errc{} && result.ptr == text.data() + text.size() &&
          set_explicit_setting_value(editor->store, key, number);
    } else field.valid = set_explicit_setting_value(editor->store, key, text) &&
        (std::string_view(field.name) != "ca_file" || !text.empty());
  }
  update_validation(editor);
  editor->changed();
}

static void combo_changed(GtkComboBox *widget, gpointer data) {
  auto *editor = static_cast<WebdavSettingsEditor *>(data);
  if (editor->synchronizing || editor->read_only) return;
  for (auto &field : editor->fields) {
    if (field.combo != GTK_WIDGET(widget)) continue;
    const auto *active = gtk_combo_box_get_active_id(widget);
    if (!active) return;
    const std::string choice(active);
    const auto key = webdav_setting_key(field.name);
    field.valid = true;
    if (choice == "inherit") {
      clear_explicit_setting_value(editor->store, key);
      if (field.entry) {
        editor->synchronizing = true;
        gtk_entry_set_text(GTK_ENTRY(field.entry), "");
        editor->synchronizing = false;
      }
    }
    else if (std::string_view(field.name) == "ca_file") {
      const std::string path = choice == "default" ? "" : gtk_entry_get_text(GTK_ENTRY(field.entry));
      field.valid = set_explicit_setting_value(editor->store, key, path) && (choice != "custom" || !path.empty());
      if (choice == "default") {
        editor->synchronizing = true;
        gtk_entry_set_text(GTK_ENTRY(field.entry), "");
        editor->synchronizing = false;
      }
    } else field.valid = set_explicit_setting_value(editor->store, key, choice);
    if (std::string_view(field.name) == "scheme") {
      for (const auto &port : editor->fields) if (std::string_view(port.name) == "port") {
        auto fallback = *editor->store;
        clear_explicit_setting_value(&fallback, webdav_setting_key("port"));
        gtk_entry_set_placeholder_text(GTK_ENTRY(port.entry), inherited_setting_label(effective_text(fallback, "port"), setting_fallback_source(fallback, webdav_setting_key("port"))).c_str());
      }
    }
  }
  update_validation(editor);
  editor->changed();
}

static void browse_response(GtkDialog *dialog, gint response, gpointer data) {
  auto *editor = static_cast<WebdavSettingsEditor *>(data);
  if (response == GTK_RESPONSE_ACCEPT) {
    auto *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
    auto *text = path ? g_filename_to_utf8(path, -1, nullptr, nullptr, nullptr) : nullptr;
    if (text) for (const auto &field : editor->fields) if (std::string_view(field.name) == "ca_file")
      gtk_entry_set_text(GTK_ENTRY(field.entry), text);
    g_free(text);
    g_free(path);
  }
  gtk_widget_destroy(GTK_WIDGET(dialog));
}

static void browse_clicked(GtkButton *, gpointer data) {
  auto *editor = static_cast<WebdavSettingsEditor *>(data);
  if (editor->dialog) { present_modal_dialog(editor->dialog); return; }
  auto *top = gtk_widget_get_toplevel(editor->root);
  editor->dialog = gtk_file_chooser_dialog_new(_("Select a PEM CA bundle"), GTK_IS_WINDOW(top) ? GTK_WINDOW(top) : nullptr,
      GTK_FILE_CHOOSER_ACTION_OPEN, _("Cancel"), GTK_RESPONSE_CANCEL, _("Open"), GTK_RESPONSE_ACCEPT, nullptr);
  assign_accessible_id(editor->dialog, editor->prefix + "_webdav_ca_dialog");
  g_signal_connect(editor->dialog, "response", G_CALLBACK(browse_response), editor);
  g_signal_connect(editor->dialog, "destroy", G_CALLBACK(+[](GtkWidget *, gpointer data) {
    static_cast<WebdavSettingsEditor *>(data)->dialog = nullptr;
  }), editor);
  show_modal_dialog(editor->dialog, GTK_IS_WINDOW(top) ? GTK_WINDOW(top) : nullptr);
}

WebdavSettingsEditor *create_webdav_settings_editor(SettingsStore *store,
    std::string prefix, bool read_only, std::function<void()> changed) {
  auto *editor = new WebdavSettingsEditor{store, std::move(prefix), read_only, std::move(changed), gtk_grid_new(), gtk_label_new("")};
  gtk_grid_set_row_spacing(GTK_GRID(editor->root), 8);
  gtk_grid_set_column_spacing(GTK_GRID(editor->root), 12);
  gtk_container_set_border_width(GTK_CONTAINER(editor->root), 12);
  int row = 0;
  for (auto &field : editor->fields) {
    const auto id = editor->prefix + "_webdav_" + field.name;
    auto *label = gtk_label_new(setting_label(webdav_setting_key(field.name)).c_str());
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    auto *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    if (!field.choices.empty()) {
      field.combo = gtk_combo_box_text_new();
      assign_accessible_id(field.combo, id + "_combo");
      gtk_box_pack_start(GTK_BOX(box), field.combo, FALSE, FALSE, 0);
      g_signal_connect(field.combo, "changed", G_CALLBACK(combo_changed), editor);
    }
    if (field.choices.empty() || std::string_view(field.name) == "ca_file") {
      field.entry = gtk_entry_new();
      assign_accessible_id(field.entry, id + "_entry");
      gtk_box_pack_start(GTK_BOX(box), field.entry, FALSE, FALSE, 0);
      g_signal_connect(field.entry, "changed", G_CALLBACK(entry_changed), editor);
    }
    if (std::string_view(field.name) == "ca_file") {
      gtk_widget_set_valign(label, GTK_ALIGN_START);
      editor->browse = gtk_button_new_with_label(_("Browse…"));
      assign_accessible_id(editor->browse, editor->prefix + "_webdav_ca_browse_button");
      gtk_box_pack_start(GTK_BOX(box), editor->browse, FALSE, FALSE, 0);
      g_signal_connect(editor->browse, "clicked", G_CALLBACK(browse_clicked), editor);
    }
    gtk_widget_set_hexpand(box, TRUE);
    gtk_grid_attach(GTK_GRID(editor->root), label, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(editor->root), box, 1, row++, 1, 1);
    if (std::string_view(field.name) == "remote_directory") {
      auto *title = gtk_label_new(_("Connection URL"));
      gtk_label_set_xalign(GTK_LABEL(title), 0);
      editor->preview = gtk_label_new("");
      assign_accessible_id(editor->preview, editor->prefix + "_webdav_url_label");
      gtk_label_set_xalign(GTK_LABEL(editor->preview), 0);
      gtk_label_set_selectable(GTK_LABEL(editor->preview), TRUE);
      gtk_label_set_line_wrap(GTK_LABEL(editor->preview), TRUE);
      gtk_label_set_line_wrap_mode(GTK_LABEL(editor->preview), PANGO_WRAP_WORD_CHAR);
      gtk_label_set_max_width_chars(GTK_LABEL(editor->preview), 48);
      gtk_widget_set_hexpand(editor->preview, TRUE);
      gtk_grid_attach(GTK_GRID(editor->root), title, 0, row, 1, 1);
      gtk_grid_attach(GTK_GRID(editor->root), editor->preview, 1, row++, 1, 1);
    }
  }
  assign_accessible_id(editor->error, editor->prefix + "_webdav_error_label");
  gtk_label_set_line_wrap(GTK_LABEL(editor->error), TRUE);
  gtk_grid_attach(GTK_GRID(editor->root), editor->error, 0, row, 2, 1);
  sync_webdav_settings_editor(editor, false);
  return editor;
}

GtkWidget *webdav_settings_editor_root(WebdavSettingsEditor *editor) { return editor->root; }
bool webdav_settings_editor_is_valid(const WebdavSettingsEditor *editor) {
  return !editor || (webdav_connection_settings(*editor->store).validation_errors.empty() &&
      std::all_of(editor->fields.begin(), editor->fields.end(), [](const auto &field) { return field.valid; }));
}
void destroy_webdav_settings_editor(WebdavSettingsEditor *editor) {
  if (editor && editor->dialog) gtk_widget_destroy(editor->dialog);
  delete editor;
}

} // namespace elder_terms
