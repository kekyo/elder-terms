// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "connection-icon-picker.h"
#include <elder-terms/connection-icon-presets.h>
#include <algorithm>
#include <cctype>
#include <list>
#include <memory>
#include <unordered_map>

#define GETTEXT_PACKAGE "elder-terms"
#include <glib/gi18n-lib.h>

namespace elder_terms {

static constexpr int thumbnail_size = 40;
static constexpr std::size_t cache_limit = 128;

struct IconPickerThumbnail {
  std::shared_ptr<cairo_surface_t> surface;
  std::list<unsigned int>::iterator position;
};

struct ConnectionIconPicker {
  GtkWidget *popup;
  GtkWidget *grid;
  GtkWidget *family;
  GtkWidget *search;
  GtkWidget *empty;
  GtkWidget *color_row;
  GtkWidget *black;
  GtkWidget *white;
  std::function<void(std::string)> selected;
  std::function<void()> choose_file;
  std::unordered_map<unsigned int, IconPickerThumbnail> thumbnails;
  std::list<unsigned int> recent;
  std::string filtered_query;
  int filtered_family = -1;
  guint loading = 0;
  int scale = 1;
  bool destroyed = false;
};

static void identify(GtkWidget *widget, const std::string &id) {
  gtk_widget_set_name(widget, id.c_str());
  atk_object_set_accessible_id(gtk_widget_get_accessible(widget), id.c_str());
}

static std::string preset_specification(ConnectionIconPicker *picker,
                                       unsigned int index) {
  const auto &base = connection_icon_presets()[index].specification;
  if (connection_icon_presets()[index].family == ConnectionIconFamily::Material &&
      gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(picker->white)))
    return "material:white:" + base.substr(std::string("material:").size());
  return base;
}

static gboolean load_visible_thumbnails(gpointer data) {
  auto *picker = static_cast<ConnectionIconPicker *>(data);
  auto *view = GTK_ICON_VIEW(picker->grid);
  GtkTreePath *first = nullptr, *last = nullptr;
  if (!gtk_widget_get_mapped(picker->popup) ||
      !gtk_icon_view_get_visible_range(view, &first, &last)) {
    picker->loading = 0;
    return G_SOURCE_REMOVE;
  }
  const int begin = gtk_tree_path_get_indices(first)[0];
  const int end = gtk_tree_path_get_indices(last)[0];
  gtk_tree_path_free(first);
  gtk_tree_path_free(last);
  auto *model = gtk_icon_view_get_model(view);
  const auto &catalog = connection_icon_presets();
  int loaded = 0;
  bool remaining = false;
  for (int row = begin; row <= end; ++row) {
    GtkTreeIter iterator;
    if (!gtk_tree_model_iter_nth_child(model, &iterator, nullptr, row)) continue;
    unsigned int index = 0;
    gtk_tree_model_get(model, &iterator, 1, &index, -1);
    auto *accessible = atk_object_ref_accessible_child(gtk_widget_get_accessible(picker->grid), row);
    if (accessible) {
      atk_object_set_name(accessible, catalog[index].name.c_str());
      g_object_unref(accessible);
    }
    const auto cached = picker->thumbnails.find(index);
    if (cached != picker->thumbnails.end()) {
      picker->recent.splice(picker->recent.begin(), picker->recent, cached->second.position);
      continue;
    }
    // Yield between small batches so scrolling and keyboard input remain live.
    if (loaded == 8) { remaining = true; break; }
    auto pixels = connection_icon_preset_thumbnail(preset_specification(picker, index),
                                                   thumbnail_size * picker->scale);
    std::shared_ptr<cairo_surface_t> surface;
    if (pixels) surface.reset(gdk_cairo_surface_create_from_pixbuf(
        pixels.get(), picker->scale, nullptr), cairo_surface_destroy);
    picker->recent.push_front(index);
    picker->thumbnails.emplace(index, IconPickerThumbnail{std::move(surface), picker->recent.begin()});
    if (picker->recent.size() > cache_limit) {
      picker->thumbnails.erase(picker->recent.back());
      picker->recent.pop_back();
    }
    ++loaded;
  }
  if (loaded) gtk_widget_queue_draw(picker->grid);
  if (remaining) return G_SOURCE_CONTINUE;
  picker->loading = 0;
  return G_SOURCE_REMOVE;
}

