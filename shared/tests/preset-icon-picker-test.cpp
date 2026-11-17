// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include <elder-terms/settings-widget.h>
#include <elder-terms/connection-icon-presets.h>
#include <cardio.h>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>

static GtkWidget *find(GtkWidget *widget, const char *id) {
  if (std::strcmp(gtk_widget_get_name(widget), id) == 0) return widget;
  if (!GTK_IS_CONTAINER(widget)) return nullptr;
  struct Search { const char *id; GtkWidget *result; } search{id, nullptr};
  gtk_container_forall(GTK_CONTAINER(widget), +[](GtkWidget *child, gpointer data) {
    auto *search = static_cast<Search *>(data);
    if (!search->result) search->result = find(child, search->id);
  }, &search);
  return search.result;
}

static void wait_until(const std::function<bool()> &predicate) {
  bool expired = false;
  const auto timeout = g_timeout_add_seconds(10, +[](gpointer data) -> gboolean {
    *static_cast<bool *>(data) = true;
    return G_SOURCE_REMOVE;
  }, &expired);
  while (!predicate() && !expired) g_main_context_iteration(nullptr, TRUE);
  if (!expired) g_source_remove(timeout);
  assert(predicate());
}

static std::vector<unsigned char> pixels(GtkWidget *image) {
  cairo_surface_t *surface = nullptr;
  g_object_get(image, "surface", &surface, nullptr);
  if (!surface) return {};
  cairo_surface_flush(surface);
  const auto *bytes = cairo_image_surface_get_data(surface);
  const auto size = cairo_image_surface_get_stride(surface) * cairo_image_surface_get_height(surface);
  std::vector<unsigned char> result(bytes, bytes + size);
  cairo_surface_destroy(surface);
  return result;
}

static const elder_terms::ConnectionIconPreset &last_preset(elder_terms::ConnectionIconFamily family) {
  const auto &presets = elder_terms::connection_icon_presets();
  for (auto iterator = presets.rbegin(); iterator != presets.rend(); ++iterator)
    if (iterator->family == family) return *iterator;
  std::abort();
}

static void select_last(GtkWidget *grid, const elder_terms::ConnectionIconPreset &expected) {
  auto *view = GTK_ICON_VIEW(grid);
  auto *model = gtk_icon_view_get_model(view);
  const auto count = gtk_tree_model_iter_n_children(model, nullptr);
  assert(count > 100);
  auto *last = gtk_tree_path_new_from_indices(count - 1, -1);
  gtk_icon_view_set_cursor(view, last, nullptr, FALSE);
  gtk_icon_view_scroll_to_path(view, last, TRUE, 1, 0);
  wait_until([&] {
    GtkTreePath *begin = nullptr, *end = nullptr;
    if (!gtk_icon_view_get_visible_range(view, &begin, &end)) return false;
    const bool visible = gtk_tree_path_compare(end, last) == 0;
    gtk_tree_path_free(begin);
    gtk_tree_path_free(end);
    return visible;
  });
  auto *accessible = atk_object_ref_accessible_child(gtk_widget_get_accessible(grid), count - 1);
  assert(accessible && ATK_IS_TEXT(accessible));
  auto *name = atk_text_get_text(ATK_TEXT(accessible), 0, -1);
  assert(name && expected.name == name);
  g_free(name);
  g_object_unref(accessible);
  gtk_icon_view_item_activated(view, last);
  gtk_tree_path_free(last);
}

