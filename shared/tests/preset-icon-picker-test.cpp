// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include <elder-terms/settings-widget.h>
#include <elder-terms/connection-icon-presets.h>
#include <cardio.h>
#include <array>
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

static std::vector<std::string> results(GtkWidget *grid) {
  std::vector<std::string> specifications;
  auto *model = gtk_icon_view_get_model(GTK_ICON_VIEW(grid));
  GtkTreeIter row;
  if (gtk_tree_model_get_iter_first(model, &row)) do {
    gchar *specification = nullptr;
    gtk_tree_model_get(model, &row, 1, &specification, -1);
    specifications.emplace_back(specification);
    g_free(specification);
  } while (gtk_tree_model_iter_next(model, &row));
  return specifications;
}

static void select_last(GtkWidget *grid, GtkWidget *scroll) {
  auto *view = GTK_ICON_VIEW(grid);
  const auto count = results(grid).size();
  assert(count > 100);
  auto *last = gtk_tree_path_new_from_indices(count - 1, -1);
  gtk_icon_view_set_cursor(view, last, nullptr, FALSE);
  gtk_icon_view_select_path(view, last);
  auto *viewport = gtk_bin_get_child(GTK_BIN(scroll));
  wait_until([&] {
    GdkRectangle cell{};
    int y = 0;
    return gtk_icon_view_get_cell_rect(view, last, nullptr, &cell) &&
        gtk_widget_translate_coordinates(grid, viewport, 0, cell.y, nullptr, &y) &&
        y >= 0 && y + cell.height <= gtk_widget_get_allocated_height(viewport);
  });
  auto *accessible = atk_object_ref_accessible_child(gtk_widget_get_accessible(grid), count - 1);
  assert(accessible && ATK_IS_TEXT(accessible));
  auto *name = atk_text_get_text(ATK_TEXT(accessible), 0, -1);
  assert(name && *name);
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
  auto *choose = GTK_BUTTON(find(root, "settings_general_icon_choose"));
  gtk_button_clicked(choose);
  auto *popup = find(window, "settings_general_icon_picker");
  assert(GTK_IS_POPOVER(popup) && gtk_widget_get_visible(popup));
  // Temporary removal checks; keep the behavioral coverage after migration.
  assert(find(popup, "settings_general_icon_family") == nullptr);
  assert(find(popup, "settings_general_icon_color_row") == nullptr);
  const std::array<const char *, 3> ids{"emoji", "material_black", "material_white"};
  const std::array<const char *, 3> titles{"Emoji", "Material (Black)", "Material (White)"};
  std::array<GtkWidget *, 3> grids{};
  std::array<std::vector<std::string>, 3> all;
  int previous_y = -1;
  for (std::size_t i = 0; i < ids.size(); ++i) {
    grids[i] = find(popup, (std::string("settings_general_icon_grid_") + ids[i]).c_str());
    assert(GTK_IS_ICON_VIEW(grids[i]));
    auto *heading = find(popup, (std::string("settings_general_icon_heading_") + ids[i]).c_str());
    assert(std::string(gtk_label_get_text(GTK_LABEL(heading))) == titles[i]);
    int y = 0;
    wait_until([&] { return gtk_widget_get_mapped(grids[i]) && gtk_widget_get_allocated_height(grids[i]) > 100; });
    assert(gtk_widget_translate_coordinates(grids[i], find(popup, "settings_general_icon_grid"), 0, 0, nullptr, &y));
    assert(y > previous_y);
    previous_y = y;
    all[i] = results(grids[i]);
    assert(all[i].size() > 100);
  }
  assert(all[1].size() == all[2].size());
  for (std::size_t i = 0; i < all[1].size(); ++i)
    assert(all[2][i] == "material:white:" + all[1][i].substr(9));
  auto *scroll = find(popup, "settings_general_icon_scroll");
  auto *adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll));
  auto *search = GTK_ENTRY(find(popup, "settings_general_icon_search"));
  auto *preview = find(root, "settings_general_icon_preview");
  const auto default_pixels = pixels(preview);
  assert(!default_pixels.empty());
  gtk_widget_hide(popup);
  assert(!settings_widget_is_dirty(state));
  gtk_button_clicked(choose);
  select_last(grids[0], scroll);
  const auto noto = all[0].back();
  assert(!gtk_widget_get_visible(popup));
  assert(general_connection_icon(settings_widget_draft_store(state)) == noto);
  wait_until([&] { return pixels(preview) != default_pixels; });
  const auto noto_pixels = pixels(preview);
  gtk_button_clicked(GTK_BUTTON(find(root, "settings_apply_button")));
  assert(applied && general_connection_icon(*applied) == noto);
  gtk_button_clicked(choose);
  select_last(grids[1], scroll);
  assert(general_connection_icon(settings_widget_draft_store(state)) == all[1].back());
  wait_until([&] { return pixels(preview) != default_pixels && pixels(preview) != noto_pixels; });
  const auto black_pixels = pixels(preview);
  gtk_button_clicked(GTK_BUTTON(find(root, "settings_cancel_button")));
  assert(general_connection_icon(settings_widget_draft_store(state)) == noto);
  gtk_button_clicked(choose);
  select_last(grids[2], scroll);
  const auto white_specification = all[2].back();
  assert(general_connection_icon(settings_widget_draft_store(state)) == white_specification);
  wait_until([&] { return pixels(preview) != black_pixels && pixels(preview) != noto_pixels; });
  gtk_button_clicked(GTK_BUTTON(find(root, "settings_save_button")));
  assert(saved && general_connection_icon(*saved) == white_specification);
  auto *directory = g_dir_make_tmp("elder-presets-XXXXXX", nullptr);
  const auto path = std::filesystem::path(directory) / "connection.ini";
  assert(save_settings(*saved, path).saved);
  assert(general_connection_icon(load_settings({.config_path = path, .startup_config_path = std::nullopt,
      .global_config_path = std::nullopt}, 12).store) == white_specification);
  gtk_button_clicked(choose);
  wait_until([&] { return gtk_adjustment_get_value(adjustment) > 0; });
  wait_until([&] { return gtk_window_get_focus(GTK_WINDOW(window)) == GTK_WIDGET(search); });
  auto *last = gtk_tree_path_new_from_indices(all[2].size() - 1, -1);
  wait_until([&] { return gtk_icon_view_path_is_selected(GTK_ICON_VIEW(grids[2]), last); });
  gtk_tree_path_free(last);
  const bool dirty_before_search = settings_widget_is_dirty(state);
  auto *current_group = find(popup, "settings_general_icon_current_group");
  wait_until([&] {
    return gtk_widget_get_visible(gtk_widget_get_parent(current_group)) &&
        std::string(gtk_label_get_text(GTK_LABEL(current_group))) == "Material (White)";
  });
  for (const char *query : {"FoLdEr_OpEn", "folder open", "folder-open", "FILE/FOLDER_OPEN"}) {
    gtk_entry_set_text(search, query);
    g_signal_emit_by_name(search, "search-changed");
    wait_until([&] {
      return results(grids[0]).empty() &&
          results(grids[1]) == std::vector<std::string>{"material:file/folder_open"} &&
          results(grids[2]) == std::vector<std::string>{"material:white:file/folder_open"};
    });
    assert(!gtk_widget_get_visible(gtk_widget_get_parent(grids[0])));
  }
  auto *first = gtk_tree_path_new_from_indices(0, -1);
  gtk_icon_view_select_path(GTK_ICON_VIEW(grids[2]), first);
  g_signal_emit_by_name(search, "search-changed");
  assert(gtk_icon_view_path_is_selected(GTK_ICON_VIEW(grids[2]), first));
  gtk_tree_path_free(first);
  assert(settings_widget_is_dirty(state) == dirty_before_search);
  gtk_entry_set_text(search, "folder");
  wait_until([&] { return results(grids[0]).size() == 2 && results(grids[1]).size() == 12 && results(grids[2]).size() == 12; });
  gtk_entry_set_text(search, "no such icon 7193");
  auto *empty = find(popup, "settings_general_icon_empty");
  wait_until([&] { return gtk_widget_get_visible(empty); });
  for (auto *grid : grids) assert(results(grid).empty());
  gtk_entry_set_text(search, "");
  wait_until([&] { return results(grids[0]) == all[0] && results(grids[1]) == all[1] && results(grids[2]) == all[2]; });
  assert(!gtk_widget_get_visible(empty));
  // Immediate Enter must use the new text even before delayed filtering runs.
  gtk_entry_set_text(search, "material:white:file/folder_open");
  g_signal_emit_by_name(search, "activate");
  assert(!gtk_widget_get_visible(popup));
  assert(general_connection_icon(settings_widget_draft_store(state)) == "material:white:file/folder_open");
  gtk_button_clicked(GTK_BUTTON(find(root, "settings_save_button")));
  assert(saved && general_connection_icon(*saved) == "material:white:file/folder_open");
  assert(save_settings(*saved, path).saved);
  gtk_button_clicked(choose);
  assert(std::string(gtk_entry_get_text(search)).empty());
  for (std::size_t i = 0; i < grids.size(); ++i) assert(results(grids[i]) == all[i]);
  gtk_widget_destroy(window);
  std::filesystem::remove_all(directory);
  g_free(directory);
  std::cout << "PASS grouped presets, shared scrolling, colors, search, Apply, Cancel, Save and reload\n";
}
