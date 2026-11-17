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
  std::list<std::string>::iterator position;
};

struct ConnectionIconPicker;

struct IconPickerGroup {
  ConnectionIconPicker *picker;
  ConnectionIconFamily family;
  bool white;
  std::string title;
  GtkWidget *box;
  GtkWidget *heading;
  GtkWidget *grid;
};

struct ConnectionIconPicker {
  GtkWidget *popup;
  GtkWidget *search;
  GtkWidget *empty;
  GtkWidget *scroll;
  GtkWidget *viewport;
  GtkWidget *content;
  GtkWidget *overlay;
  GtkWidget *sticky;
  GtkWidget *sticky_label;
  GtkAdjustment *adjustment;
  std::vector<std::unique_ptr<IconPickerGroup>> groups;
  std::function<void(std::string)> selected;
  std::function<void()> choose_file;
  std::unordered_map<std::string, IconPickerThumbnail> thumbnails;
  std::list<std::string> recent;
  std::string filtered_query;
  std::string pending_selection;
  guint loading = 0;
  int scale = 1;
  int sticky_y = 0;
  bool initialized = false;
  bool changing_selection = false;
  bool destroyed = false;
};

static void identify(GtkWidget *widget, const std::string &id) {
  gtk_widget_set_name(widget, id.c_str());
  atk_object_set_accessible_id(gtk_widget_get_accessible(widget), id.c_str());
}

static std::string row_string(GtkTreeModel *model, GtkTreeIter *row, int column) {
  gchar *text = nullptr;
  gtk_tree_model_get(model, row, column, &text, -1);
  std::string result = text ? text : "";
  g_free(text);
  return result;
}

static int group_count(const IconPickerGroup *group) {
  return gtk_tree_model_iter_n_children(
      gtk_icon_view_get_model(GTK_ICON_VIEW(group->grid)), nullptr);
}

static void update_heading(ConnectionIconPicker *picker) {
  const IconPickerGroup *active = nullptr;
  int next_y = gtk_widget_get_allocated_height(picker->viewport);
  for (const auto &group : picker->groups) {
    if (!gtk_widget_get_visible(group->box)) continue;
    int y = 0;
    if (!gtk_widget_translate_coordinates(group->heading, picker->content, 0, 0, nullptr, &y)) continue;
    y -= static_cast<int>(gtk_adjustment_get_value(picker->adjustment));
    if (y < 0) active = group.get();
    else if (active) { next_y = y; break; }
  }
  gtk_widget_set_visible(picker->sticky, active != nullptr);
  if (!active) return;
  gtk_label_set_text(GTK_LABEL(picker->sticky_label), active->title.c_str());
  int height = 0;
  gtk_widget_get_preferred_height(picker->sticky, nullptr, &height);
  const int y = std::min(0, next_y - height);
  if (picker->sticky_y != y) {
    picker->sticky_y = y;
    gtk_widget_queue_resize(picker->overlay);
  }
}

static void reveal_item(IconPickerGroup *group, GtkTreePath *path) {
  auto *picker = group->picker;
  if (!gtk_widget_get_mapped(group->grid)) return;
  GdkRectangle cell{};
  if (!gtk_icon_view_get_cell_rect(GTK_ICON_VIEW(group->grid), path, nullptr, &cell)) return;
  int y = 0;
  if (!gtk_widget_translate_coordinates(group->grid, picker->content, 0, cell.y, nullptr, &y)) return;
  const int heading = gtk_widget_get_allocated_height(group->heading);
  gtk_adjustment_clamp_page(picker->adjustment, std::max(0, y - heading), y + cell.height);
}

static void select_item(IconPickerGroup *group, int index, bool focus) {
  auto *picker = group->picker;
  picker->changing_selection = true;
  for (const auto &other : picker->groups)
    gtk_icon_view_unselect_all(GTK_ICON_VIEW(other->grid));
  auto *path = gtk_tree_path_new_from_indices(index, -1);
  gtk_icon_view_set_cursor(GTK_ICON_VIEW(group->grid), path, nullptr, FALSE);
  gtk_icon_view_select_path(GTK_ICON_VIEW(group->grid), path);
  if (focus) gtk_widget_grab_focus(group->grid);
  reveal_item(group, path);
  gtk_tree_path_free(path);
  picker->changing_selection = false;
}