int main(int argc, char **argv) {
  using namespace elder_terms;
  gtk_init(&argc, &argv);
  cardio::dispatcher_group_glib group;
  cardio::dispatcher_host_glib_auto dispatcher(group);
  SettingsWidgetOptions options;
  options.store = create_default_settings(default_terminal_display_settings(12), "Preset test");
  std::optional<SettingsStore> applied, saved;
  options.callbacks.apply = [&](const SettingsStore &store, SettingsWidgetAction action) {
    applied = store;
    action.complete(true, {});
  };
  options.callbacks.save = [&](const SettingsStore &store, SettingsWidgetAction action) {
    saved = store;
    action.complete(true, {});
  };
  auto *state = create_settings_widget(std::move(options));
  auto *root = settings_widget_root(state);
  auto *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_container_add(GTK_CONTAINER(window), root);
  g_signal_connect(window, "destroy", G_CALLBACK(+[](GtkWidget *, gpointer data) {
    destroy_settings_widget(static_cast<SettingsWidgetState *>(data));
  }), state);
  gtk_widget_show_all(window);
  gtk_button_clicked(GTK_BUTTON(find(root, "settings_general_icon_choose")));
  auto *popup = find(window, "settings_general_icon_picker");
  assert(GTK_IS_POPOVER(popup) && gtk_widget_get_visible(popup));
  auto *grid = find(popup, "settings_general_icon_grid");
  assert(GTK_IS_ICON_VIEW(grid));
  auto *family = find(popup, "settings_general_icon_family");
  auto *color_row = find(popup, "settings_general_icon_color_row");
  auto *black = find(popup, "settings_general_icon_color_black");
  auto *white = find(popup, "settings_general_icon_color_white");
  assert(color_row && black && white);
  assert(!gtk_widget_get_visible(color_row));
  auto *preview = find(root, "settings_general_icon_preview");
  const auto default_pixels = pixels(preview);
  assert(!default_pixels.empty());
  gtk_widget_hide(popup);
  assert(!settings_widget_is_dirty(state));
  gtk_button_clicked(GTK_BUTTON(find(root, "settings_general_icon_choose")));
  const auto &noto = last_preset(ConnectionIconFamily::Noto);
  const auto &material = last_preset(ConnectionIconFamily::Material);
  select_last(grid, noto);
  assert(!gtk_widget_get_visible(popup));
  assert(general_connection_icon(settings_widget_draft_store(state)) == noto.specification);
  wait_until([&] { return pixels(preview) != default_pixels; });
  const auto noto_pixels = pixels(preview);
  gtk_button_clicked(GTK_BUTTON(find(root, "settings_apply_button")));
  assert(applied && general_connection_icon(*applied) == noto.specification);
  gtk_button_clicked(GTK_BUTTON(find(root, "settings_general_icon_choose")));
  gtk_combo_box_set_active(GTK_COMBO_BOX(family), 1);
  assert(gtk_widget_get_visible(color_row));
  assert(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(black)));
  select_last(grid, material);
  assert(general_connection_icon(settings_widget_draft_store(state)) == material.specification);
  wait_until([&] { return pixels(preview) != default_pixels && pixels(preview) != noto_pixels; });
  gtk_button_clicked(GTK_BUTTON(find(root, "settings_general_icon_choose")));
  assert(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(black)));
  gtk_button_clicked(GTK_BUTTON(white));
  assert(general_connection_icon(settings_widget_draft_store(state)) == material.specification);
  gtk_widget_hide(popup);
  gtk_button_clicked(GTK_BUTTON(find(root, "settings_general_icon_choose")));
  assert(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(black)));
  gtk_widget_hide(popup);
  gtk_button_clicked(GTK_BUTTON(find(root, "settings_cancel_button")));
  assert(general_connection_icon(settings_widget_draft_store(state)) == noto.specification);
  wait_until([&] { return pixels(preview) == noto_pixels; });
  gtk_button_clicked(GTK_BUTTON(find(root, "settings_general_icon_choose")));
  gtk_combo_box_set_active(GTK_COMBO_BOX(family), 1);
  auto *adjustment = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(grid));
  gtk_adjustment_set_value(adjustment, gtk_adjustment_get_upper(adjustment));
  wait_until([&] { return gtk_adjustment_get_value(adjustment) > 0; });
  const auto before_color = gtk_adjustment_get_value(adjustment);
  gtk_button_clicked(GTK_BUTTON(white));
  assert(gtk_widget_get_visible(popup));
  assert(gtk_adjustment_get_value(adjustment) == before_color);
  assert(general_connection_icon(settings_widget_draft_store(state)) == noto.specification);
  select_last(grid, material);
  const auto white_specification = std::string("material:white:") +
      material.specification.substr(std::string("material:").size());
  assert(general_connection_icon(settings_widget_draft_store(state)) == white_specification);
  gtk_button_clicked(GTK_BUTTON(find(root, "settings_save_button")));
  assert(saved && general_connection_icon(*saved) == white_specification);
  auto *directory = g_dir_make_tmp("elder-presets-XXXXXX", nullptr);
  const auto path = std::filesystem::path(directory) / "connection.ini";
  assert(save_settings(*saved, path).saved);
  gtk_widget_destroy(window);
  SettingsWidgetOptions restored;
  restored.store = load_settings({.config_path = path, .startup_config_path = std::nullopt,
      .global_config_path = std::nullopt}, 12).store;
  restored.callbacks.save = [&](const SettingsStore &store, SettingsWidgetAction action) {
    saved = store;
    action.complete(true, {});
  };
  auto *reopened = create_settings_widget(std::move(restored));
  auto *reopened_root = settings_widget_root(reopened);
  auto *reopened_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_container_add(GTK_CONTAINER(reopened_window), reopened_root);
  g_signal_connect(reopened_window, "destroy", G_CALLBACK(+[](GtkWidget *, gpointer data) {
    destroy_settings_widget(static_cast<SettingsWidgetState *>(data));
  }), reopened);
  gtk_widget_show_all(reopened_window);
  auto *selection = find(reopened_root, "settings_general_icon_selection");
  assert(std::string(gtk_label_get_text(GTK_LABEL(selection))).find(material.name) != std::string::npos);
  assert(std::string(gtk_label_get_text(GTK_LABEL(selection))).find("White") != std::string::npos);
  assert(general_connection_icon(settings_widget_draft_store(reopened)) == white_specification);
  gtk_button_clicked(GTK_BUTTON(find(reopened_root, "settings_general_icon_choose")));
  auto *reopened_popup = find(reopened_window, "settings_general_icon_picker");
  assert(gtk_widget_get_visible(reopened_popup));
  assert(gtk_combo_box_get_active(GTK_COMBO_BOX(find(reopened_popup,
      "settings_general_icon_family"))) == 1);
  assert(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(find(reopened_popup,
      "settings_general_icon_color_white"))));
  auto *search = find(reopened_popup, "settings_general_icon_search");
  assert(GTK_IS_SEARCH_ENTRY(search));
  wait_until([&] { return gtk_window_get_focus(GTK_WINDOW(reopened_window)) == search; });
  auto *searched_grid = GTK_ICON_VIEW(find(reopened_popup, "settings_general_icon_grid"));
  auto *searched_family = GTK_COMBO_BOX(find(reopened_popup, "settings_general_icon_family"));
  const auto material_count = gtk_tree_model_iter_n_children(gtk_icon_view_get_model(searched_grid), nullptr);
  auto results = [&] {
    std::vector<std::string> specifications;
    auto *model = gtk_icon_view_get_model(searched_grid);
    GtkTreeIter row;
    if (gtk_tree_model_get_iter_first(model, &row)) do {
      unsigned int index;
      gtk_tree_model_get(model, &row, 1, &index, -1);
      specifications.push_back(connection_icon_presets()[index].specification);
    } while (gtk_tree_model_iter_next(model, &row));
    return specifications;
  };
  for (const char *query : {"FoLdEr_OpEn", "folder open", "FILE/FOLDER_OPEN"}) {
    gtk_entry_set_text(GTK_ENTRY(search), query);
    wait_until([&] { return results() == std::vector<std::string>{"material:file/folder_open"}; });
  }
  g_signal_emit_by_name(search, "search-changed");
  auto *first = gtk_tree_path_new_from_indices(0, -1);
  gtk_icon_view_select_path(searched_grid, first);
  g_signal_emit_by_name(search, "search-changed");
  assert(gtk_icon_view_path_is_selected(searched_grid, first));
  gtk_tree_path_free(first);
  assert(!settings_widget_is_dirty(reopened));
  gtk_combo_box_set_active(searched_family, 0);
  wait_until([&] { return results().empty(); });
  gtk_entry_set_text(GTK_ENTRY(search), "FACE");
  wait_until([&] { return !results().empty(); });
  for (const auto &specification : results()) assert(specification.starts_with("noto:"));
  gtk_combo_box_set_active(searched_family, 1);
  wait_until([&] { return !results().empty(); });
  for (const auto &specification : results()) assert(specification.starts_with("material:"));
  gtk_entry_set_text(GTK_ENTRY(search), "no such icon 7193");
  wait_until([&] { return results().empty(); });
  auto *empty = find(reopened_popup, "settings_general_icon_empty");
  assert(GTK_IS_LABEL(empty) && gtk_widget_get_visible(empty));
  gtk_entry_set_text(GTK_ENTRY(search), "");
  wait_until([&] { return static_cast<int>(results().size()) == material_count; });
  assert(!gtk_widget_get_visible(empty));
  // Immediate Enter must use the new text even before delayed filtering runs.
  gtk_entry_set_text(GTK_ENTRY(search), "folder open");
  g_signal_emit_by_name(search, "activate");
  assert(!gtk_widget_get_visible(reopened_popup));
  const std::string searched_specification = "material:white:file/folder_open";
  assert(general_connection_icon(settings_widget_draft_store(reopened)) == searched_specification);
  gtk_button_clicked(GTK_BUTTON(find(reopened_root, "settings_save_button")));
  assert(saved && general_connection_icon(*saved) == searched_specification);
  assert(save_settings(*saved, path).saved);
  assert(general_connection_icon(load_settings({.config_path = path, .startup_config_path = std::nullopt,
      .global_config_path = std::nullopt}, 12).store) == searched_specification);
  gtk_button_clicked(GTK_BUTTON(find(reopened_root, "settings_general_icon_choose")));
  assert(std::string(gtk_entry_get_text(GTK_ENTRY(search))).empty());
  assert(gtk_tree_model_iter_n_children(gtk_icon_view_get_model(searched_grid), nullptr) == material_count);
  gtk_widget_destroy(reopened_window);
  std::filesystem::remove_all(directory);
  g_free(directory);
  std::cout << "PASS both preset families, scroll-to-last selection, Apply, Cancel, Save and reload\n";
}
