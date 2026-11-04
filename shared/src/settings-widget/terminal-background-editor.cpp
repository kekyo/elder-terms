// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "terminal-background-editor.h"
#include "settings-validation.h"
#include "settings-presentation.h"
#include <elder-terms/modal-color-button.h>
#include <elder-terms/modal-dialog.h>
#include <array>
#include <charconv>
#include <cstdio>

#define GETTEXT_PACKAGE "elder-terms"
#include <glib/gi18n-lib.h>

namespace elder_terms {

struct BackgroundField {
  const char *name;
  bool number;
  GtkWidget *entry = nullptr;
  bool valid = true;
};

struct TerminalBackgroundEditor {
  SettingsStore *store;
  std::string prefix;
  std::function<void()> changed;
  GtkWidget *root;
  GtkWidget *mode;
  GtkWidget *color;
  GtkWidget *status;
  GtkWidget *image_path = nullptr;
  GtkWidget *image_fit = nullptr;
  GtkWidget *image_dialog = nullptr;
  bool image_valid = true;
  std::string status_text = "";
  bool synchronizing = false;
  std::array<BackgroundField, 4> fields{{{"background_opacity", true},
      {"background_tint_color", false}, {"background_tint_opacity", true}, {"background_image_opacity", true}}};
};

static void identify(GtkWidget *widget, const std::string &id) {
  gtk_widget_set_name(widget, id.c_str());
  atk_object_set_accessible_id(gtk_widget_get_accessible(widget), id.c_str());
}

static std::string value_text(const SettingValue &value) {
  if (const auto *number = std::get_if<gint64>(&value)) return std::to_string(*number);
  return std::get<std::string>(value);
}

static void sync_color(TerminalBackgroundEditor *editor) {
  const auto rgb = terminal_background_settings(*editor->store).tint;
  const GdkRGBA color{rgb.red / 255.0, rgb.green / 255.0, rgb.blue / 255.0, 1};
  gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(editor->color), &color);
}

static void validate_fields(TerminalBackgroundEditor *editor) {
  for (const auto &field : editor->fields) {
    set_settings_input_error(field.entry, field.valid ? "" :
        field.number ? _("Enter an integer from 0 to 100") : _("Enter a color as #RRGGBB"));
  }
  set_settings_input_error(editor->image_path, editor->image_valid ? "" : _("Enter an absolute local image path"));
}

void sync_terminal_background_editor(TerminalBackgroundEditor *editor, bool preserve_invalid) {
  if (!editor) return;
  editor->synchronizing = true;
  const auto key = terminal_background_mode_setting_key();
  const auto fallback = value_text(setting_fallback_value(*editor->store, key, std::string("opaque")));
  const auto inherited = inherited_setting_label(setting_choice_label(key, fallback), setting_fallback_source(*editor->store, key));
  gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(editor->mode));
  gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(editor->mode), "inherit", inherited.c_str());
  for (const auto *mode : {"opaque", "transparent", "blur"})
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(editor->mode), mode, setting_choice_label(key, mode).c_str());
  const auto effective = setting_string_value_or_default(*editor->store, key, "opaque");
  gtk_combo_box_set_active_id(GTK_COMBO_BOX(editor->mode),
      setting_has_explicit_value(*editor->store, key) ? effective.c_str() : "inherit");
  for (auto &field : editor->fields) {
    const auto field_key = make_setting_key("terminal", field.name);
    const auto builtin = field.number ? SettingValue{gint64{0}} : SettingValue{std::string("#000000")};
    const auto placeholder = inherited_setting_label(value_text(setting_fallback_value(*editor->store, field_key, builtin)),
        setting_fallback_source(*editor->store, field_key));
    gtk_entry_set_placeholder_text(GTK_ENTRY(field.entry), placeholder.c_str());
    if (preserve_invalid && !field.valid) continue;
    gtk_entry_set_text(GTK_ENTRY(field.entry), setting_has_explicit_value(*editor->store, field_key)
        ? value_text(setting_value_or_default(*editor->store, field_key, builtin)).c_str() : "");
    field.valid = true;
  }
  const auto image_key = terminal_background_image_setting_key();
  const auto image_fallback = setting_string_value_or_default(*editor->store, image_key, "");
  const auto placeholder = setting_has_explicit_value(*editor->store, image_key) ? std::string(_("No image")) :
      inherited_setting_label(image_fallback.empty() ? _("No image") : image_fallback, setting_fallback_source(*editor->store, image_key));
  gtk_entry_set_placeholder_text(GTK_ENTRY(editor->image_path), placeholder.c_str());
  if (!preserve_invalid || editor->image_valid) {
    gtk_entry_set_text(GTK_ENTRY(editor->image_path), setting_has_explicit_value(*editor->store, image_key) ? image_fallback.c_str() : "");
    editor->image_valid = true;
  }
  const auto fit_key = terminal_background_image_fit_setting_key();
  const auto fit_fallback = value_text(setting_fallback_value(*editor->store, fit_key, std::string("cover")));
  const auto fit_label = inherited_setting_label(setting_choice_label(fit_key, fit_fallback), setting_fallback_source(*editor->store, fit_key));
  gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(editor->image_fit));
  gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(editor->image_fit), "inherit", fit_label.c_str());
  for (const auto *fit : {"cover", "contain", "stretch"})
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(editor->image_fit), fit, setting_choice_label(fit_key, fit).c_str());
  const auto fit = setting_string_value_or_default(*editor->store, fit_key, "cover");
  gtk_combo_box_set_active_id(GTK_COMBO_BOX(editor->image_fit),
      setting_has_explicit_value(*editor->store, fit_key) ? fit.c_str() : "inherit");
  sync_color(editor);
  validate_fields(editor);
  editor->synchronizing = false;
}

