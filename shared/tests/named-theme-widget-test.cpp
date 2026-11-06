// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include <elder-terms/settings-widget.h>
#include <elder-terms/settings-preferences-widget.h>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace elder_terms;

static void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}
static GtkWidget *find(GtkWidget *widget, const char *id) {
  if (std::strcmp(gtk_widget_get_name(widget), id) == 0) return widget;
  if (!GTK_IS_CONTAINER(widget)) return nullptr;
  auto *children = gtk_container_get_children(GTK_CONTAINER(widget));
  GtkWidget *result = nullptr;
  for (auto *item = children; item && !result; item = item->next) result = find(GTK_WIDGET(item->data), id);
  g_list_free(children);
  return result;
}
static void write(const std::filesystem::path &path, const std::string &text) { std::ofstream(path) << text; }

static void test_shared_theme_editing(const std::filesystem::path &directory) {
  const auto global = directory / "edit-global.ini";
  const auto themes = directory / "edit-theme.ini";
  auto *preferences = create_settings_preferences_widget({.session = begin_settings_edit(global, themes, 12),
      .new_window_connections = {}, .references = {}, .changed = {}});
  auto *root = settings_preferences_widget_root(preferences);
  g_object_ref_sink(root);
  gtk_button_clicked(GTK_BUTTON(find(root, "themes_new_button")));
  gtk_entry_set_text(GTK_ENTRY(find(root, "themes_name_entry")), "Red theme");
  const GdkRGBA red{122.0 / 255, 34.0 / 255, 34.0 / 255, 1};
  for (const auto *id : {"theme_0_general_background_button", "theme_0_general_exterior_background_button"}) {
    auto *button = find(root, id);
    gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(button), &red);
    g_signal_emit_by_name(button, "color-set");
  }
  require(settings_preferences_widget_save(preferences).complete, "UI-created red theme must save");
  const auto saved = settings_preferences_widget_saved_context(preferences);
  require(saved->catalog.themes.size() == 1, "one theme must be created");
  const auto id = saved->catalog.themes.front().id;
  SettingsLoadOptions load{.config_path = std::nullopt, .startup_config_path = std::nullopt,
      .global_config_path = global, .theme_config_path = themes};
  std::vector<SettingsWidgetState *> connections;
  for (int index = 0; index < 2; ++index) {
    auto *connection = create_settings_widget({.store = load_settings(load, 12).store, .callbacks = {}});
    connections.push_back(connection);
    auto *connection_root = settings_widget_root(connection);
    g_object_ref_sink(connection_root);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(find(connection_root, "settings_general_theme_combo")), id.c_str());
    require(save_settings(settings_widget_draft_store(connection), directory / ("selected-" + std::to_string(index) + ".ini")).saved,
        "UI-selected theme must save on both connections");
  }
  auto *first_root = settings_widget_root(connections[0]);
  auto *background = find(first_root, "settings_general_background_button");
  const GdkRGBA blue{0, 0, 1, 1};
  gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(background), &blue);
  g_signal_emit_by_name(background, "color-set");
  gtk_entry_set_text(GTK_ENTRY(find(root, "themes_name_entry")), "Renamed red");
  const GdkRGBA green{0, 1, 0, 1};
  auto *theme_background = find(root, "theme_0_general_background_button");
  gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(theme_background), &green);
  g_signal_emit_by_name(theme_background, "color-set");
  gtk_button_clicked(GTK_BUTTON(find(root, "themes_duplicate_button")));
  require(settings_preferences_widget_save(preferences).complete, "renamed and duplicated themes must save");
  const auto updated = settings_preferences_widget_saved_context(preferences);
  require(updated->catalog.themes.size() == 2 && updated->catalog.themes[0].id == id && updated->catalog.themes[1].id != id,
      "rename must retain ID and duplicate must receive a new ID");
  auto fallbacks = updated->global;
  fallbacks.theme_context = updated;
  for (auto *connection : connections) settings_widget_rebase_fallbacks(connection, fallbacks);
  require(setting_string_value_or_default(settings_widget_draft_store(connections[0]), general_background_setting_key(), "") == "#0000FF",
      "first connection must keep its background override");
  require(setting_string_value_or_default(settings_widget_draft_store(connections[1]), general_background_setting_key(), "") == "#00FF00",
      "second connection must inherit the edited theme");
  gtk_combo_box_set_active_id(GTK_COMBO_BOX(find(first_root, "settings_general_background_mode_combo")), "inherit");
  require(setting_string_value_or_default(settings_widget_draft_store(connections[0]), general_background_setting_key(), "") == "#00FF00",
      "clearing the first override must expose the shared theme");
  for (int index = 0; index < 2; ++index) {
    load.config_path = directory / ("selected-" + std::to_string(index) + ".ini");
    const auto restored = load_settings(load, 12);
    require(selected_settings_theme(restored.store) == id &&
        setting_string_value_or_default(restored.store, general_background_setting_key(), "") == "#00FF00",
        "saved references must follow rename and edits without expanding theme values");
    auto *widget = settings_widget_root(connections[index]);
    destroy_settings_widget(connections[index]);
    gtk_widget_destroy(widget);
    g_object_unref(widget);
  }
  destroy_settings_preferences_widget(preferences);
  gtk_widget_destroy(root);
  g_object_unref(root);
}

