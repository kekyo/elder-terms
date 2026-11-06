// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include <elder-terms/settings-preferences-widget.h>
#include <elder-terms/application-settings-widget.h>
#include <elder-terms/settings-widget.h>
#include <algorithm>
#include <memory>
#include "settings-validation.h"

#define GETTEXT_PACKAGE "elder-terms"
#include <glib/gi18n-lib.h>

namespace elder_terms {

static void assign_accessible_id(GtkWidget *widget, const char *id) {
  gtk_widget_set_name(widget, id);
  atk_object_set_accessible_id(gtk_widget_get_accessible(widget), id);
}

struct ThemeEditor {
  SettingsTheme theme;
  SettingsWidgetState *widget = nullptr;
  GtkWidget *row = nullptr;
  GtkWidget *label = nullptr;
  GtkWidget *page = nullptr;
  GtkWidget *preview = nullptr;
};

struct SettingsPreferencesWidget {
  SettingsPreferencesOptions options;
  ApplicationSettingsWidgetState *application = nullptr;
  SettingsWidgetState *defaults = nullptr;
  std::vector<std::unique_ptr<ThemeEditor>> themes;
  ThemeEditor *selected = nullptr;
  GtkWidget *root = nullptr;
  GtkWidget *list = nullptr;
  GtkWidget *stack = nullptr;
  GtkWidget *name = nullptr;
  GtkWidget *references = nullptr;
  GtkWidget *remove = nullptr;
  GtkWidget *duplicate = nullptr;
  GtkWidget *message = nullptr;
  bool synchronizing = true;
  unsigned int next_editor_id = 0;
};

static SettingsStore global_draft(const SettingsPreferencesWidget *state) {
  auto global = settings_widget_draft_store(state->defaults);
  const auto application = application_settings_widget_draft_store(state->application);
  for (auto &entry : global.entries) {
    const auto definitions = application_setting_definitions();
    const bool is_application = std::any_of(definitions.begin(), definitions.end(), [&entry](const auto &definition) {
      return definition.key.section == entry.definition.key.section && definition.key.name == entry.definition.key.name;
    });
    if (!is_application) continue;
    for (const auto &value : application.entries)
      if (value.definition.key.section == entry.definition.key.section && value.definition.key.name == entry.definition.key.name) entry = value;
  }
  return global;
}

static SettingsThemeCatalog theme_draft(const SettingsPreferencesWidget *state) {
  SettingsThemeCatalog catalog;
  for (const auto &editor : state->themes) {
    auto theme = editor->theme;
    theme.store = settings_widget_draft_store(editor->widget);
    catalog.themes.push_back(std::move(theme));
  }
  return catalog;
}

static std::string lines(const std::vector<std::string> &messages) {
  std::string text;
  for (const auto &message : messages) {
    if (!text.empty()) text += "\n";
    text += message;
  }
  return text;
}

static void refresh_theme_reference_display(SettingsPreferencesWidget *state) {
  const auto names = state->selected && state->options.references
      ? state->options.references(state->selected->theme.id) : std::vector<std::string>();
  gtk_label_set_text(GTK_LABEL(state->references), names.empty() ? _("No connections use this theme") : lines(names).c_str());
  gtk_widget_set_sensitive(state->remove, state->selected && names.empty());
  gtk_widget_set_sensitive(state->duplicate, state->selected != nullptr);
}

static void changed(SettingsPreferencesWidget *state) {
  if (state->synchronizing) return;
  const auto errors = validate_settings_themes(theme_draft(state));
  set_settings_input_error(state->name, lines(errors));
  refresh_theme_reference_display(state);
  if (state->selected) gtk_widget_queue_draw(state->selected->preview);
  if (state->options.changed) state->options.changed();
}

static gboolean draw_preview(GtkWidget *widget, cairo_t *cr, gpointer data) {
  const auto *editor = static_cast<ThemeEditor *>(data);
  const auto colors = general_color_settings(settings_widget_draft_store(editor->widget));
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  const auto fill = [cr](const std::optional<RgbColor> &color, double fallback) {
    if (color) cairo_set_source_rgb(cr, color->red / 255.0, color->green / 255.0, color->blue / 255.0);
    else cairo_set_source_rgb(cr, fallback, fallback, fallback);
  };
  fill(colors.background, 0.0);
  cairo_paint(cr);
  fill(colors.exterior_background, 0.25);
  cairo_rectangle(cr, 0, 0, allocation.width, 28);
  cairo_fill(cr);
  cairo_set_source_rgb(cr, 1, 1, 1);
  auto *layout = gtk_widget_create_pango_layout(widget, _("Theme preview"));
  cairo_move_to(cr, 10, 6);
  pango_cairo_show_layout(cr, layout);
  pango_layout_set_text(layout, "AaBb 0123456789", -1);
  cairo_move_to(cr, 10, 43);
  pango_cairo_show_layout(cr, layout);
  g_object_unref(layout);
  return FALSE;
}

static void select_theme(GtkListBox *, GtkListBoxRow *row, gpointer data) {
  auto *state = static_cast<SettingsPreferencesWidget *>(data);
  state->selected = nullptr;
  for (const auto &editor : state->themes) {
    if (editor->row == GTK_WIDGET(row)) state->selected = editor.get();
  }
  state->synchronizing = true;
  gtk_widget_set_sensitive(state->name, state->selected != nullptr);
  gtk_entry_set_text(GTK_ENTRY(state->name), state->selected ? state->selected->theme.name.c_str() : "");
  if (state->selected) gtk_stack_set_visible_child(GTK_STACK(state->stack), state->selected->page);
  state->synchronizing = false;
  changed(state);
}

static void add_theme(SettingsPreferencesWidget *state, SettingsTheme theme) {
  auto editor = std::make_unique<ThemeEditor>();
  auto *item = editor.get();
  item->theme = std::move(theme);
  item->page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  item->preview = gtk_drawing_area_new();
  gtk_widget_set_size_request(item->preview, 300, 76);
  const auto prefix = "theme_" + std::to_string(state->next_editor_id++);
  assign_accessible_id(item->preview, (prefix + "_preview").c_str());
  g_signal_connect(item->preview, "draw", G_CALLBACK(draw_preview), item);
  gtk_box_pack_start(GTK_BOX(item->page), item->preview, FALSE, FALSE, 0);
  item->widget = create_settings_widget({.store = item->theme.store, .show_actions = false,
      .mode = SettingsWidgetMode::named_theme, .id_prefix = prefix,
      .callbacks = {.apply = {}, .save = {}, .cancel = {}, .changed = [state, item] {
        gtk_widget_queue_draw(item->preview);
        changed(state);
      }}});
  auto *scroll = gtk_scrolled_window_new(nullptr, nullptr);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), FALSE);
  gtk_container_add(GTK_CONTAINER(scroll), settings_widget_root(item->widget));
  gtk_box_pack_start(GTK_BOX(item->page), scroll, TRUE, TRUE, 0);
  gtk_stack_add_named(GTK_STACK(state->stack), item->page, item->theme.id.c_str());
  item->row = gtk_list_box_row_new();
  item->label = gtk_label_new(item->theme.name.c_str());
  gtk_label_set_xalign(GTK_LABEL(item->label), 0);
  gtk_widget_set_margin_start(item->label, 8);
  gtk_widget_set_margin_end(item->label, 8);
  gtk_widget_set_margin_top(item->label, 6);
  gtk_widget_set_margin_bottom(item->label, 6);
  gtk_container_add(GTK_CONTAINER(item->row), item->label);
  state->themes.push_back(std::move(editor));
  gtk_container_add(GTK_CONTAINER(state->list), item->row);
  gtk_widget_show_all(item->row);
  gtk_widget_show_all(item->page);
}