static gboolean load_visible_thumbnails(gpointer data) {
  auto *picker = static_cast<ConnectionIconPicker *>(data);
  if (!gtk_widget_get_mapped(picker->popup)) {
    picker->loading = 0;
    return G_SOURCE_REMOVE;
  }
  if (!picker->pending_selection.empty()) {
    for (const auto &group : picker->groups) {
      auto *model = gtk_icon_view_get_model(GTK_ICON_VIEW(group->grid));
      GtkTreeIter row;
      int index = 0;
      if (gtk_tree_model_get_iter_first(model, &row)) do {
        if (row_string(model, &row, 1) == picker->pending_selection) {
          select_item(group.get(), index, false);
          break;
        }
        ++index;
      } while (gtk_tree_model_iter_next(model, &row));
    }
    picker->pending_selection.clear();
  }
  update_heading(picker);
  int loaded = 0;
  for (const auto &group : picker->groups) {
    if (!gtk_widget_get_mapped(group->grid) || !group_count(group.get())) continue;
    int y = 0;
    if (!gtk_widget_translate_coordinates(group->grid, picker->content, 0, 0, nullptr, &y)) continue;
    y -= static_cast<int>(gtk_adjustment_get_value(picker->adjustment));
    const int height = gtk_widget_get_allocated_height(picker->viewport);
    if (y >= height || y + gtk_widget_get_allocated_height(group->grid) <= 0) continue;
    auto *view = GTK_ICON_VIEW(group->grid);
    auto *first = gtk_tree_path_new_from_indices(0, -1);
    GdkRectangle cell{};
    const bool laid_out = gtk_icon_view_get_cell_rect(view, first, nullptr, &cell) && cell.height > 0;
    gtk_tree_path_free(first);
    if (!laid_out) continue;
    // Each grid has its full natural height. Clip against the shared viewport,
    // rather than GtkIconView's own adjustment, which covers the entire group.
    const int stride = cell.height + gtk_icon_view_get_row_spacing(view);
    const int columns = gtk_icon_view_get_columns(view);
    const int begin = std::max(0, (-y - cell.y) / stride) * columns;
    const int end = std::min(group_count(group.get()),
        (std::max(0, (height - y - cell.y) / stride) + 1) * columns);
    auto *model = gtk_icon_view_get_model(view);
    for (int index = begin; index < end; ++index) {
      GtkTreeIter row;
      if (!gtk_tree_model_iter_nth_child(model, &row, nullptr, index)) continue;
      const auto specification = row_string(model, &row, 1);
      auto *accessible = atk_object_ref_accessible_child(gtk_widget_get_accessible(group->grid), index);
      if (accessible) {
        atk_object_set_name(accessible, row_string(model, &row, 0).c_str());
        g_object_unref(accessible);
      }
      const auto cached = picker->thumbnails.find(specification);
      if (cached != picker->thumbnails.end()) {
        picker->recent.splice(picker->recent.begin(), picker->recent, cached->second.position);
        continue;
      }
      // Yield between small batches so scrolling and keyboard input remain live.
      if (loaded == 8) return G_SOURCE_CONTINUE;
      auto pixels = connection_icon_preset_thumbnail(specification, thumbnail_size * picker->scale);
      std::shared_ptr<cairo_surface_t> surface;
      if (pixels) surface.reset(gdk_cairo_surface_create_from_pixbuf(
          pixels.get(), picker->scale, nullptr), cairo_surface_destroy);
      picker->recent.push_front(specification);
      picker->thumbnails.emplace(specification, IconPickerThumbnail{std::move(surface), picker->recent.begin()});
      if (picker->recent.size() > cache_limit) {
        picker->thumbnails.erase(picker->recent.back());
        picker->recent.pop_back();
      }
      gtk_widget_queue_draw(group->grid);
      ++loaded;
    }
  }
  picker->loading = 0;
  return G_SOURCE_REMOVE;
}

