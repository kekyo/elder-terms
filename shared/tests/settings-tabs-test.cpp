// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include <elder-terms/settings-widget.h>

#include <elder-terms/settings/general-settings.h>
#include <elder-terms/settings/terminal-settings.h>

#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace elder_terms;

static void require(bool condition, const char *message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

static GtkWidget *find_widget(GtkWidget *parent, const char *name) {
  if (std::strcmp(gtk_widget_get_name(parent), name) == 0) {
    return parent;
  }
  if (!GTK_IS_CONTAINER(parent)) {
    return nullptr;
  }
  GList *children = gtk_container_get_children(GTK_CONTAINER(parent));
  GtkWidget *found = nullptr;
  for (GList *item = children; item != nullptr && found == nullptr; item = item->next) {
    found = find_widget(GTK_WIDGET(item->data), name);
  }
  g_list_free(children);
  return found;
}

static int count_widgets(GtkWidget *parent, const char *name) {
  int count = std::strcmp(gtk_widget_get_name(parent), name) == 0 ? 1 : 0;
  if (GTK_IS_CONTAINER(parent)) {
    GList *children = gtk_container_get_children(GTK_CONTAINER(parent));
    for (GList *item = children; item != nullptr; item = item->next) {
      count += count_widgets(GTK_WIDGET(item->data), name);
    }
    g_list_free(children);
  }
  return count;
}

static bool belongs_to(GtkWidget *widget, GtkWidget *page) {
  for (GtkWidget *current = widget; current != nullptr;
       current = gtk_widget_get_parent(current)) {
    if (current == page) {
      return true;
    }
  }
  return false;
}

struct Fixture {
  SettingsWidgetState *state;
  GtkWidget *window;
  GtkWidget *root;
  std::string prefix;
};

static Fixture create_fixture(const char *type, SettingsWidgetMode mode,
                              SettingsWidgetCallbacks callbacks = {}) {
  SettingsWidgetOptions options;
  options.store = create_default_settings(default_terminal_display_settings(12),
                                          "Tab test");
  set_explicit_setting_value(&options.store, general_type_setting_key(),
                             SettingValue{std::string(type)});
  options.mode = mode;
  options.id_prefix = mode == SettingsWidgetMode::global_defaults
                          ? "global_settings" : "settings";
  options.callbacks = std::move(callbacks);
  const std::string prefix = options.id_prefix;
  SettingsWidgetState *state = create_settings_widget(std::move(options));
  GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  GtkWidget *root = settings_widget_root(state);
  gtk_container_add(GTK_CONTAINER(window), root);
  g_signal_connect(window, "destroy", G_CALLBACK(+[](GtkWidget *, gpointer data) {
    destroy_settings_widget(static_cast<SettingsWidgetState *>(data));
  }), state);
  gtk_widget_show_all(window);
  return {state, window, root, prefix};
}

static void destroy_fixture(Fixture *fixture) {
  gtk_widget_destroy(fixture->window);
}

static void verify_tab_structure(const Fixture &fixture, bool terminal_visible) {
  const auto id = [&fixture](const char *suffix) {
    return fixture.prefix + "_" + suffix;
  };
  auto *behavior = find_widget(fixture.root, id("terminal_page").c_str());
  auto *appearance = find_widget(fixture.root, id("terminal_appearance_page").c_str());
  auto *notebook = find_widget(fixture.root, id("notebook").c_str());
  auto *behavior_tab = behavior && notebook
                           ? gtk_notebook_get_tab_label(GTK_NOTEBOOK(notebook), behavior)
                           : nullptr;
  auto *appearance_tab = appearance && notebook
                             ? gtk_notebook_get_tab_label(GTK_NOTEBOOK(notebook), appearance)
                             : nullptr;
  require(behavior && appearance && behavior_tab && appearance_tab && notebook,
          "Terminal tabs are missing");
  require(std::strcmp(gtk_widget_get_name(behavior_tab), id("terminal_tab").c_str()) == 0 &&
              std::strcmp(gtk_widget_get_name(appearance_tab),
                          id("terminal_appearance_tab").c_str()) == 0,
          "Terminal tab identities differ");
  require(gtk_widget_get_visible(behavior) == terminal_visible &&
              gtk_widget_get_visible(appearance) == terminal_visible &&
              gtk_widget_get_visible(behavior_tab) == terminal_visible &&
              gtk_widget_get_visible(appearance_tab) == terminal_visible,
          "Terminal tab visibility differs from connection type");
  require(gtk_notebook_page_num(GTK_NOTEBOOK(notebook), appearance) ==
              gtk_notebook_page_num(GTK_NOTEBOOK(notebook), behavior) + 1,
          "Terminal appearance does not follow Terminal");
  require(std::strcmp(gtk_label_get_text(GTK_LABEL(appearance_tab)),
                      "Terminal appearance") == 0,
          "Terminal appearance tab has the wrong title");

  auto *general = find_widget(fixture.root, id("general_page").c_str());
  require(general != nullptr && gtk_widget_get_visible(general),
          "General tab is missing or hidden");
  for (const char *field : {"general_show_border_combo", "general_border_width_entry"}) {
    const auto name = id(field);
    auto *widget = find_widget(fixture.root, name.c_str());
    require(widget != nullptr && count_widgets(fixture.root, name.c_str()) == 1 &&
                belongs_to(widget, general) && gtk_widget_get_visible(widget),
            "Border field is missing, duplicated, hidden, or outside General");
  }

  constexpr std::array behavior_fields{
      "terminal_encoding_combo", "terminal_backspace_code_combo",
      "terminal_cursor_key_mode_combo", "terminal_return_code_combo",
      "terminal_width_entry", "terminal_height_entry",
      "terminal_scrollback_lines_entry", "terminal_bell_sound_entry",
      "terminal_zoom_in_key_entry", "terminal_zoom_out_key_entry",
      "terminal_send_break_key_entry", "terminal_accessibility_enabled_combo",
      "terminal_block_copy_trim_trailing_spaces_combo"};
  constexpr std::array appearance_fields{
      "terminal_fonts_mode_combo", "terminal_font_size_dip_entry",
      "terminal_cursor_shape_combo", "terminal_cursor_blink_combo",
      "terminal_background_mode_combo", "terminal_background_opacity_entry",
      "terminal_background_tint_color_entry", "terminal_background_tint_opacity_entry",
      "terminal_background_image_entry", "terminal_background_image_opacity_entry",
      "terminal_background_image_fit_combo", "terminal_color_filter_combo",
      "terminal_color_vision_combo", "terminal_color_vision_strength_entry",
      "terminal_color_preview"};
  for (const char *field : behavior_fields) {
    const std::string name = id(field);
    GtkWidget *widget = find_widget(fixture.root, name.c_str());
    require(widget != nullptr && count_widgets(fixture.root, name.c_str()) == 1 &&
                belongs_to(widget, behavior) && !belongs_to(widget, appearance),
            "Behavior field is missing, duplicated, or on the wrong tab");
  }
  for (const char *field : appearance_fields) {
    const std::string name = id(field);
    GtkWidget *widget = find_widget(fixture.root, name.c_str());
    require(widget != nullptr && count_widgets(fixture.root, name.c_str()) == 1 &&
                belongs_to(widget, appearance) && !belongs_to(widget, behavior),
            "Appearance field is missing, duplicated, or on the wrong tab");
  }
  auto *scrollbar = gtk_scrolled_window_get_vscrollbar(
      GTK_SCROLLED_WINDOW(appearance));
  require(scrollbar != nullptr &&
              std::strcmp(gtk_widget_get_name(scrollbar),
                          id("terminal_appearance_page_scrollbar").c_str()) == 0 &&
              belongs_to(scrollbar, appearance),
          "Font list scroll surface is not on the appearance tab");
}

static void verify_actions() {
  int applied = 0;
  int saved = 0;
  int cancelled = 0;
  SettingsWidgetCallbacks callbacks;
  callbacks.apply = [&](const SettingsStore &store, SettingsWidgetAction action) {
    const auto display = terminal_display_settings(store);
    require(display.width == 88 && display.font_size_dip == 15,
            "Apply did not include edits from both tabs");
    ++applied;
    action.complete(true, {});
  };
  callbacks.save = [&](const SettingsStore &store, SettingsWidgetAction action) {
    const auto display = terminal_display_settings(store);
    require(display.width == 89 && display.font_size_dip == 16,
            "Save did not include edits from both tabs");
    ++saved;
    action.complete(true, {});
  };
  callbacks.cancel = [&] { ++cancelled; };
  Fixture fixture = create_fixture("local", SettingsWidgetMode::connection,
                                   std::move(callbacks));
  verify_tab_structure(fixture, true);
  auto *width = GTK_ENTRY(find_widget(fixture.root, "settings_terminal_width_entry"));
  auto *font_size = GTK_ENTRY(find_widget(fixture.root,
                                         "settings_terminal_font_size_dip_entry"));
  gtk_entry_set_text(width, "88");
  gtk_entry_set_text(font_size, "15");
  gtk_button_clicked(GTK_BUTTON(find_widget(fixture.root, "settings_apply_button")));
  require(applied == 1,
          "Apply did not accept the two-tab draft");
  gtk_entry_set_text(width, "89");
  gtk_entry_set_text(font_size, "16");
  gtk_button_clicked(GTK_BUTTON(find_widget(fixture.root, "settings_save_button")));
  require(saved == 1,
          "Save did not accept the two-tab draft");
  gtk_entry_set_text(width, "90");
  gtk_entry_set_text(font_size, "17");
  gtk_button_clicked(GTK_BUTTON(find_widget(fixture.root, "settings_cancel_button")));
  const auto restored = terminal_display_settings(settings_widget_draft_store(fixture.state));
  require(cancelled == 1 && restored.width == 89 && restored.font_size_dip == 16,
          "Cancel did not restore the saved values from both tabs");
  destroy_fixture(&fixture);
}

int main(int argc, char **argv) {
  try {
    require(gtk_init_check(&argc, &argv), "GTK initialization failed");
    verify_actions();
    for (const char *type : {"sftp", "ftp", "webdav"}) {
      Fixture transfer = create_fixture(type, SettingsWidgetMode::connection);
      verify_tab_structure(transfer, false);
      destroy_fixture(&transfer);
    }
    Fixture defaults = create_fixture("sftp", SettingsWidgetMode::global_defaults);
    verify_tab_structure(defaults, true);
    destroy_fixture(&defaults);
  } catch (const std::exception &exception) {
    std::cerr << exception.what() << '\n';
    return 1;
  }
  return 0;
}