static void schedule_thumbnails(ConnectionIconPicker *picker) {
  if (!picker->destroyed && !picker->loading)
    picker->loading = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, load_visible_thumbnails, picker, nullptr);
}

static std::string search_text(std::string value) {
  for (auto &character : value)
    character = character == '_' ? ' ' : static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
  return value;
}

static void update_icons(ConnectionIconPicker *picker) {
  const auto family_index = gtk_combo_box_get_active(GTK_COMBO_BOX(picker->family));
  const auto query = search_text(gtk_entry_get_text(GTK_ENTRY(picker->search)));
  // Reopening or a delayed search notification must not replace unchanged
  // rows, their keyboard selection, or their accessible objects.
  if (picker->filtered_family == family_index && picker->filtered_query == query) return;
  picker->filtered_family = family_index;
  picker->filtered_query = query;
  const auto family = family_index == 1
      ? ConnectionIconFamily::Material : ConnectionIconFamily::Noto;
  gtk_widget_set_visible(picker->color_row, family == ConnectionIconFamily::Material);
  auto *model = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_UINT);
  const auto &catalog = connection_icon_presets();
  for (unsigned int index = 0; index < catalog.size(); ++index) {
    if (catalog[index].family != family) continue;
    if (search_text(catalog[index].name).find(query) == std::string::npos &&
        search_text(catalog[index].specification).find(query) == std::string::npos) continue;
    GtkTreeIter iterator;
    gtk_list_store_insert_with_values(model, &iterator, -1,
        0, catalog[index].name.c_str(), 1, index, -1);
  }
  gtk_icon_view_set_model(GTK_ICON_VIEW(picker->grid), GTK_TREE_MODEL(model));
  gtk_widget_set_visible(picker->empty, gtk_tree_model_iter_n_children(GTK_TREE_MODEL(model), nullptr) == 0);
  g_object_unref(model);
  auto *adjustment = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(picker->grid));
  gtk_adjustment_set_value(adjustment, gtk_adjustment_get_lower(adjustment));
  schedule_thumbnails(picker);
}

void sync_connection_icon_picker(GtkWidget *popup, const std::string &specification) {
  auto *picker = static_cast<ConnectionIconPicker *>(
      g_object_get_data(G_OBJECT(popup), "elder-terms-icon-picker"));
  if (!picker) return;
  const bool material = specification.starts_with("material:");
  auto *color = specification.starts_with("material:white:")
      ? picker->white : picker->black;
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(color), TRUE);
  gtk_entry_set_text(GTK_ENTRY(picker->search), "");
  gtk_combo_box_set_active(GTK_COMBO_BOX(picker->family), material ? 1 : 0);
  update_icons(picker);
}