static void schedule_thumbnails(ConnectionIconPicker *picker) {
  if (!picker->destroyed && !picker->loading)
    picker->loading = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, load_visible_thumbnails, picker, nullptr);
}

static std::string search_text(std::string value) {
  for (auto &character : value)
    character = character == '_' || character == '-' ? ' ' :
        static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
  return value;
}

static void update_icons(ConnectionIconPicker *picker) {
  const auto query = search_text(gtk_entry_get_text(GTK_ENTRY(picker->search)));
  // Reopening or a delayed search notification must not replace unchanged
  // rows, their keyboard selection, or their accessible objects.
  if (picker->initialized && picker->filtered_query == query) return;
  picker->initialized = true;
  picker->filtered_query = query;
  picker->pending_selection.clear();
  picker->changing_selection = true;
  int total = 0;
  for (const auto &group : picker->groups) {
    auto *model = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING);
    int count = 0;
    for (const auto &preset : connection_icon_presets()) {
      if (preset.family != group->family) continue;
      const auto specification = group->white
          ? "material:white:" + preset.specification.substr(9) : preset.specification;
      if (search_text(preset.name).find(query) == std::string::npos &&
          search_text(specification).find(query) == std::string::npos) continue;
      GtkTreeIter row;
      gtk_list_store_insert_with_values(model, &row, -1,
          0, preset.name.c_str(), 1, specification.c_str(), -1);
      ++count;
    }
    gtk_icon_view_set_model(GTK_ICON_VIEW(group->grid), GTK_TREE_MODEL(model));
    g_object_unref(model);
    gtk_widget_set_visible(group->box, count > 0);
    total += count;
  }
  picker->changing_selection = false;
  gtk_widget_set_visible(picker->empty, total == 0);
  gtk_widget_hide(picker->sticky);
  gtk_adjustment_set_value(picker->adjustment, 0);
  schedule_thumbnails(picker);
}

void sync_connection_icon_picker(GtkWidget *popup, const std::string &specification) {
  auto *picker = static_cast<ConnectionIconPicker *>(
      g_object_get_data(G_OBJECT(popup), "elder-terms-icon-picker"));
  if (!picker) return;
  gtk_entry_set_text(GTK_ENTRY(picker->search), "");
  update_icons(picker);
  picker->pending_selection = specification;
  schedule_thumbnails(picker);
}

static gboolean navigate_group(GtkWidget *widget, GdkEventKey *event, gpointer data) {
  auto *group = static_cast<IconPickerGroup *>(data);
  auto *picker = group->picker;
  if (event->keyval == GDK_KEY_Escape) { gtk_widget_hide(picker->popup); return TRUE; }
  auto *view = GTK_ICON_VIEW(widget);
  GtkTreePath *path = nullptr;
  if (!gtk_icon_view_get_cursor(view, &path, nullptr)) return FALSE;
  const int index = gtk_tree_path_get_indices(path)[0];
  gtk_tree_path_free(path);
  const int columns = gtk_icon_view_get_columns(view);
  const int count = group_count(group);
  const bool backwards = (event->keyval == GDK_KEY_Left && index == 0) ||
      (event->keyval == GDK_KEY_Up && index < columns);
  const bool forwards = (event->keyval == GDK_KEY_Right && index == count - 1) ||
      (event->keyval == GDK_KEY_Down && index + columns >= count);
  if (!backwards && !forwards) return FALSE;
  auto found = std::find_if(picker->groups.begin(), picker->groups.end(),
      [group](const auto &candidate) { return candidate.get() == group; });
  int next = static_cast<int>(found - picker->groups.begin()) + (backwards ? -1 : 1);
  while (next >= 0 && next < static_cast<int>(picker->groups.size())) {
    auto *target = picker->groups[next].get();
    if (group_count(target)) {
      select_item(target, backwards ? group_count(target) - 1 : 0, true);
      return TRUE;
    }
    next += backwards ? -1 : 1;
  }
  return FALSE;
}

