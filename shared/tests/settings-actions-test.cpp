// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include <elder-terms/settings-widget.h>
#include <cassert>
#include <cstring>
#include <iostream>

static GtkWidget *find(GtkWidget *widget, const char *id) {
  if (std::strcmp(gtk_widget_get_name(widget), id) == 0) return widget;
  if (!GTK_IS_CONTAINER(widget)) return nullptr;
  auto *children = gtk_container_get_children(GTK_CONTAINER(widget));
  GtkWidget *result = nullptr;
  for (auto *item = children; item && !result; item = item->next) result = find(GTK_WIDGET(item->data), id);
  g_list_free(children);
  return result;
}

int main(int argc, char **argv) {
  using namespace elder_terms;
  gtk_init(&argc, &argv);
  SettingsWidgetOptions options;
  options.store = create_default_settings(default_terminal_display_settings(12), "Action test");
  options.is_runtime = true;
  SettingsWidgetAction pending;
  bool asynchronous = false;
  options.callbacks.apply = [&](const SettingsStore &, SettingsWidgetAction action) {
    if (asynchronous) pending = std::move(action);
    else action.complete(false, "Rejected for testing");
  };
  options.callbacks.save = [](const SettingsStore &, SettingsWidgetAction action) {
    action.complete(false, "Save failed for testing");
  };
  auto *state = create_settings_widget(std::move(options));
  auto *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  auto *root = settings_widget_root(state);
  gtk_container_add(GTK_CONTAINER(window), root);
  g_signal_connect(window, "destroy", G_CALLBACK(+[](GtkWidget *, gpointer data) {
    destroy_settings_widget(static_cast<SettingsWidgetState *>(data));
  }), state);
  auto *opacity = find(root, "settings_terminal_background_opacity_entry");
  gtk_entry_set_text(GTK_ENTRY(opacity), "101");
  assert(!gtk_widget_get_sensitive(find(root, "settings_apply_button")));
  gtk_entry_set_text(GTK_ENTRY(opacity), "0");
  assert(gtk_widget_get_sensitive(find(root, "settings_apply_button")));
  auto *tint = find(root, "settings_terminal_background_tint_color_entry");
  gtk_entry_set_text(GTK_ENTRY(tint), "blue");
  assert(!gtk_widget_get_sensitive(find(root, "settings_save_button")));
  gtk_entry_set_text(GTK_ENTRY(tint), "#0000ff");
  assert(gtk_widget_get_sensitive(find(root, "settings_save_button")));
  gtk_button_clicked(GTK_BUTTON(find(root, "settings_cancel_button")));
  assert(terminal_background_settings(settings_widget_draft_store(state)).opacity == 80);
  auto *shape = find(root, "settings_terminal_cursor_shape_combo");
  auto *trim = find(root, "settings_terminal_block_copy_trim_trailing_spaces_combo");
  auto *width = find(root, "settings_terminal_ambiguous_width_combo");
  assert(width != nullptr);
  const auto width_key = make_setting_key("terminal", "ambiguous_width");
  assert(gtk_combo_box_set_active_id(GTK_COMBO_BOX(width), "font"));
  assert(setting_has_explicit_value(settings_widget_draft_store(state), width_key));
  assert(setting_string_value_or_default(settings_widget_draft_store(state), width_key, "") == "font");
  assert(gtk_combo_box_set_active_id(GTK_COMBO_BOX(width), "wide"));
  assert(setting_has_explicit_value(settings_widget_draft_store(state), width_key));
  assert(setting_string_value_or_default(settings_widget_draft_store(state), width_key, "") == "wide");
  assert(gtk_combo_box_set_active_id(GTK_COMBO_BOX(width), "narrow"));
  assert(setting_string_value_or_default(settings_widget_draft_store(state), width_key, "") == "narrow");
  assert(gtk_combo_box_set_active_id(GTK_COMBO_BOX(width), "inherit"));
  assert(!setting_has_explicit_value(settings_widget_draft_store(state), width_key));
  assert(setting_string_value_or_default(settings_widget_draft_store(state), width_key, "") == "font");
  assert(terminal_block_copy_trim_trailing_spaces(settings_widget_draft_store(state)));
  gtk_combo_box_set_active_id(GTK_COMBO_BOX(trim), "disabled");
  assert(setting_has_explicit_value(
      settings_widget_draft_store(state),
      terminal_block_copy_trim_trailing_spaces_setting_key()));
  assert(!terminal_block_copy_trim_trailing_spaces(
      settings_widget_draft_store(state)));
  gtk_combo_box_set_active_id(GTK_COMBO_BOX(trim), "inherit");
  assert(!setting_has_explicit_value(
      settings_widget_draft_store(state),
      terminal_block_copy_trim_trailing_spaces_setting_key()));
  auto *image_path = find(root, "settings_terminal_background_image_entry");
  gtk_entry_set_text(GTK_ENTRY(image_path), "relative.png");
  assert(!gtk_widget_get_sensitive(find(root, "settings_apply_button")));
  gtk_entry_set_text(GTK_ENTRY(image_path), "/tmp/image.png");
  assert(gtk_widget_get_sensitive(find(root, "settings_apply_button")));
  gtk_button_clicked(GTK_BUTTON(find(root, "settings_terminal_background_image_clear")));
  assert(setting_has_explicit_value(settings_widget_draft_store(state), terminal_background_image_setting_key()));
  assert(terminal_background_image_settings(settings_widget_draft_store(state)).path.empty());
  gtk_button_clicked(GTK_BUTTON(find(root, "settings_terminal_background_image_inherit")));
  assert(!setting_has_explicit_value(settings_widget_draft_store(state), terminal_background_image_setting_key()));
  gtk_combo_box_set_active_id(GTK_COMBO_BOX(shape), "bar");
  gtk_button_clicked(GTK_BUTTON(find(root, "settings_apply_button")));
  assert(settings_widget_is_dirty(state));
  assert(std::string(gtk_label_get_text(GTK_LABEL(find(root, "settings_action_message")))) == "Rejected for testing");
  assert(gtk_widget_get_sensitive(find(root, "settings_apply_button")));
  gtk_button_clicked(GTK_BUTTON(find(root, "settings_cancel_button")));
  assert(terminal_cursor_settings(settings_widget_draft_store(state)).shape == TerminalCursorShape::block);
  assert(terminal_block_copy_trim_trailing_spaces(settings_widget_draft_store(state)));
  asynchronous = true;
  gtk_combo_box_set_active_id(GTK_COMBO_BOX(shape), "bar");
  gtk_button_clicked(GTK_BUTTON(find(root, "settings_apply_button")));
  assert(pending.current());
  assert(!gtk_widget_get_sensitive(find(root, "settings_apply_button")));
  assert(!gtk_widget_get_sensitive(find(root, "settings_save_button")));
  assert(gtk_widget_get_sensitive(find(root, "settings_cancel_button")));
  // Editing while preparation is pending invalidates the old snapshot.
  const auto stale = pending;
  gtk_combo_box_set_active_id(GTK_COMBO_BOX(shape), "underline");
  assert(!stale.current());
  stale.complete(true, {});
  gtk_button_clicked(GTK_BUTTON(find(root, "settings_apply_button")));
  assert(pending.current());
  pending.complete(true, {});
  assert(!pending.current());
  gtk_combo_box_set_active_id(GTK_COMBO_BOX(shape), "bar");
  gtk_button_clicked(GTK_BUTTON(find(root, "settings_save_button")));
  gtk_button_clicked(GTK_BUTTON(find(root, "settings_cancel_button")));
  assert(terminal_cursor_settings(settings_widget_draft_store(state)).shape == TerminalCursorShape::underline);
  gtk_combo_box_set_active_id(GTK_COMBO_BOX(shape), "bar");
  gtk_button_clicked(GTK_BUTTON(find(root, "settings_apply_button")));
  gtk_button_clicked(GTK_BUTTON(find(root, "settings_cancel_button")));
  assert(!pending.current());
  pending.complete(true, {});
  assert(terminal_cursor_settings(settings_widget_draft_store(state)).shape == TerminalCursorShape::underline);
  gtk_combo_box_set_active_id(GTK_COMBO_BOX(shape), "bar");
  gtk_button_clicked(GTK_BUTTON(find(root, "settings_apply_button")));
  gtk_button_clicked(GTK_BUTTON(find(root, "settings_terminal_background_image_browse")));
  auto *toplevels = gtk_window_list_toplevels();
  GtkWidget *chooser = nullptr;
  for (auto *item = toplevels; item; item = item->next) {
    auto *candidate = GTK_WIDGET(item->data);
    if (std::strcmp(gtk_widget_get_name(candidate), "settings_terminal_background_image_dialog") == 0) chooser = candidate;
  }
  g_list_free(toplevels);
  assert(chooser && gtk_widget_get_visible(chooser) && GTK_IS_FILE_CHOOSER(chooser));
  assert(gtk_file_chooser_get_local_only(GTK_FILE_CHOOSER(chooser)));
  bool chooser_destroyed = false;
  g_signal_connect(chooser, "destroy", G_CALLBACK(+[](GtkWidget *, gpointer data) {
    *static_cast<bool *>(data) = true;
  }), &chooser_destroyed);
  gtk_widget_destroy(window);
  assert(chooser_destroyed);
  assert(!pending.current());
  pending.complete(true, {});
  std::cout << "PASS failed, pending, accepted, edited, cancelled and destroyed settings actions\n";
}