GtkWidget *create_connection_icon_picker(GtkWidget *relative_to,
    const std::string &prefix, std::function<void(std::string)> selected,
    std::function<void()> choose_file) {
  auto *picker = new ConnectionIconPicker{};
  picker->popup = gtk_popover_new(relative_to);
  g_object_set_data_full(G_OBJECT(picker->popup), "elder-terms-icon-picker", picker, +[](gpointer data) {
    delete static_cast<ConnectionIconPicker *>(data);
  });
  picker->grid = gtk_icon_view_new();
  picker->family = gtk_combo_box_text_new();
  picker->search = gtk_search_entry_new();
  picker->empty = gtk_label_new(_("No matching icons"));
  picker->color_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  picker->black = gtk_radio_button_new_with_label(nullptr, _("Black"));
  picker->white = gtk_radio_button_new_with_label_from_widget(
      GTK_RADIO_BUTTON(picker->black), _("White"));
  picker->selected = std::move(selected);
  picker->choose_file = std::move(choose_file);
  picker->scale = gtk_widget_get_scale_factor(relative_to);
  identify(picker->popup, prefix + "_general_icon_picker");
  identify(picker->grid, prefix + "_general_icon_grid");
  identify(picker->family, prefix + "_general_icon_family");
  identify(picker->search, prefix + "_general_icon_search");
  identify(picker->empty, prefix + "_general_icon_empty");
  identify(picker->color_row, prefix + "_general_icon_color_row");
  identify(picker->black, prefix + "_general_icon_color_black");
  identify(picker->white, prefix + "_general_icon_color_white");
  auto *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_container_set_border_width(GTK_CONTAINER(box), 8);
  gtk_container_add(GTK_CONTAINER(picker->popup), box);
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(picker->family), "Noto Color Emoji");
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(picker->family), "Material Icons");
  gtk_combo_box_set_active(GTK_COMBO_BOX(picker->family), 0);
  gtk_entry_set_placeholder_text(GTK_ENTRY(picker->search), _("Search icons…"));
  atk_object_set_name(gtk_widget_get_accessible(picker->search), _("Search icons…"));
  gtk_box_pack_start(GTK_BOX(box), picker->search, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), picker->family, FALSE, FALSE, 0);
  auto *color_label = gtk_label_new(_("Glyph color"));
  gtk_box_pack_start(GTK_BOX(picker->color_row), color_label, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(picker->color_row), picker->black, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(picker->color_row), picker->white, FALSE, FALSE, 0);
  gtk_widget_show_all(picker->color_row);
  gtk_widget_set_no_show_all(picker->color_row, TRUE);
  gtk_widget_hide(picker->color_row);
  gtk_box_pack_start(GTK_BOX(box), picker->color_row, FALSE, FALSE, 0);
  auto *scroll = gtk_scrolled_window_new(nullptr, nullptr);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);
  identify(gtk_scrolled_window_get_vscrollbar(GTK_SCROLLED_WINDOW(scroll)),
      prefix + "_general_icon_scrollbar");
  gtk_widget_set_size_request(scroll, 432, 300);
  gtk_container_add(GTK_CONTAINER(scroll), picker->grid);
  gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);
  gtk_widget_set_no_show_all(picker->empty, TRUE);
  gtk_box_pack_start(GTK_BOX(box), picker->empty, FALSE, FALSE, 0);
  auto *view = GTK_ICON_VIEW(picker->grid);
  gtk_icon_view_set_item_width(view, thumbnail_size);
  gtk_icon_view_set_item_padding(view, 4);
  gtk_icon_view_set_margin(view, 4);
  gtk_icon_view_set_column_spacing(view, 2);
  gtk_icon_view_set_row_spacing(view, 2);
  gtk_icon_view_set_selection_mode(view, GTK_SELECTION_SINGLE);
  gtk_icon_view_set_activate_on_single_click(view, TRUE);
  gtk_icon_view_set_tooltip_column(view, 0);
  auto *image = gtk_cell_renderer_pixbuf_new();
  gtk_cell_renderer_set_fixed_size(image, thumbnail_size, thumbnail_size);
  gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(view), image, FALSE);
  gtk_cell_layout_set_cell_data_func(GTK_CELL_LAYOUT(view), image,
      +[](GtkCellLayout *, GtkCellRenderer *cell, GtkTreeModel *model,
          GtkTreeIter *iterator, gpointer data) {
        auto *picker = static_cast<ConnectionIconPicker *>(data);
        unsigned int index = 0;
        gtk_tree_model_get(model, iterator, 1, &index, -1);
        const auto found = picker->thumbnails.find(index);
        g_object_set(cell, "surface", found == picker->thumbnails.end()
            ? nullptr : found->second.surface.get(), nullptr);
      }, picker, nullptr);
  // GtkIconView exposes renderer text through AtkText even when labels are
  // hidden, allowing screen readers to identify offscreen items as well.
  auto *text = gtk_cell_renderer_text_new();
  g_object_set(text, "visible", FALSE, nullptr);
  gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(view), text, FALSE);
  gtk_cell_layout_add_attribute(GTK_CELL_LAYOUT(view), text, "text", 0);
  auto *file = gtk_button_new_with_label(_("Choose image file…"));
  identify(file, prefix + "_general_icon_file");
  gtk_box_pack_start(GTK_BOX(box), file, FALSE, FALSE, 0);
  g_signal_connect(file, "clicked", G_CALLBACK(+[](GtkButton *, gpointer data) {
    auto *picker = static_cast<ConnectionIconPicker *>(data);
    auto callback = picker->choose_file;
    gtk_widget_hide(picker->popup);
    callback();
  }), picker);
  g_signal_connect(view, "item-activated", G_CALLBACK(+[](GtkIconView *view, GtkTreePath *path, gpointer data) {
    auto *picker = static_cast<ConnectionIconPicker *>(data);
    GtkTreeIter iterator;
    auto *model = gtk_icon_view_get_model(view);
    if (!gtk_tree_model_get_iter(model, &iterator, path)) return;
    unsigned int index = 0;
    gtk_tree_model_get(model, &iterator, 1, &index, -1);
    const auto specification = preset_specification(picker, index);
    auto callback = picker->selected;
    gtk_widget_hide(picker->popup);
    callback(specification);
  }), picker);
  g_signal_connect(picker->family, "changed", G_CALLBACK(+[](GtkComboBox *, gpointer data) {
    update_icons(static_cast<ConnectionIconPicker *>(data));
  }), picker);
  g_signal_connect(picker->search, "search-changed", G_CALLBACK(+[](GtkSearchEntry *, gpointer data) {
    update_icons(static_cast<ConnectionIconPicker *>(data));
  }), picker);
  g_signal_connect(picker->search, "activate", G_CALLBACK(+[](GtkEntry *, gpointer data) {
    auto *picker = static_cast<ConnectionIconPicker *>(data);
    // Enter may precede the delayed search-changed signal. Resolve the current
    // text before activating a result so the previous query cannot be chosen.
    update_icons(picker);
    auto *view = GTK_ICON_VIEW(picker->grid);
    if (!gtk_tree_model_iter_n_children(gtk_icon_view_get_model(view), nullptr)) return;
    auto *first = gtk_tree_path_new_from_indices(0, -1);
    gtk_icon_view_item_activated(view, first);
    gtk_tree_path_free(first);
  }), picker);
  g_signal_connect(picker->search, "stop-search", G_CALLBACK(+[](GtkSearchEntry *, gpointer data) {
    gtk_widget_hide(static_cast<ConnectionIconPicker *>(data)->popup);
  }), picker);
  g_signal_connect(picker->white, "toggled", G_CALLBACK(+[](GtkToggleButton *, gpointer data) {
    auto *picker = static_cast<ConnectionIconPicker *>(data);
    picker->thumbnails.clear();
    picker->recent.clear();
    gtk_widget_queue_draw(picker->grid);
    schedule_thumbnails(picker);
  }), picker);
  g_signal_connect(picker->grid, "map", G_CALLBACK(+[](GtkWidget *, gpointer data) {
    schedule_thumbnails(static_cast<ConnectionIconPicker *>(data));
  }), picker);
  g_signal_connect(picker->grid, "size-allocate", G_CALLBACK(+[](GtkWidget *, GtkAllocation *, gpointer data) {
    schedule_thumbnails(static_cast<ConnectionIconPicker *>(data));
  }), picker);
  g_signal_connect(picker->grid, "notify::scale-factor", G_CALLBACK(+[](GObject *, GParamSpec *, gpointer data) {
    auto *picker = static_cast<ConnectionIconPicker *>(data);
    picker->scale = gtk_widget_get_scale_factor(picker->grid);
    picker->thumbnails.clear();
    picker->recent.clear();
    schedule_thumbnails(picker);
  }), picker);
  auto *adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll));
  g_signal_connect(adjustment, "value-changed", G_CALLBACK(+[](GtkAdjustment *, gpointer data) {
    schedule_thumbnails(static_cast<ConnectionIconPicker *>(data));
  }), picker);
  g_signal_connect(picker->popup, "destroy", G_CALLBACK(+[](GtkWidget *, gpointer data) {
    auto *picker = static_cast<ConnectionIconPicker *>(data);
    if (picker->loading) g_source_remove(picker->loading);
    picker->loading = 0;
    picker->destroyed = true;
  }), picker);
  update_icons(picker);
  gtk_widget_show_all(box);
  return picker->popup;
}

} // namespace elder_terms