static void test_widget(const std::filesystem::path &directory) {
  const auto global = directory / "global.ini";
  const auto themes = directory / "theme.ini";
  const auto connection = directory / "connection.ini";
  write(global, "[general]\nbackground=#111111\n");
  write(themes, "[theme.red]\nname=Red\n[theme.red.general]\nbackground=#990000\n"
                "[theme.green]\nname=Green\n[theme.green.general]\nbackground=#009900\n");
  write(connection, "[general]\ntheme=red\nbackground=#990000\n");
  SettingsLoadOptions load{.config_path = connection, .startup_config_path = std::nullopt, .global_config_path = global};
  SettingsStore applied;
  int applied_count = 0;
  auto *state = create_settings_widget({.store = load_settings(load, 12).store, .is_runtime = true,
      .callbacks = {.apply = [&](const auto &store, SettingsWidgetAction action) {
        applied = store; ++applied_count; action.complete(true, "");
      }, .save = {}, .cancel = {}, .changed = {}}});
  auto *root = settings_widget_root(state);
  auto *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_container_add(GTK_CONTAINER(window), root);
  auto *combo = find(root, "settings_general_theme_combo");
  require(combo != nullptr, "connection General page must expose the theme selector");
  auto *width = find(root, "settings_terminal_width_entry");
  gtk_entry_set_text(GTK_ENTRY(width), "invalid width");
  require(gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), "green"), "registered theme must be selectable");
  require(std::string(gtk_entry_get_text(GTK_ENTRY(width))) == "invalid width", "theme switch must keep invalid edits");
  auto draft = settings_widget_draft_store(state);
  require(selected_settings_theme(draft) == "green", "draft must track the selected stable ID");
  require(setting_string_value_or_default(draft, general_background_setting_key(), "") == "#990000",
          "same-value connection override must survive switching themes");
  auto *background = find(root, "settings_general_background_mode_combo");
  gtk_combo_box_set_active_id(GTK_COMBO_BOX(background), "inherit");
  require(setting_string_value_or_default(settings_widget_draft_store(state), general_background_setting_key(), "") == "#009900",
          "explicit clear must expose the selected theme");
  gchar *label = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(background));
  require(label && std::string(label).find("Green") != std::string::npos, "inheritance label must name the selected theme");
  g_free(label);
  require(!settings_widget_is_valid(state), "theme switch must not hide unrelated invalid input");
  require(applied_count == 0, "theme preview must not apply runtime changes");

  write(themes, "[theme.red]\nname=Renamed red\n[theme.red.general]\nbackground=#550000\n"
                "[theme.green]\nname=Green\n[theme.green.general]\nbackground=#005500\n");
  const auto refreshed = load_settings(load, 12).store;
  settings_widget_rebase_fallbacks(state, refreshed);
  require(std::string(gtk_entry_get_text(GTK_ENTRY(width))) == "invalid width", "theme refresh must preserve invalid text");
  require(setting_string_value_or_default(settings_widget_draft_store(state), general_background_setting_key(), "") == "#005500",
          "draft must rebase its own green selection");
  gtk_button_clicked(GTK_BUTTON(find(root, "settings_cancel_button")));
  draft = settings_widget_draft_store(state);
  require(selected_settings_theme(draft) == "red", "cancel must restore the applied theme selection");
  gtk_combo_box_set_active_id(GTK_COMBO_BOX(background), "inherit");
  require(setting_string_value_or_default(settings_widget_draft_store(state), general_background_setting_key(), "") == "#550000",
          "applied and draft stores must rebase with their separate selections");
  gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), "");
  require(setting_string_value_or_default(settings_widget_draft_store(state), general_background_setting_key(), "") == "#111111",
          "none must restore global defaults");
  gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), "green");
  gtk_button_clicked(GTK_BUTTON(find(root, "settings_apply_button")));
  require(applied_count == 1 && selected_settings_theme(applied) == "green", "Apply must use the selected theme");
  require(save_settings(applied, connection).saved, "selected reference must save");
  const auto saved = load_settings(load, 12);
  require(selected_settings_theme(saved.store) == "green" && !setting_has_explicit_value(saved.store, general_background_setting_key()),
          "connection saves must contain reference and explicit overrides only");
  // Match the application's container destroy callback lifetime.
  g_signal_connect(window, "destroy", G_CALLBACK(+[](GtkWidget *, gpointer data) {
    destroy_settings_widget(static_cast<SettingsWidgetState *>(data));
  }), state);
  gtk_widget_destroy(window);
}

int main(int argc, char **argv) {
  gtk_init(&argc, &argv);
  gchar *temporary = g_dir_make_tmp("elder-theme-widget-XXXXXX", nullptr);
  if (!temporary) return 1;
  const std::filesystem::path directory(temporary);
  g_free(temporary);
  try {
    test_widget(directory);
    test_shared_theme_editing(directory);
    std::filesystem::remove_all(directory);
    std::cout << "Theme widget passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::filesystem::remove_all(directory);
    std::cerr << error.what() << "\n";
    return 1;
  }
}