static GtkWidget *create_heading(const std::string &title) {
  auto *label = gtk_label_new(title.c_str());
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  gtk_widget_set_margin_start(label, 8);
  gtk_widget_set_margin_end(label, 8);
  gtk_widget_set_margin_top(label, 6);
  gtk_widget_set_margin_bottom(label, 6);
  atk_object_set_role(gtk_widget_get_accessible(label), ATK_ROLE_HEADING);
  return label;
}

static void add_group(ConnectionIconPicker *picker, const std::string &prefix,
                      const char *id, const char *title, ConnectionIconFamily family, bool white) {
  auto owned = std::make_unique<IconPickerGroup>(IconPickerGroup{
      picker, family, white, title, gtk_box_new(GTK_ORIENTATION_VERTICAL, 0),
      create_heading(title), gtk_icon_view_new()});
  auto *group = owned.get();
  picker->groups.push_back(std::move(owned));
  identify(group->box, prefix + "_general_icon_group_" + id);
  identify(group->heading, prefix + "_general_icon_heading_" + id);
  identify(group->grid, prefix + "_general_icon_grid_" + id);
  gtk_box_pack_start(GTK_BOX(picker->content), group->box, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(group->box), group->heading, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(group->box), group->grid, FALSE, FALSE, 0);
  auto *view = GTK_ICON_VIEW(group->grid);
  gtk_icon_view_set_columns(view, 8);
  gtk_icon_view_set_item_width(view, thumbnail_size);
  gtk_icon_view_set_item_padding(view, 4);
  gtk_icon_view_set_margin(view, 4);
  gtk_icon_view_set_column_spacing(view, 2);
  gtk_icon_view_set_row_spacing(view, 2);
  gtk_icon_view_set_selection_mode(view, GTK_SELECTION_SINGLE);
  gtk_icon_view_set_activate_on_single_click(view, TRUE);
  gtk_icon_view_set_tooltip_column(view, 0);
  atk_object_set_name(gtk_widget_get_accessible(group->grid), title);
  auto *image = gtk_cell_renderer_pixbuf_new();
  gtk_cell_renderer_set_fixed_size(image, thumbnail_size, thumbnail_size);
  gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(view), image, FALSE);
  gtk_cell_layout_set_cell_data_func(GTK_CELL_LAYOUT(view), image,
      +[](GtkCellLayout *, GtkCellRenderer *cell, GtkTreeModel *model,
          GtkTreeIter *row, gpointer data) {
        auto *group = static_cast<IconPickerGroup *>(data);
        const auto found = group->picker->thumbnails.find(row_string(model, row, 1));
        g_object_set(cell, "surface", found == group->picker->thumbnails.end()
            ? nullptr : found->second.surface.get(), nullptr);
      }, group, nullptr);
  // GtkIconView exposes renderer text through AtkText even when labels are
  // hidden, allowing screen readers to identify offscreen items as well.
  auto *text = gtk_cell_renderer_text_new();
  g_object_set(text, "visible", FALSE, nullptr);
  gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(view), text, FALSE);
  gtk_cell_layout_add_attribute(GTK_CELL_LAYOUT(view), text, "text", 0);
  g_signal_connect(view, "item-activated", G_CALLBACK(+[](GtkIconView *view, GtkTreePath *path, gpointer data) {
    auto *picker = static_cast<IconPickerGroup *>(data)->picker;
    GtkTreeIter row;
    auto *model = gtk_icon_view_get_model(view);
    if (!gtk_tree_model_get_iter(model, &row, path)) return;
    const auto specification = row_string(model, &row, 1);
    auto callback = picker->selected;
    gtk_widget_hide(picker->popup);
    callback(specification);
  }), group);
  g_signal_connect(view, "selection-changed", G_CALLBACK(+[](GtkIconView *view, gpointer data) {
    auto *group = static_cast<IconPickerGroup *>(data);
    auto *picker = group->picker;
    if (picker->changing_selection) return;
    auto *items = gtk_icon_view_get_selected_items(view);
    if (!items) return;
    picker->changing_selection = true;
    for (const auto &other : picker->groups)
      if (other.get() != group) gtk_icon_view_unselect_all(GTK_ICON_VIEW(other->grid));
    reveal_item(group, static_cast<GtkTreePath *>(items->data));
    g_list_free_full(items, reinterpret_cast<GDestroyNotify>(gtk_tree_path_free));
    picker->changing_selection = false;
  }), group);
  g_signal_connect(view, "key-press-event", G_CALLBACK(navigate_group), group);
  g_signal_connect(view, "size-allocate", G_CALLBACK(+[](GtkWidget *, GtkAllocation *, gpointer data) {
    schedule_thumbnails(static_cast<IconPickerGroup *>(data)->picker);
  }), group);
  gtk_widget_show_all(group->box);
  gtk_widget_set_no_show_all(group->box, TRUE);
}