static std::string unique_theme_name(const SettingsPreferencesWidget *state, const std::string &base) {
  auto name = base;
  unsigned int number = 2;
  while (std::any_of(state->themes.begin(), state->themes.end(), [&name](const auto &editor) { return editor->theme.name == name; }))
    name = base + " " + std::to_string(number++);
  return name;
}

static void new_theme(SettingsPreferencesWidget *state, bool duplicate) {
  gchar *id = g_uuid_string_random();
  SettingsTheme theme{.id = id, .name = unique_theme_name(state, duplicate && state->selected ? state->selected->theme.name : _("New theme")),
      .store = duplicate && state->selected ? settings_widget_draft_store(state->selected->widget) : create_theme_settings(global_draft(state))};
  g_free(id);
  add_theme(state, std::move(theme));
  gtk_list_box_select_row(GTK_LIST_BOX(state->list), GTK_LIST_BOX_ROW(state->themes.back()->row));
  changed(state);
}

static GtkWidget *create_themes_page(SettingsPreferencesWidget *state) {
  auto *page = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  auto *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  state->list = gtk_list_box_new();
  assign_accessible_id(state->list, "themes_list");
  gtk_widget_set_size_request(left, 180, -1);
  auto *scroll = gtk_scrolled_window_new(nullptr, nullptr);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_container_add(GTK_CONTAINER(scroll), state->list);
  gtk_box_pack_start(GTK_BOX(left), scroll, TRUE, TRUE, 0);
  auto *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  auto *add = gtk_button_new_with_label(_("New"));
  state->duplicate = gtk_button_new_with_label(_("Duplicate"));
  state->remove = gtk_button_new_with_label(_("Delete"));
  assign_accessible_id(add, "themes_new_button");
  assign_accessible_id(state->duplicate, "themes_duplicate_button");
  assign_accessible_id(state->remove, "themes_delete_button");
  for (auto *button : {add, state->duplicate, state->remove}) gtk_box_pack_start(GTK_BOX(buttons), button, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(left), buttons, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(page), left, FALSE, FALSE, 0);
  auto *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  state->name = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(state->name), _("Theme name"));
  assign_accessible_id(state->name, "themes_name_entry");
  gtk_box_pack_start(GTK_BOX(right), state->name, FALSE, FALSE, 0);
  state->references = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(state->references), 0);
  gtk_label_set_line_wrap(GTK_LABEL(state->references), TRUE);
  assign_accessible_id(state->references, "themes_references");
  gtk_box_pack_start(GTK_BOX(right), state->references, FALSE, FALSE, 0);
  state->stack = gtk_stack_new();
  gtk_stack_set_transition_type(GTK_STACK(state->stack), GTK_STACK_TRANSITION_TYPE_NONE);
  gtk_box_pack_start(GTK_BOX(right), state->stack, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(page), right, TRUE, TRUE, 0);
  g_signal_connect(state->list, "row-selected", G_CALLBACK(select_theme), state);
  g_signal_connect(state->name, "changed", G_CALLBACK(+[](GtkEntry *entry, gpointer data) {
    auto *state = static_cast<SettingsPreferencesWidget *>(data);
    if (state->synchronizing || !state->selected) return;
    state->selected->theme.name = gtk_entry_get_text(entry);
    gtk_label_set_text(GTK_LABEL(state->selected->label), state->selected->theme.name.c_str());
    changed(state);
  }), state);
  g_signal_connect(add, "clicked", G_CALLBACK(+[](GtkButton *, gpointer data) { new_theme(static_cast<SettingsPreferencesWidget *>(data), false); }), state);
  g_signal_connect(state->duplicate, "clicked", G_CALLBACK(+[](GtkButton *, gpointer data) { new_theme(static_cast<SettingsPreferencesWidget *>(data), true); }), state);
  g_signal_connect(state->remove, "clicked", G_CALLBACK(+[](GtkButton *, gpointer data) {
    auto *state = static_cast<SettingsPreferencesWidget *>(data);
    if (!state->selected || (state->options.references && !state->options.references(state->selected->theme.id).empty())) return;
    auto *selected = state->selected;
    state->selected = nullptr;
    // Remove the row only after dropping its editor from lookup; GTK emits selection changes synchronously.
    const auto found = std::find_if(state->themes.begin(), state->themes.end(), [selected](const auto &value) { return value.get() == selected; });
    auto removed = std::move(*found);
    state->themes.erase(found);
    gtk_widget_destroy(removed->row);
    gtk_widget_destroy(removed->page);
    destroy_settings_widget(removed->widget);
    if (!state->themes.empty()) gtk_list_box_select_row(GTK_LIST_BOX(state->list), GTK_LIST_BOX_ROW(state->themes.front()->row));
    changed(state);
  }), state);
  return page;
}