static void entry_changed(GtkEditable *widget, gpointer data) {
  auto *editor = static_cast<TerminalBackgroundEditor *>(data);
  if (editor->synchronizing) return;
  for (auto &field : editor->fields) {
    if (field.entry != GTK_WIDGET(widget)) continue;
    const auto key = make_setting_key("terminal", field.name);
    const std::string text = gtk_entry_get_text(GTK_ENTRY(widget));
    if (text.empty()) {
      clear_explicit_setting_value(editor->store, key);
      field.valid = true;
    } else if (field.number) {
      gint64 number = 0;
      const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
      field.valid = parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() &&
          set_explicit_setting_value(editor->store, key, number);
    } else field.valid = set_explicit_setting_value(editor->store, key, text);
  }
  sync_color(editor);
  validate_fields(editor);
  editor->changed();
}

static void choose_image(GtkButton *, gpointer data) {
  auto *editor = static_cast<TerminalBackgroundEditor *>(data);
  if (editor->image_dialog) { present_modal_dialog(editor->image_dialog); return; }
  auto *top = gtk_widget_get_toplevel(editor->root);
  auto *parent = GTK_IS_WINDOW(top) ? GTK_WINDOW(top) : nullptr;
  auto *dialog = gtk_file_chooser_dialog_new(_("Select a background image"), parent, GTK_FILE_CHOOSER_ACTION_OPEN,
      _("Cancel"), GTK_RESPONSE_CANCEL, _("Open"), GTK_RESPONSE_ACCEPT, nullptr);
  editor->image_dialog = dialog;
  identify(dialog, editor->prefix + "_terminal_background_image_dialog");
  gtk_file_chooser_set_local_only(GTK_FILE_CHOOSER(dialog), TRUE);
  gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(dialog), FALSE);
  auto *filter = gtk_file_filter_new();
  gtk_file_filter_set_name(filter, _("PNG and JPEG images"));
  gtk_file_filter_add_mime_type(filter, "image/png");
  gtk_file_filter_add_mime_type(filter, "image/jpeg");
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);
  const auto current = terminal_background_image_settings(*editor->store).path;
  if (auto *filename = g_filename_from_utf8(current.c_str(), -1, nullptr, nullptr, nullptr)) {
    if (*filename) gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dialog), filename);
    g_free(filename);
  }
  g_signal_connect(dialog, "destroy", G_CALLBACK(+[](GtkWidget *, gpointer data) {
    static_cast<TerminalBackgroundEditor *>(data)->image_dialog = nullptr;
  }), editor);
  g_signal_connect(dialog, "response", G_CALLBACK(+[](GtkDialog *dialog, gint response, gpointer data) {
    auto *editor = static_cast<TerminalBackgroundEditor *>(data);
    if (response == GTK_RESPONSE_ACCEPT) {
      auto *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
      if (filename) {
        auto *text = g_filename_to_utf8(filename, -1, nullptr, nullptr, nullptr);
        if (text) gtk_entry_set_text(GTK_ENTRY(editor->image_path), text);
        g_free(text);
        g_free(filename);
      }
    }
    gtk_widget_destroy(GTK_WIDGET(dialog));
  }), editor);
  show_modal_dialog(dialog, parent);
}