GtkWidget *create_connection_icon_picker(GtkWidget *relative_to,
    const std::string &prefix, std::function<void(std::string)> selected,
    std::function<void()> choose_file) {
  auto *picker = new ConnectionIconPicker{};
  picker->popup = gtk_popover_new(relative_to);
  g_object_set_data_full(G_OBJECT(picker->popup), "elder-terms-icon-picker", picker, +[](gpointer data) {
    delete static_cast<ConnectionIconPicker *>(data);
  });
  picker->search = gtk_search_entry_new();
  picker->empty = gtk_label_new(_("No matching icons"));
  picker->scroll = gtk_scrolled_window_new(nullptr, nullptr);
  picker->viewport = gtk_viewport_new(nullptr, nullptr);
  picker->content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  picker->overlay = gtk_overlay_new();
  picker->sticky = gtk_event_box_new();
  picker->sticky_label = create_heading("");
  picker->adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(picker->scroll));
  picker->selected = std::move(selected);
  picker->choose_file = std::move(choose_file);
  picker->scale = gtk_widget_get_scale_factor(relative_to);
  identify(picker->popup, prefix + "_general_icon_picker");
  identify(picker->content, prefix + "_general_icon_grid");
  identify(picker->search, prefix + "_general_icon_search");
  identify(picker->empty, prefix + "_general_icon_empty");
  identify(picker->scroll, prefix + "_general_icon_scroll");
  identify(picker->sticky_label, prefix + "_general_icon_current_group");
  auto *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_container_set_border_width(GTK_CONTAINER(box), 8);
  gtk_container_add(GTK_CONTAINER(picker->popup), box);
  gtk_entry_set_placeholder_text(GTK_ENTRY(picker->search), _("Search icons…"));
  atk_object_set_name(gtk_widget_get_accessible(picker->search), _("Search icons…"));
  gtk_box_pack_start(GTK_BOX(box), picker->search, FALSE, FALSE, 0);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(picker->scroll), GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);
  identify(gtk_scrolled_window_get_vscrollbar(GTK_SCROLLED_WINDOW(picker->scroll)),
      prefix + "_general_icon_scrollbar");
  gtk_widget_set_size_request(picker->scroll, 432, 300);
  gtk_viewport_set_shadow_type(GTK_VIEWPORT(picker->viewport), GTK_SHADOW_NONE);
  gtk_container_add(GTK_CONTAINER(picker->viewport), picker->content);
  gtk_container_add(GTK_CONTAINER(picker->scroll), picker->viewport);
  gtk_container_add(GTK_CONTAINER(picker->overlay), picker->scroll);
  gtk_container_add(GTK_CONTAINER(picker->sticky), picker->sticky_label);
  gtk_style_context_add_class(gtk_widget_get_style_context(picker->sticky), GTK_STYLE_CLASS_BACKGROUND);
  gtk_overlay_add_overlay(GTK_OVERLAY(picker->overlay), picker->sticky);
  gtk_widget_set_halign(picker->sticky, GTK_ALIGN_FILL);
  gtk_widget_set_valign(picker->sticky, GTK_ALIGN_START);
  gtk_box_pack_start(GTK_BOX(box), picker->overlay, TRUE, TRUE, 0);
  gtk_widget_show_all(picker->sticky);
  gtk_widget_set_no_show_all(picker->sticky, TRUE);
  gtk_widget_hide(picker->sticky);
  g_signal_connect(picker->overlay, "get-child-position", G_CALLBACK(+[](GtkOverlay *, GtkWidget *, GdkRectangle *allocation, gpointer data) -> gboolean {
    auto *picker = static_cast<ConnectionIconPicker *>(data);
    int height = 0;
    gtk_widget_get_preferred_height(picker->sticky, nullptr, &height);
    allocation->x = 0;
    allocation->y = picker->sticky_y;
    allocation->width = gtk_widget_get_allocated_width(picker->viewport);
    allocation->height = height;
    return TRUE;
  }), picker);
  add_group(picker, prefix, "emoji", _("Emoji"), ConnectionIconFamily::Noto, false);
  add_group(picker, prefix, "material_black", _("Material (Black)"), ConnectionIconFamily::Material, false);
  add_group(picker, prefix, "material_white", _("Material (White)"), ConnectionIconFamily::Material, true);
  gtk_widget_set_no_show_all(picker->empty, TRUE);
  gtk_box_pack_start(GTK_BOX(box), picker->empty, FALSE, FALSE, 0);
  auto *file = gtk_button_new_with_label(_("Choose image file…"));
  identify(file, prefix + "_general_icon_file");
  gtk_box_pack_start(GTK_BOX(box), file, FALSE, FALSE, 0);
  g_signal_connect(file, "clicked", G_CALLBACK(+[](GtkButton *, gpointer data) {
    auto *picker = static_cast<ConnectionIconPicker *>(data);
    auto callback = picker->choose_file;
    gtk_widget_hide(picker->popup);
    callback();
  }), picker);
  g_signal_connect(picker->search, "search-changed", G_CALLBACK(+[](GtkSearchEntry *, gpointer data) {
    update_icons(static_cast<ConnectionIconPicker *>(data));
  }), picker);
  g_signal_connect(picker->search, "activate", G_CALLBACK(+[](GtkEntry *, gpointer data) {
    auto *picker = static_cast<ConnectionIconPicker *>(data);
    // Enter may precede the delayed search-changed signal. Resolve the current
    // text before activating a result so the previous query cannot be chosen.
    update_icons(picker);
    for (const auto &group : picker->groups) {
      if (!group_count(group.get())) continue;
      auto *first = gtk_tree_path_new_from_indices(0, -1);
      gtk_icon_view_item_activated(GTK_ICON_VIEW(group->grid), first);
      gtk_tree_path_free(first);
      break;
    }
  }), picker);
  g_signal_connect(picker->search, "stop-search", G_CALLBACK(+[](GtkSearchEntry *, gpointer data) {
    gtk_widget_hide(static_cast<ConnectionIconPicker *>(data)->popup);
  }), picker);
  g_signal_connect(picker->popup, "map", G_CALLBACK(+[](GtkWidget *, gpointer data) {
    auto *picker = static_cast<ConnectionIconPicker *>(data);
    gtk_widget_grab_focus(picker->search);
    schedule_thumbnails(picker);
  }), picker);
  g_signal_connect(picker->viewport, "size-allocate", G_CALLBACK(+[](GtkWidget *widget, GtkAllocation *, gpointer data) {
    auto *picker = static_cast<ConnectionIconPicker *>(data);
    const int columns = std::max(1, (gtk_widget_get_allocated_width(widget) - 6) / 50);
    for (const auto &group : picker->groups)
      gtk_icon_view_set_columns(GTK_ICON_VIEW(group->grid), columns);
    schedule_thumbnails(picker);
  }), picker);
  g_signal_connect(picker->popup, "notify::scale-factor", G_CALLBACK(+[](GObject *, GParamSpec *, gpointer data) {
    auto *picker = static_cast<ConnectionIconPicker *>(data);
    picker->scale = gtk_widget_get_scale_factor(picker->popup);
    picker->thumbnails.clear();
    picker->recent.clear();
    schedule_thumbnails(picker);
  }), picker);
  g_signal_connect(picker->adjustment, "value-changed", G_CALLBACK(+[](GtkAdjustment *, gpointer data) {
    auto *picker = static_cast<ConnectionIconPicker *>(data);
    update_heading(picker);
    schedule_thumbnails(picker);
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