SettingsPreferencesWidget *create_settings_preferences_widget(SettingsPreferencesOptions options) {
  auto *state = new SettingsPreferencesWidget;
  state->options = std::move(options);
  state->root = gtk_notebook_new();
  assign_accessible_id(state->root, "application_dialog_notebook");
  state->application = create_application_settings_widget({.store = state->options.session.global,
      .id_prefix = "application_settings", .new_window_connections = state->options.new_window_connections,
      .changed = [state] { changed(state); }});
  state->defaults = create_settings_widget({.store = state->options.session.global, .show_actions = false,
      .mode = SettingsWidgetMode::global_defaults, .id_prefix = "global_settings",
      .callbacks = {.apply = {}, .save = {}, .cancel = {}, .changed = [state] {
        if (state->synchronizing) return;
        state->synchronizing = true;
        const auto global = global_draft(state);
        for (const auto &theme : state->themes) {
          settings_widget_rebase_fallbacks(theme->widget, global);
          gtk_widget_queue_draw(theme->preview);
        }
        state->synchronizing = false;
        changed(state);
      }}});
  gtk_notebook_append_page(GTK_NOTEBOOK(state->root), application_settings_widget_root(state->application), gtk_label_new(_("Application")));
  auto *defaults_tab = gtk_label_new(_("Connection defaults"));
  assign_accessible_id(defaults_tab, "connection_defaults_tab");
  gtk_notebook_append_page(GTK_NOTEBOOK(state->root), settings_widget_root(state->defaults), defaults_tab);
  auto *theme_page = create_themes_page(state);
  gtk_notebook_append_page(GTK_NOTEBOOK(state->root), theme_page, gtk_label_new(_("Themes")));
  state->message = gtk_label_new(lines(state->options.session.warnings).c_str());
  g_object_ref_sink(state->message);
  gtk_label_set_line_wrap(GTK_LABEL(state->message), TRUE);
  gtk_label_set_max_width_chars(GTK_LABEL(state->message), 90);
  gtk_label_set_xalign(GTK_LABEL(state->message), 0);
  assign_accessible_id(state->message, "settings_save_status");
  gtk_widget_set_no_show_all(state->message, TRUE);
  gtk_widget_set_visible(state->message, !state->options.session.warnings.empty());
  for (const auto &theme : state->options.session.catalog.themes) add_theme(state, theme);
  gtk_widget_show_all(state->root);
  state->synchronizing = false;
  if (!state->themes.empty()) gtk_list_box_select_row(GTK_LIST_BOX(state->list), GTK_LIST_BOX_ROW(state->themes.front()->row));
  else select_theme(nullptr, nullptr, state);
  return state;
}