TerminalBackgroundEditor *create_terminal_background_editor(SettingsStore *store, std::string prefix,
    GtkSizeGroup *labels, std::function<void()> changed) {
  auto *editor = new TerminalBackgroundEditor{store, std::move(prefix), std::move(changed),
      gtk_grid_new(), gtk_combo_box_text_new(), create_modal_color_button(), gtk_label_new(nullptr)};
  gtk_grid_set_row_spacing(GTK_GRID(editor->root), 8);
  gtk_grid_set_column_spacing(GTK_GRID(editor->root), 16);
  const auto row = [&](const SettingKey &key, GtkWidget *widget, int index) {
    auto *label = gtk_label_new(setting_label(key).c_str());
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_size_group_add_widget(labels, label);
    gtk_label_set_mnemonic_widget(GTK_LABEL(label), widget);
    gtk_widget_set_hexpand(widget, TRUE);
    gtk_grid_attach(GTK_GRID(editor->root), label, 0, index, 1, 1);
    gtk_grid_attach(GTK_GRID(editor->root), widget, 1, index, 1, 1);
  };
  identify(editor->mode, editor->prefix + "_terminal_background_mode_combo");
  row(terminal_background_mode_setting_key(), editor->mode, 0);
  g_signal_connect(editor->mode, "changed", G_CALLBACK(+[](GtkComboBox *combo, gpointer data) {
    auto *editor = static_cast<TerminalBackgroundEditor *>(data);
    if (editor->synchronizing) return;
    const auto *id = gtk_combo_box_get_active_id(combo);
    if (!id) return;
    const auto key = terminal_background_mode_setting_key();
    if (std::string_view(id) == "inherit") clear_explicit_setting_value(editor->store, key);
    else set_explicit_setting_value(editor->store, key, std::string(id));
    editor->changed();
  }), editor);
  int index = 1;
  auto &image_opacity = editor->fields.back();
  for (auto &field : editor->fields) {
    field.entry = gtk_entry_new();
    identify(field.entry, editor->prefix + "_terminal_" + field.name + "_entry");
    g_signal_connect(field.entry, "changed", G_CALLBACK(entry_changed), editor);
    GtkWidget *control = field.entry;
    if (!field.number) {
      control = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
      gtk_box_pack_start(GTK_BOX(control), field.entry, TRUE, TRUE, 0);
      gtk_box_pack_start(GTK_BOX(control), editor->color, FALSE, FALSE, 0);
    }
    if (&field != &image_opacity) row(make_setting_key("terminal", field.name), control, index++);
  }
  identify(editor->color, editor->prefix + "_terminal_background_tint_color_button");
  g_signal_connect(editor->color, "color-set", G_CALLBACK(+[](GtkColorButton *button, gpointer data) {
    auto *editor = static_cast<TerminalBackgroundEditor *>(data);
    GdkRGBA color{};
    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(button), &color);
    char text[8];
    std::snprintf(text, sizeof(text), "#%02x%02x%02x", static_cast<unsigned>(color.red * 255 + 0.5),
        static_cast<unsigned>(color.green * 255 + 0.5), static_cast<unsigned>(color.blue * 255 + 0.5));
    gtk_entry_set_text(GTK_ENTRY(editor->fields[1].entry), text);
  }), editor);
  editor->image_path = gtk_entry_new();
  identify(editor->image_path, editor->prefix + "_terminal_background_image_entry");
  row(terminal_background_image_setting_key(), editor->image_path, index++);
  g_signal_connect(editor->image_path, "changed", G_CALLBACK(+[](GtkEditable *entry, gpointer data) {
    auto *editor = static_cast<TerminalBackgroundEditor *>(data);
    if (editor->synchronizing) return;
    editor->image_valid = set_explicit_setting_value(editor->store, terminal_background_image_setting_key(),
        std::string(gtk_entry_get_text(GTK_ENTRY(entry))));
    validate_fields(editor);
    editor->changed();
  }), editor);
  auto *image_buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  auto *choose = gtk_button_new_with_label(_("Select file"));
  auto *clear = gtk_button_new_with_label(_("Clear image"));
  auto *inherit = gtk_button_new_with_label(_("Inherit"));
  identify(choose, editor->prefix + "_terminal_background_image_browse");
  identify(clear, editor->prefix + "_terminal_background_image_clear");
  identify(inherit, editor->prefix + "_terminal_background_image_inherit");
  for (auto *button : {choose, clear, inherit}) gtk_box_pack_start(GTK_BOX(image_buttons), button, FALSE, FALSE, 0);
  gtk_grid_attach(GTK_GRID(editor->root), image_buttons, 1, index++, 1, 1);
  g_signal_connect(choose, "clicked", G_CALLBACK(choose_image), editor);
  g_signal_connect(clear, "clicked", G_CALLBACK(+[](GtkButton *, gpointer data) {
    auto *editor = static_cast<TerminalBackgroundEditor *>(data);
    set_explicit_setting_value(editor->store, terminal_background_image_setting_key(), std::string());
    editor->image_valid = true;
    sync_terminal_background_editor(editor, true);
    editor->changed();
  }), editor);
  g_signal_connect(inherit, "clicked", G_CALLBACK(+[](GtkButton *, gpointer data) {
    auto *editor = static_cast<TerminalBackgroundEditor *>(data);
    clear_explicit_setting_value(editor->store, terminal_background_image_setting_key());
    editor->image_valid = true;
    sync_terminal_background_editor(editor, true);
    editor->changed();
  }), editor);
  row(terminal_background_image_opacity_setting_key(), image_opacity.entry, index++);
  editor->image_fit = gtk_combo_box_text_new();
  identify(editor->image_fit, editor->prefix + "_terminal_background_image_fit_combo");
  row(terminal_background_image_fit_setting_key(), editor->image_fit, index++);
  g_signal_connect(editor->image_fit, "changed", G_CALLBACK(+[](GtkComboBox *combo, gpointer data) {
    auto *editor = static_cast<TerminalBackgroundEditor *>(data);
    if (editor->synchronizing) return;
    const auto *id = gtk_combo_box_get_active_id(combo);
    if (!id) return;
    if (std::string_view(id) == "inherit") clear_explicit_setting_value(editor->store, terminal_background_image_fit_setting_key());
    else set_explicit_setting_value(editor->store, terminal_background_image_fit_setting_key(), std::string(id));
    editor->changed();
  }), editor);
  identify(editor->status, editor->prefix + "_terminal_background_status");
  gtk_label_set_line_wrap(GTK_LABEL(editor->status), TRUE);
  gtk_label_set_xalign(GTK_LABEL(editor->status), 0);
  gtk_widget_set_no_show_all(editor->status, TRUE);
  gtk_grid_attach(GTK_GRID(editor->root), editor->status, 0, index, 2, 1);
  sync_terminal_background_editor(editor, false);
  return editor;
}

GtkWidget *terminal_background_editor_root(TerminalBackgroundEditor *editor) { return editor->root; }
bool terminal_background_editor_is_valid(const TerminalBackgroundEditor *editor) {
  if (!editor) return true;
  if (!editor->image_valid) return false;
  for (const auto &field : editor->fields) if (!field.valid) return false;
  return true;
}
void terminal_background_editor_set_status(TerminalBackgroundEditor *editor, const std::string &text) {
  if (!editor || editor->status_text == text) return;
  editor->status_text = text;
  gtk_label_set_text(GTK_LABEL(editor->status), text.c_str());
  gtk_widget_set_visible(editor->status, !text.empty());
}
void destroy_terminal_background_editor(TerminalBackgroundEditor *editor) {
  if (!editor) return;
  if (editor->image_dialog) gtk_widget_destroy(editor->image_dialog);
  delete editor;
}
}