GtkWidget *settings_preferences_widget_root(SettingsPreferencesWidget *state) { return state->root; }
GtkWidget *settings_preferences_widget_status(SettingsPreferencesWidget *state) { return state->message; }

bool settings_preferences_widget_is_valid(const SettingsPreferencesWidget *state) {
  if (!state || !state->options.session.loaded || !application_settings_widget_is_valid(state->application) ||
      !settings_widget_is_valid(state->defaults) || !validate_settings_themes(theme_draft(state)).empty()) return false;
  return std::all_of(state->themes.begin(), state->themes.end(), [](const auto &theme) { return settings_widget_is_valid(theme->widget); });
}

bool settings_preferences_widget_is_dirty(const SettingsPreferencesWidget *state) {
  return state && (!explicit_settings_equal(global_draft(state), state->options.session.global) ||
      !settings_themes_equal(theme_draft(state), state->options.session.catalog) ||
      !settings_preferences_widget_is_valid(state));
}

SettingsEditResult settings_preferences_widget_save(SettingsPreferencesWidget *state) {
  if (!settings_preferences_widget_is_valid(state)) return {};
  auto result = save_settings_edit(&state->options.session, global_draft(state), theme_draft(state));
  const auto status = [](SettingsFileSaveStatus value) {
    return value == SettingsFileSaveStatus::saved ? _("Saved") : value == SettingsFileSaveStatus::failed ? _("Not saved") : _("Unchanged");
  };
  const auto text = std::string("global.ini: ") + status(result.global) + "\ntheme.ini: " + status(result.themes) +
      (result.warnings.empty() ? "" : "\n" + lines(result.warnings));
  gtk_label_set_text(GTK_LABEL(state->message), text.c_str());
  gtk_widget_show(state->message);
  changed(state);
  return result;
}

SettingsStore settings_preferences_widget_saved_global(const SettingsPreferencesWidget *state) { return state->options.session.global; }

std::shared_ptr<const SettingsThemeContext> settings_preferences_widget_saved_context(const SettingsPreferencesWidget *state) {
  return std::make_shared<SettingsThemeContext>(SettingsThemeContext{.global = state->options.session.global,
      .catalog = state->options.session.catalog, .global_path = state->options.session.global_file.path,
      .theme_path = state->options.session.theme_file.path});
}

void destroy_settings_preferences_widget(SettingsPreferencesWidget *state) {
  if (!state) return;
  destroy_application_settings_widget(state->application);
  destroy_settings_widget(state->defaults);
  for (const auto &theme : state->themes) destroy_settings_widget(theme->widget);
  gtk_widget_destroy(state->message);
  g_object_unref(state->message);
  delete state;
}

}
