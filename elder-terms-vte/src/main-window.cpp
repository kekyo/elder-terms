#include <algorithm>
#include <array>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

#include <gdk/gdk.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gestament/gtk.h>
#include <vte/vte.h>

#include "main-window.h"
#include "widget-background.h"

namespace elder_terms {

static constexpr int indicator_icon_pixel_size = 18;
static constexpr guint transfer_progress_pulse_period_ms = 120;
static constexpr const char *disconnected_title_suffix = " (Disconnected)";
static constexpr const char *terminal_dim_overlay_style_class =
    "terminal-dim-overlay";
static constexpr const char *disconnected_notice_background_style_class =
    "disconnected-notice-background";
static constexpr const char *disconnected_notice_label_style_class =
    "disconnected-notice-label";
static constexpr const char *transfer_progress_notice_background_style_class =
    "transfer-progress-notice-background";
static constexpr const char *transfer_progress_notice_label_style_class =
    "transfer-progress-notice-label";
static constexpr const char *settings_exterior_background_style_class =
    "settings-exterior-background";
static constexpr const char *settings_background_style_class =
    "settings-background";
static constexpr const char *gtk_color_button_style_class = "color";
static constexpr const char *ssh_prompt_background_style_class =
    "ssh-prompt-background";
static constexpr const char *ssh_prompt_title_style_class =
    "ssh-prompt-title";
static constexpr const char *ssh_prompt_message_style_class =
    "ssh-prompt-message";
static constexpr const char *main_window_css =
    ".terminal-dim-overlay {"
    "  background-color: rgba(0, 0, 0, 0.35);"
    "}"
    ".disconnected-notice-background {"
    "  background-color: rgba(64, 64, 64, 0.8);"
    "}"
    ".disconnected-notice-label {"
    "  color: #ffff00;"
    "}"
    ".transfer-progress-notice-background {"
    "  background-color: rgba(64, 64, 64, 0.8);"
    "}"
    ".transfer-progress-notice-label {"
    "  color: #ffff00;"
    "}"
    ".ssh-prompt-background {"
    "  background-color: rgba(48, 48, 48, 0.96);"
    "}"
    ".ssh-prompt-title {"
    "  color: #ffffff;"
    "  font-weight: bold;"
    "}"
    ".ssh-prompt-message {"
    "  color: #ffffff;"
    "}";
static constexpr const char *indicator_on_icon_file_name = "green-on.png";
static constexpr const char *indicator_off_icon_file_name = "green-off.png";
static constexpr const char *terminal_context_copy_item_id =
    "terminal_context_copy_item";
static constexpr const char *terminal_context_paste_item_id =
    "terminal_context_paste_item";
static constexpr const char *terminal_context_menu_state_key =
    "terminal-context-menu-state";
static constexpr const char *terminal_context_paste_query_generation_key =
    "terminal-context-paste-query-generation";

struct TerminalContextMenuState {
  VteTerminal *terminal = nullptr;
  GtkWidget *menu = nullptr;
  GtkWidget *copy_item = nullptr;
  GtkWidget *paste_item = nullptr;
  guint paste_query_generation = 0;
  MainWindowTerminalPasteCallbacks paste_callbacks;
};

struct ClipboardTargetsRequest {
  GtkWidget *paste_item = nullptr;
  guint generation = 0;
  std::function<bool()> can_paste;

  ~ClipboardTargetsRequest() {
    if (paste_item != nullptr) {
      g_object_unref(paste_item);
    }
  }
};

struct ClipboardTextRequest {
  std::function<bool()> can_paste;
  std::function<void(std::string utf8_text)> paste;
};

struct MainWindowSshPromptRequest {
  std::shared_ptr<cardio::promise_source<SshUserPromptResponse>> source;
  cardio::cancellation_registration cancellation_registration;
  bool input_required = false;
};

struct MainWindowSshPromptState {
  GtkWidget *panel = nullptr;
  GtkWidget *title_label = nullptr;
  GtkWidget *message_label = nullptr;
  GtkWidget *entry = nullptr;
  GtkWidget *accept_button = nullptr;
  std::shared_ptr<MainWindowSshPromptRequest> request;
};

struct MainWindowTransferProgressState {
  GtkWidget *cancel_button = nullptr;
  MainWindowTransferCancelCallback cancel;
};

static void on_terminal_context_copy_activate(GtkMenuItem *, gpointer data) {
  auto *terminal = VTE_TERMINAL(data);
  vte_terminal_copy_clipboard_format(terminal, VTE_FORMAT_TEXT);
}

static void on_clipboard_targets_received(GtkClipboard *, GdkAtom *atoms,
                                          gint atom_count, gpointer data) {
  std::unique_ptr<ClipboardTargetsRequest> request(
      static_cast<ClipboardTargetsRequest *>(data));
  if (request->paste_item == nullptr ||
      gtk_widget_get_parent(request->paste_item) == nullptr) {
    return;
  }

  const guint current_generation = GPOINTER_TO_UINT(g_object_get_data(
      G_OBJECT(request->paste_item),
      terminal_context_paste_query_generation_key));
  if (request->generation != current_generation) {
    return;
  }

  const bool text_available =
      atoms != nullptr && atom_count > 0 &&
      gtk_targets_include_text(atoms, atom_count) != FALSE;
  const bool application_available =
      request->can_paste && request->can_paste();
  gtk_widget_set_sensitive(request->paste_item,
                           text_available && application_available);
}

static void update_terminal_context_paste_sensitivity(
    TerminalContextMenuState *state) {
  gtk_widget_set_sensitive(state->paste_item, false);
  ++state->paste_query_generation;
  if (state->paste_query_generation == 0) {
    ++state->paste_query_generation;
  }
  g_object_set_data(
      G_OBJECT(state->paste_item),
      terminal_context_paste_query_generation_key,
      GUINT_TO_POINTER(state->paste_query_generation));

  if (!state->paste_callbacks.can_paste ||
      !state->paste_callbacks.paste ||
      !state->paste_callbacks.can_paste()) {
    return;
  }

  GtkClipboard *clipboard = gtk_widget_get_clipboard(
      GTK_WIDGET(state->terminal), GDK_SELECTION_CLIPBOARD);
  auto *request = new ClipboardTargetsRequest{
      .paste_item =
          GTK_WIDGET(g_object_ref(state->paste_item)),
      .generation = state->paste_query_generation,
      .can_paste = state->paste_callbacks.can_paste,
  };
  gtk_clipboard_request_targets(clipboard, on_clipboard_targets_received,
                                request);
}

static void on_clipboard_text_received(GtkClipboard *, const gchar *text,
                                       gpointer data) {
  std::unique_ptr<ClipboardTextRequest> request(
      static_cast<ClipboardTextRequest *>(data));
  if (text == nullptr || text[0] == '\0' || !request->can_paste ||
      !request->paste || !request->can_paste()) {
    return;
  }
  request->paste(std::string(text));
}

static void on_terminal_context_paste_activate(GtkMenuItem *, gpointer data) {
  auto *state = static_cast<TerminalContextMenuState *>(data);
  if (!state->paste_callbacks.can_paste ||
      !state->paste_callbacks.paste ||
      !state->paste_callbacks.can_paste()) {
    return;
  }

  GtkClipboard *clipboard = gtk_widget_get_clipboard(
      GTK_WIDGET(state->terminal), GDK_SELECTION_CLIPBOARD);
  auto *request = new ClipboardTextRequest{
      .can_paste = state->paste_callbacks.can_paste,
      .paste = state->paste_callbacks.paste,
  };
  gtk_clipboard_request_text(clipboard, on_clipboard_text_received, request);
}

static gboolean on_terminal_button_press(GtkWidget *, GdkEventButton *event,
                                         gpointer data) {
  if (event->type != GDK_BUTTON_PRESS ||
      event->button != GDK_BUTTON_SECONDARY) {
    return GDK_EVENT_PROPAGATE;
  }

  auto *state = static_cast<TerminalContextMenuState *>(data);
  gtk_widget_set_sensitive(
      state->copy_item, vte_terminal_get_has_selection(state->terminal));
  update_terminal_context_paste_sensitivity(state);
  gtk_menu_popup_at_pointer(
      GTK_MENU(state->menu), reinterpret_cast<GdkEvent *>(event));
  return GDK_EVENT_STOP;
}

static void destroy_terminal_context_menu_state(gpointer data) {
  auto *state = static_cast<TerminalContextMenuState *>(data);
  gtk_widget_destroy(state->menu);
  g_object_unref(state->menu);
  delete state;
}

static void install_terminal_context_menu(GtkWidget *terminal_widget,
                                          GtkWidget *terminal_overlay) {
  auto *state = new TerminalContextMenuState();
  state->terminal = VTE_TERMINAL(terminal_widget);
  state->menu = gtk_menu_new();
  g_object_ref_sink(state->menu);
  state->copy_item = gtk_menu_item_new_with_label("Copy");
  gestament_gtk_assign_accessible_id(state->copy_item,
                                     terminal_context_copy_item_id);
  gtk_menu_shell_append(GTK_MENU_SHELL(state->menu), state->copy_item);
  state->paste_item = gtk_menu_item_new_with_label("Paste");
  gestament_gtk_assign_accessible_id(state->paste_item,
                                     terminal_context_paste_item_id);
  gtk_menu_shell_append(GTK_MENU_SHELL(state->menu), state->paste_item);
  gtk_widget_show_all(state->menu);

  g_signal_connect(state->copy_item, "activate",
                   G_CALLBACK(on_terminal_context_copy_activate),
                   terminal_widget);
  g_signal_connect(state->paste_item, "activate",
                   G_CALLBACK(on_terminal_context_paste_activate), state);
  gtk_widget_add_events(terminal_widget, GDK_BUTTON_PRESS_MASK);
  g_signal_connect(terminal_widget, "button-press-event",
                   G_CALLBACK(on_terminal_button_press), state);
  gtk_widget_add_events(terminal_overlay, GDK_BUTTON_PRESS_MASK);
  g_signal_connect(terminal_overlay, "button-press-event",
                   G_CALLBACK(on_terminal_button_press), state);
  g_object_set_data_full(G_OBJECT(terminal_overlay),
                         terminal_context_menu_state_key, state,
                         destroy_terminal_context_menu_state);
}

static void set_terminal_dim_overlay_window_pass_through(GtkWidget *widget) {
  GdkWindow *window = gtk_widget_get_window(widget);
  if (window != nullptr) {
    gdk_window_set_pass_through(window, TRUE);
  }
}

static void on_terminal_dim_overlay_realize(GtkWidget *widget, gpointer) {
  // GtkEventBox owns an input window, so make that window transparent to
  // pointer events in addition to the GtkOverlay child property.
  set_terminal_dim_overlay_window_pass_through(widget);
}

static void install_terminal_dim_overlay_input_pass_through(
    GtkWidget *terminal_dim_overlay) {
  GtkWidget *terminal_surface_overlay =
      gtk_widget_get_parent(terminal_dim_overlay);
  if (terminal_surface_overlay != nullptr &&
      GTK_IS_OVERLAY(terminal_surface_overlay)) {
    gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(terminal_surface_overlay),
                                         terminal_dim_overlay, TRUE);
  }
  g_signal_connect(terminal_dim_overlay, "realize",
                   G_CALLBACK(on_terminal_dim_overlay_realize), nullptr);
  set_terminal_dim_overlay_window_pass_through(terminal_dim_overlay);
}

static std::filesystem::path executable_directory() {
  const std::filesystem::path executable_path =
      std::filesystem::read_symlink("/proc/self/exe");
  return executable_path.parent_path();
}

static std::filesystem::path ui_path() {
  return executable_directory() / "main-window.ui";
}

static std::filesystem::path indicator_icon_path(const char *file_name) {
  return executable_directory() / file_name;
}

static GtkWidget *required_widget(GtkBuilder *builder, const char *id) {
  GObject *object = gtk_builder_get_object(builder, id);
  if (object == nullptr || !GTK_IS_WIDGET(object)) {
    std::cerr << "Missing GTK widget: " << id << '\n';
    return nullptr;
  }

  return GTK_WIDGET(object);
}

static bool main_window_has_required_widgets(const MainWindow &main_window) {
  return main_window.window != nullptr && main_window.header_bar != nullptr &&
         main_window.settings_button != nullptr &&
         main_window.transfer_button != nullptr &&
         main_window.root_box != nullptr &&
         main_window.terminal_scroller != nullptr &&
         main_window.terminal_overlay != nullptr &&
         main_window.terminal != nullptr &&
         main_window.terminal_dim_overlay != nullptr &&
         main_window.ssh_prompt_panel != nullptr &&
         main_window.ssh_prompt_background != nullptr &&
         main_window.ssh_prompt_title_label != nullptr &&
         main_window.ssh_prompt_message_label != nullptr &&
         main_window.ssh_prompt_entry != nullptr &&
         main_window.ssh_prompt_cancel_button != nullptr &&
         main_window.ssh_prompt_accept_button != nullptr &&
         main_window.disconnected_notice != nullptr &&
         main_window.disconnected_notice_background != nullptr &&
         main_window.disconnected_notice_label != nullptr &&
         main_window.transfer_progress_overlay != nullptr &&
         main_window.transfer_progress_notice != nullptr &&
         main_window.transfer_progress_notice_background != nullptr &&
         main_window.transfer_progress_notice_label != nullptr &&
         main_window.transfer_progress_bar != nullptr &&
         main_window.transfer_cancel_button != nullptr &&
         main_window.terminal_scrollbar != nullptr &&
         main_window.status_bar != nullptr &&
         main_window.status_label != nullptr &&
         main_window.activity_indicator_bar != nullptr;
}

static GdkPixbuf *load_indicator_pixbuf(const std::filesystem::path &path) {
  GError *error = nullptr;
  GdkPixbuf *pixbuf =
      gdk_pixbuf_new_from_file_at_scale(path.c_str(),
                                        indicator_icon_pixel_size,
                                        indicator_icon_pixel_size, TRUE,
                                        &error);
  if (pixbuf == nullptr) {
    std::cerr << "Failed to load indicator image: " << path << '\n';
    if (error != nullptr) {
      std::cerr << error->message << '\n';
      g_clear_error(&error);
    }
    return nullptr;
  }

  return pixbuf;
}

static std::string activity_indicator_widget_id(ActivityIndicatorId indicator,
                                                const char *suffix) {
  return std::string(activity_indicator_token(indicator)) + "_indicator_" +
         suffix;
}

static GtkWidget *create_activity_indicator_label(
    ActivityIndicatorId indicator) {
  GtkWidget *label = gtk_label_new(activity_indicator_label(indicator));
  gtk_widget_set_can_focus(label, FALSE);
  gtk_label_set_xalign(GTK_LABEL(label), 0.5F);

  PangoAttrList *attributes = pango_attr_list_new();
  pango_attr_list_insert(attributes, pango_attr_scale_new(0.75));
  gtk_label_set_attributes(GTK_LABEL(label), attributes);
  pango_attr_list_unref(attributes);

  const std::string id = activity_indicator_widget_id(indicator, "label");
  gestament_gtk_assign_accessible_id(label, id.c_str());
  gtk_widget_show(label);
  return label;
}

static GtkWidget *create_activity_indicator_image(
    ActivityIndicatorId indicator) {
  GtkWidget *image = gtk_image_new();
  gtk_widget_set_can_focus(image, FALSE);
  gtk_widget_set_halign(image, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(image, GTK_ALIGN_CENTER);
  gtk_widget_set_size_request(image, indicator_icon_pixel_size,
                              indicator_icon_pixel_size);

  const std::string id = activity_indicator_widget_id(indicator, "image");
  gestament_gtk_assign_accessible_id(image, id.c_str());
  gtk_widget_show(image);
  return image;
}

static void create_activity_indicator_widget(MainWindow *main_window,
                                             ActivityIndicatorId indicator) {
  const std::size_t index = activity_indicator_index(indicator);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_can_focus(box, FALSE);
  gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
  gtk_widget_set_size_request(box, 20, -1);

  const std::string id = activity_indicator_widget_id(indicator, "box");
  gestament_gtk_assign_accessible_id(box, id.c_str());

  GtkWidget *image = create_activity_indicator_image(indicator);
  GtkWidget *label = create_activity_indicator_label(indicator);
  gtk_box_pack_start(GTK_BOX(box), image, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(main_window->activity_indicator_bar), box, FALSE,
                     FALSE, 0);
  gtk_widget_show(box);

  main_window->indicator_boxes[index] = box;
  main_window->indicator_images[index] = image;
  main_window->indicator_labels[index] = label;
}

static void create_activity_indicator_widgets(MainWindow *main_window) {
  for (ActivityIndicatorId indicator : activity_indicator_ids) {
    create_activity_indicator_widget(main_window, indicator);
  }
}

static ActivityIndicatorMode activity_indicator_mode(
    ActivityIndicatorId indicator) {
  if (indicator == ActivityIndicatorId::sd ||
      indicator == ActivityIndicatorId::rd) {
    return ActivityIndicatorMode::blink;
  }
  return ActivityIndicatorMode::steady;
}

static bool load_indicator_images(MainWindow *main_window) {
  main_window->indicator_on_icon =
      load_indicator_pixbuf(indicator_icon_path(indicator_on_icon_file_name));
  main_window->indicator_off_icon =
      load_indicator_pixbuf(indicator_icon_path(indicator_off_icon_file_name));
  if (main_window->indicator_on_icon == nullptr ||
      main_window->indicator_off_icon == nullptr) {
    return false;
  }

  for (ActivityIndicatorId indicator : activity_indicator_ids) {
    const std::size_t index = activity_indicator_index(indicator);
    initialize_activity_indicator_widget(
        &main_window->indicators[index], main_window->indicator_images[index],
        main_window->indicator_on_icon, main_window->indicator_off_icon,
        activity_indicator_mode(indicator));
  }
  return true;
}

static void remove_main_window_exterior_provider(
    GtkWidget *widget, GtkCssProvider *provider) {
  if (widget == nullptr || provider == nullptr) {
    return;
  }
  gtk_style_context_remove_provider(
      gtk_widget_get_style_context(widget),
      GTK_STYLE_PROVIDER(provider));
}

static void add_main_window_exterior_provider(
    GtkWidget *widget, GtkCssProvider *provider) {
  if (widget == nullptr || provider == nullptr) {
    return;
  }
  gtk_style_context_add_provider(
      gtk_widget_get_style_context(widget),
      GTK_STYLE_PROVIDER(provider),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

static void update_widget_style_class(
    GtkWidget *widget, const char *style_class, bool enabled) {
  if (widget == nullptr) {
    return;
  }
  GtkStyleContext *context = gtk_widget_get_style_context(widget);
  if (enabled) {
    gtk_style_context_add_class(context, style_class);
  } else {
    gtk_style_context_remove_class(context, style_class);
  }
}

struct SettingsStyleClassUpdate {
  const char *style_class = nullptr;
  bool enabled = false;
};

static void update_settings_background_class(
    MainWindow *main_window, bool enabled) {
  if (main_window->settings_dialog == nullptr) {
    return;
  }

  GtkWidget *dialog_body = gtk_bin_get_child(
      GTK_BIN(main_window->settings_dialog));
  update_widget_style_class(
      dialog_body,
      settings_background_style_class, enabled);
}

static void update_settings_exterior_control_classes_callback(
    GtkWidget *widget, gpointer data) {
  auto *update = static_cast<SettingsStyleClassUpdate *>(data);
  if (widget == nullptr || update == nullptr) {
    return;
  }
  if (GTK_IS_COMBO_BOX(widget)) {
    update_widget_style_class(
        widget, update->style_class, update->enabled);
    return;
  }
  if (GTK_IS_BUTTON_BOX(widget)) {
    update_widget_style_class(
        gtk_widget_get_parent(widget),
        update->style_class, update->enabled);
    return;
  }
  if (GTK_IS_CONTAINER(widget)) {
    gtk_container_forall(
        GTK_CONTAINER(widget),
        update_settings_exterior_control_classes_callback, update);
  }
}

static void update_settings_exterior_control_classes(
    MainWindow *main_window, bool enabled) {
  if (main_window->settings_dialog == nullptr) {
    return;
  }

  GtkWidget *titlebar = gtk_window_get_titlebar(
      GTK_WINDOW(main_window->settings_dialog));
  update_widget_style_class(
      titlebar, settings_exterior_background_style_class, enabled);
  SettingsStyleClassUpdate update{
      .style_class = settings_exterior_background_style_class,
      .enabled = enabled,
  };
  update_settings_exterior_control_classes_callback(
      main_window->settings_widget_root, &update);
}

static void remove_main_window_settings_provider(
    MainWindow *main_window, GtkCssProvider *provider) {
  if (main_window->settings_dialog == nullptr || provider == nullptr) {
    return;
  }
  GdkScreen *screen =
      gtk_widget_get_screen(main_window->settings_dialog);
  if (screen != nullptr) {
    gtk_style_context_remove_provider_for_screen(
        screen, GTK_STYLE_PROVIDER(provider));
  }
}

static void add_main_window_settings_provider(
    MainWindow *main_window, GtkCssProvider *provider,
    guint priority) {
  if (main_window->settings_dialog == nullptr || provider == nullptr) {
    return;
  }
  GdkScreen *screen =
      gtk_widget_get_screen(main_window->settings_dialog);
  if (screen != nullptr) {
    gtk_style_context_add_provider_for_screen(
        screen, GTK_STYLE_PROVIDER(provider),
        priority);
  }
}

static void remove_main_window_settings_exterior_background(
    MainWindow *main_window) {
  GtkCssProvider *provider =
      main_window->settings_exterior_background_provider;
  if (provider == nullptr) {
    return;
  }
  remove_main_window_settings_provider(main_window, provider);
  update_settings_exterior_control_classes(main_window, false);
}

static void add_main_window_settings_exterior_background(
    MainWindow *main_window) {
  GtkCssProvider *provider =
      main_window->settings_exterior_background_provider;
  if (provider == nullptr) {
    return;
  }
  update_settings_exterior_control_classes(main_window, true);
  add_main_window_settings_provider(
      main_window, provider,
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
}

static void remove_main_window_settings_background(
    MainWindow *main_window) {
  GtkCssProvider *provider = main_window->settings_background_provider;
  if (provider == nullptr) {
    return;
  }
  remove_main_window_settings_provider(main_window, provider);
  update_settings_background_class(main_window, false);
}

static void add_main_window_settings_background(MainWindow *main_window) {
  GtkCssProvider *provider = main_window->settings_background_provider;
  if (provider == nullptr) {
    return;
  }
  update_settings_background_class(main_window, true);
  add_main_window_settings_provider(
      main_window, provider,
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

static void clear_main_window_exterior_background(MainWindow *main_window) {
  remove_main_window_settings_exterior_background(main_window);
  GtkCssProvider *provider =
      main_window->exterior_background_provider;
  if (provider != nullptr) {
    remove_main_window_exterior_provider(
        main_window->header_bar, provider);
    remove_main_window_exterior_provider(
        main_window->transfer_button, provider);
    remove_main_window_exterior_provider(
        main_window->settings_button, provider);
    remove_main_window_exterior_provider(
        main_window->status_bar, provider);
  }
  g_clear_object(&main_window->exterior_background_provider);
  g_clear_object(
      &main_window->settings_exterior_background_provider);
}

static void clear_main_window_settings_background(
    MainWindow *main_window) {
  remove_main_window_settings_background(main_window);
  g_clear_object(&main_window->settings_background_provider);
}

static std::array<GtkWidget *, 3>
main_window_overlay_panels(MainWindow *main_window) {
  return {
      main_window->ssh_prompt_panel,
      main_window->disconnected_notice,
      main_window->transfer_progress_overlay,
  };
}

static void clear_main_window_overlay_background(
    MainWindow *main_window) {
  GtkCssProvider *provider =
      main_window->overlay_background_provider;
  if (provider == nullptr) {
    return;
  }

  for (GtkWidget *panel : main_window_overlay_panels(main_window)) {
    remove_widget_tree_background_provider(panel, provider);
  }
  g_clear_object(&main_window->overlay_background_provider);
}

static void set_main_window_overlay_background(
    MainWindow *main_window, const std::optional<RgbColor> &color) {
  clear_main_window_overlay_background(main_window);
  if (!color.has_value()) {
    return;
  }

  GtkCssProvider *provider =
      create_widget_background_provider(
          color.value(), "terminal overlay");
  if (provider == nullptr) {
    return;
  }

  main_window->overlay_background_provider = provider;
  for (GtkWidget *panel : main_window_overlay_panels(main_window)) {
    add_widget_tree_background_provider_at_priority(
        panel, provider,
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
  }
}

static void set_main_window_exterior_background(
    MainWindow *main_window, const std::optional<RgbColor> &color) {
  clear_main_window_exterior_background(main_window);
  if (!color.has_value() || main_window->header_bar == nullptr ||
      main_window->status_bar == nullptr) {
    return;
  }

  GtkCssProvider *provider =
      create_widget_background_provider(
          color.value(), "exterior");
  if (provider == nullptr) {
    return;
  }

  GtkCssProvider *settings_provider =
      create_scoped_widget_background_provider(
          color.value(), settings_exterior_background_style_class,
          nullptr, "settings exterior");
  main_window->exterior_background_provider = provider;
  main_window->settings_exterior_background_provider =
      settings_provider;
  add_main_window_exterior_provider(
      main_window->header_bar, provider);
  add_main_window_exterior_provider(
      main_window->transfer_button, provider);
  add_main_window_exterior_provider(
      main_window->settings_button, provider);
  add_main_window_exterior_provider(
      main_window->status_bar, provider);
  add_main_window_settings_exterior_background(main_window);
}

static void set_main_window_settings_background(
    MainWindow *main_window, const std::optional<RgbColor> &color) {
  clear_main_window_settings_background(main_window);
  if (!color.has_value()) {
    return;
  }

  main_window->settings_background_provider =
      create_scoped_widget_background_provider(
          color.value(), settings_background_style_class,
          gtk_color_button_style_class,
          "settings");
  add_main_window_settings_background(main_window);
}

static void set_main_window_terminal_background(
    MainWindow *main_window, const std::optional<RgbColor> &color) {
  set_main_window_settings_background(main_window, color);
  set_main_window_overlay_background(main_window, color);
  if (main_window->terminal == nullptr) {
    return;
  }

  VteTerminal *terminal = VTE_TERMINAL(main_window->terminal);
  if (!color.has_value()) {
    if (main_window->terminal_background_overridden) {
      vte_terminal_set_default_colors(terminal);
      main_window->terminal_background_overridden = false;
    }
    return;
  }

  static constexpr gdouble channel_maximum = 255.0;
  const GdkRGBA background{
      .red = static_cast<gdouble>(color->red) / channel_maximum,
      .green = static_cast<gdouble>(color->green) / channel_maximum,
      .blue = static_cast<gdouble>(color->blue) / channel_maximum,
      .alpha = 1.0,
  };
  vte_terminal_set_color_background(terminal, &background);
  main_window->terminal_background_overridden = true;
}

static void apply_main_window_style(MainWindow *main_window) {
  GtkCssProvider *provider = gtk_css_provider_new();
  gtk_css_provider_load_from_data(provider, main_window_css, -1, nullptr);

  GtkStyleContext *dim_context =
      gtk_widget_get_style_context(main_window->terminal_dim_overlay);
  gtk_style_context_add_class(dim_context, terminal_dim_overlay_style_class);
  gtk_style_context_add_provider(dim_context, GTK_STYLE_PROVIDER(provider),
                                 GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

  GtkStyleContext *notice_context = gtk_widget_get_style_context(
      main_window->disconnected_notice_background);
  gtk_style_context_add_class(notice_context,
                              disconnected_notice_background_style_class);
  gtk_style_context_add_provider(notice_context, GTK_STYLE_PROVIDER(provider),
                                 GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

  GtkStyleContext *notice_label_context =
      gtk_widget_get_style_context(main_window->disconnected_notice_label);
  gtk_style_context_add_class(notice_label_context,
                              disconnected_notice_label_style_class);
  gtk_style_context_add_provider(notice_label_context,
                                 GTK_STYLE_PROVIDER(provider),
                                 GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

  GtkStyleContext *transfer_notice_context = gtk_widget_get_style_context(
      main_window->transfer_progress_notice_background);
  gtk_style_context_add_class(
      transfer_notice_context, transfer_progress_notice_background_style_class);
  gtk_style_context_add_provider(transfer_notice_context,
                                 GTK_STYLE_PROVIDER(provider),
                                 GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

  GtkStyleContext *transfer_notice_label_context = gtk_widget_get_style_context(
      main_window->transfer_progress_notice_label);
  gtk_style_context_add_class(transfer_notice_label_context,
                              transfer_progress_notice_label_style_class);
  gtk_style_context_add_provider(transfer_notice_label_context,
                                 GTK_STYLE_PROVIDER(provider),
                                 GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

  GtkStyleContext *ssh_prompt_background_context =
      gtk_widget_get_style_context(main_window->ssh_prompt_background);
  gtk_style_context_add_class(ssh_prompt_background_context,
                              ssh_prompt_background_style_class);
  gtk_style_context_add_provider(ssh_prompt_background_context,
                                 GTK_STYLE_PROVIDER(provider),
                                 GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

  GtkStyleContext *ssh_prompt_title_context =
      gtk_widget_get_style_context(main_window->ssh_prompt_title_label);
  gtk_style_context_add_class(ssh_prompt_title_context,
                              ssh_prompt_title_style_class);
  gtk_style_context_add_provider(ssh_prompt_title_context,
                                 GTK_STYLE_PROVIDER(provider),
                                 GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

  GtkStyleContext *ssh_prompt_message_context =
      gtk_widget_get_style_context(main_window->ssh_prompt_message_label);
  gtk_style_context_add_class(ssh_prompt_message_context,
                              ssh_prompt_message_style_class);
  gtk_style_context_add_provider(ssh_prompt_message_context,
                                 GTK_STYLE_PROVIDER(provider),
                                 GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

  g_object_unref(provider);
}

static std::string display_title(const MainWindow &main_window) {
  if (main_window.connection_phase !=
      TerminalSessionConnectionPhase::disconnected) {
    return main_window.base_title;
  }

  return main_window.base_title + disconnected_title_suffix;
}

static void apply_main_window_title(MainWindow *main_window) {
  const std::string title = display_title(*main_window);
  if (main_window->window != nullptr) {
    gtk_window_set_title(GTK_WINDOW(main_window->window), title.c_str());
  }
  if (main_window->header_bar != nullptr) {
    gtk_header_bar_set_title(GTK_HEADER_BAR(main_window->header_bar),
                             title.c_str());
  }
}

static void set_main_window_disconnected_notice_visible(
    MainWindow *main_window, bool visible) {
  if (main_window == nullptr || main_window->disconnected_notice == nullptr) {
    return;
  }

  gtk_widget_set_no_show_all(main_window->disconnected_notice, !visible);
  gtk_widget_set_visible(main_window->disconnected_notice, visible);
  if (visible) {
    gtk_widget_show_all(main_window->disconnected_notice);
  }
}

static void set_main_window_terminal_dim_visible(MainWindow *main_window,
                                                 bool visible) {
  if (main_window == nullptr || main_window->terminal_dim_overlay == nullptr) {
    return;
  }

  gtk_widget_set_no_show_all(main_window->terminal_dim_overlay, !visible);
  gtk_widget_set_visible(main_window->terminal_dim_overlay, visible);
  if (visible) {
    gtk_widget_show_all(main_window->terminal_dim_overlay);
  }
}

static void hide_main_window_ssh_prompt(MainWindowSshPromptState *state) {
  if (state == nullptr || state->panel == nullptr) {
    return;
  }

  gtk_entry_set_text(GTK_ENTRY(state->entry), "");
  gtk_widget_set_visible(state->entry, false);
  gtk_widget_set_no_show_all(state->entry, true);
  gtk_widget_set_visible(state->panel, false);
  gtk_widget_set_no_show_all(state->panel, true);
}

static void complete_main_window_ssh_prompt(
    MainWindowSshPromptState *state, SshUserPromptResponse response) {
  if (state == nullptr || state->request == nullptr) {
    return;
  }

  std::shared_ptr<MainWindowSshPromptRequest> request =
      std::exchange(state->request, nullptr);
  request->cancellation_registration = {};
  hide_main_window_ssh_prompt(state);
  (void)request->source->try_resolve(std::move(response));
}

static void on_ssh_prompt_accept_clicked(GtkButton *, gpointer data) {
  auto *state = static_cast<MainWindowSshPromptState *>(data);
  if (state == nullptr || state->request == nullptr) {
    return;
  }

  std::string text;
  if (state->request->input_required) {
    const char *entry_text = gtk_entry_get_text(GTK_ENTRY(state->entry));
    text = entry_text == nullptr ? "" : entry_text;
  }
  complete_main_window_ssh_prompt(
      state, {.accepted = true, .text = std::move(text)});
}

static void on_ssh_prompt_cancel_clicked(GtkButton *, gpointer data) {
  complete_main_window_ssh_prompt(
      static_cast<MainWindowSshPromptState *>(data), {});
}

static void on_ssh_prompt_entry_activated(GtkEntry *, gpointer data) {
  on_ssh_prompt_accept_clicked(nullptr, data);
}

static void on_transfer_cancel_clicked(GtkButton *, gpointer data) {
  auto *state = static_cast<MainWindowTransferProgressState *>(data);
  if (state == nullptr || !state->cancel) {
    return;
  }
  if (state->cancel() && state->cancel_button != nullptr) {
    gtk_widget_set_sensitive(state->cancel_button, FALSE);
  }
}

static void stop_main_window_transfer_progress_pulse(MainWindow *main_window) {
  if (main_window == nullptr ||
      main_window->transfer_progress_pulse_source == 0) {
    return;
  }

  g_source_remove(main_window->transfer_progress_pulse_source);
  main_window->transfer_progress_pulse_source = 0;
}

static gboolean pulse_main_window_transfer_progress(gpointer data) {
  auto *main_window = static_cast<MainWindow *>(data);
  if (main_window == nullptr || main_window->transfer_progress_bar == nullptr ||
      main_window->transfer_progress_notice == nullptr ||
      !gtk_widget_get_visible(main_window->transfer_progress_notice)) {
    if (main_window != nullptr) {
      main_window->transfer_progress_pulse_source = 0;
    }
    return G_SOURCE_REMOVE;
  }

  gtk_progress_bar_pulse(GTK_PROGRESS_BAR(main_window->transfer_progress_bar));
  return G_SOURCE_CONTINUE;
}

static void start_main_window_transfer_progress_pulse(MainWindow *main_window) {
  if (main_window == nullptr ||
      main_window->transfer_progress_pulse_source != 0) {
    return;
  }

  main_window->transfer_progress_pulse_source =
      g_timeout_add(transfer_progress_pulse_period_ms,
                    pulse_main_window_transfer_progress, main_window);
}

std::optional<MainWindow> load_main_window() {
  MainWindow main_window;
  GError *error = nullptr;
  main_window.builder = gtk_builder_new();
  const std::filesystem::path builder_file = ui_path();
  if (!gtk_builder_add_from_file(main_window.builder, builder_file.c_str(),
                                 &error)) {
    std::cerr << "Failed to load UI file: " << builder_file << '\n';
    if (error != nullptr) {
      std::cerr << error->message << '\n';
      g_clear_error(&error);
    }
    release_main_window(&main_window);
    return std::nullopt;
  }

  gestament_gtk_assign_accessible_ids_from_builder(main_window.builder);

  main_window.window = required_widget(main_window.builder, "main_window");
  main_window.header_bar = required_widget(main_window.builder, "header_bar");
  main_window.settings_button =
      required_widget(main_window.builder, "settings_button");
  main_window.transfer_button =
      required_widget(main_window.builder, "transfer_button");
  main_window.root_box = required_widget(main_window.builder, "root_box");
  main_window.terminal_scroller =
      required_widget(main_window.builder, "terminal_scroller");
  main_window.terminal_overlay =
      required_widget(main_window.builder, "terminal_overlay");
  main_window.terminal =
      required_widget(main_window.builder, "terminal_view");
  main_window.terminal_dim_overlay =
      required_widget(main_window.builder, "terminal_dim_overlay");
  main_window.ssh_prompt_panel =
      required_widget(main_window.builder, "ssh_prompt_panel");
  main_window.ssh_prompt_background =
      required_widget(main_window.builder, "ssh_prompt_background");
  main_window.ssh_prompt_title_label =
      required_widget(main_window.builder, "ssh_prompt_title_label");
  main_window.ssh_prompt_message_label =
      required_widget(main_window.builder, "ssh_prompt_message_label");
  main_window.ssh_prompt_entry =
      required_widget(main_window.builder, "ssh_prompt_entry");
  main_window.ssh_prompt_cancel_button =
      required_widget(main_window.builder, "ssh_prompt_cancel_button");
  main_window.ssh_prompt_accept_button =
      required_widget(main_window.builder, "ssh_prompt_accept_button");
  main_window.disconnected_notice =
      required_widget(main_window.builder, "disconnected_notice");
  main_window.disconnected_notice_background =
      required_widget(main_window.builder, "disconnected_notice_background");
  main_window.disconnected_notice_label =
      required_widget(main_window.builder, "disconnected_notice_label");
  main_window.transfer_progress_overlay =
      required_widget(main_window.builder, "transfer_progress_overlay");
  main_window.transfer_progress_notice =
      required_widget(main_window.builder, "transfer_progress_notice");
  main_window.transfer_progress_notice_background =
      required_widget(main_window.builder, "transfer_progress_notice_background");
  main_window.transfer_progress_notice_label =
      required_widget(main_window.builder, "transfer_progress_notice_label");
  main_window.transfer_progress_bar =
      required_widget(main_window.builder, "transfer_progress_bar");
  main_window.transfer_cancel_button =
      required_widget(main_window.builder, "transfer_cancel_button");
  main_window.terminal_scrollbar =
      required_widget(main_window.builder, "terminal_scrollbar");
  main_window.status_bar = required_widget(main_window.builder, "status_bar");
  main_window.status_label =
      required_widget(main_window.builder, "status_label");
  main_window.activity_indicator_bar =
      required_widget(main_window.builder, "activity_indicator_bar");
  if (!main_window_has_required_widgets(main_window)) {
    release_main_window(&main_window);
    return std::nullopt;
  }
  install_terminal_context_menu(main_window.terminal,
                                main_window.terminal_overlay);
  install_terminal_dim_overlay_input_pass_through(
      main_window.terminal_dim_overlay);
  main_window.ssh_prompt_state = std::make_shared<MainWindowSshPromptState>(
      MainWindowSshPromptState{
          .panel = main_window.ssh_prompt_panel,
          .title_label = main_window.ssh_prompt_title_label,
          .message_label = main_window.ssh_prompt_message_label,
          .entry = main_window.ssh_prompt_entry,
          .accept_button = main_window.ssh_prompt_accept_button,
          .request = nullptr,
      });
  g_signal_connect(main_window.ssh_prompt_accept_button, "clicked",
                   G_CALLBACK(on_ssh_prompt_accept_clicked),
                   main_window.ssh_prompt_state.get());
  g_signal_connect(main_window.ssh_prompt_cancel_button, "clicked",
                   G_CALLBACK(on_ssh_prompt_cancel_clicked),
                   main_window.ssh_prompt_state.get());
  g_signal_connect(main_window.ssh_prompt_entry, "activate",
                   G_CALLBACK(on_ssh_prompt_entry_activated),
                   main_window.ssh_prompt_state.get());
  main_window.transfer_progress_state =
      std::make_shared<MainWindowTransferProgressState>(
          MainWindowTransferProgressState{
              .cancel_button = main_window.transfer_cancel_button,
              .cancel = {},
          });
  g_signal_connect(main_window.transfer_cancel_button, "clicked",
                   G_CALLBACK(on_transfer_cancel_clicked),
                   main_window.transfer_progress_state.get());
  apply_main_window_style(&main_window);
  create_activity_indicator_widgets(&main_window);
  if (!load_indicator_images(&main_window)) {
    release_main_window(&main_window);
    return std::nullopt;
  }
  set_main_window_activity_indicator_connection_kind(
      &main_window, TerminalConnectionKind::local_shell);
  set_main_window_connection_phase(
      &main_window, TerminalSessionConnectionPhase::disconnected);
  set_main_window_transfer_button_visible(&main_window, false);

  return main_window;
}

void set_main_window_colors(MainWindow *main_window,
                            const GeneralColorSettings &settings) {
  if (main_window == nullptr) {
    return;
  }

  set_main_window_exterior_background(main_window,
                                      settings.exterior_background);
  set_main_window_terminal_background(main_window,
                                      settings.background);
}

void set_main_window_settings_dialog(
    MainWindow *main_window, GtkWidget *dialog,
    GtkWidget *settings_root) {
  if (main_window == nullptr) {
    return;
  }

  if (main_window->settings_background_provider != nullptr) {
    remove_main_window_settings_background(main_window);
  }
  if (main_window->settings_exterior_background_provider != nullptr) {
    remove_main_window_settings_exterior_background(main_window);
  }
  main_window->settings_dialog = dialog;
  main_window->settings_widget_root = settings_root;
  if (main_window->settings_background_provider != nullptr) {
    add_main_window_settings_background(main_window);
  }
  if (main_window->settings_exterior_background_provider != nullptr) {
    add_main_window_settings_exterior_background(main_window);
  }
}

void set_main_window_terminal_paste_callbacks(
    MainWindow *main_window, MainWindowTerminalPasteCallbacks callbacks) {
  if (main_window == nullptr || main_window->terminal_overlay == nullptr) {
    return;
  }

  auto *state = static_cast<TerminalContextMenuState *>(g_object_get_data(
      G_OBJECT(main_window->terminal_overlay),
      terminal_context_menu_state_key));
  if (state != nullptr) {
    state->paste_callbacks = std::move(callbacks);
  }
}

void note_main_window_activity(MainWindow *main_window,
                               ActivityIndicatorId indicator) {
  if (main_window == nullptr) {
    return;
  }

  const std::size_t index = activity_indicator_index(indicator);
  if (!main_window->indicator_visible[index]) {
    return;
  }
  note_activity_indicator_widget(&main_window->indicators[index]);
}

void set_main_window_indicator_state(MainWindow *main_window,
                                     ActivityIndicatorId indicator,
                                     bool active) {
  if (main_window == nullptr) {
    return;
  }

  const std::size_t index = activity_indicator_index(indicator);
  if (!main_window->indicator_visible[index]) {
    active = false;
  }
  set_activity_indicator_widget_active(&main_window->indicators[index],
                                       active);
}

void set_main_window_connection_phase(MainWindow *main_window,
                                      TerminalSessionConnectionPhase phase) {
  if (main_window == nullptr) {
    return;
  }

  const TerminalConnectionPresentation presentation =
      terminal_connection_presentation(phase);
  set_main_window_indicator_state(main_window, ActivityIndicatorId::conn,
                                  presentation.connection_active);
  main_window->connection_phase = phase;
  main_window->connection_active = presentation.connection_active;
  set_main_window_terminal_interactive(main_window,
                                       presentation.terminal_interactive);
  set_main_window_disconnected_notice_visible(
      main_window, presentation.disconnected_notice_visible);
  apply_main_window_title(main_window);
}

void set_main_window_terminal_interactive(MainWindow *main_window,
                                          bool interactive) {
  if (main_window == nullptr) {
    return;
  }
  main_window->terminal_interactive = interactive;
  if (main_window->terminal == nullptr) {
    return;
  }

  vte_terminal_set_input_enabled(VTE_TERMINAL(main_window->terminal),
                                 interactive);
  set_main_window_terminal_dim_visible(main_window, !interactive);
}

void focus_main_window_terminal_if_interactive(MainWindow *main_window) {
  if (main_window == nullptr || !main_window->terminal_interactive ||
      main_window->terminal == nullptr ||
      !gtk_widget_get_visible(main_window->terminal) ||
      !gtk_widget_get_mapped(main_window->terminal)) {
    return;
  }

  gtk_widget_grab_focus(main_window->terminal);
}

cardio::promise<SshUserPromptResponse> prompt_main_window_ssh_async(
    MainWindow *main_window, const SshUserPrompt &prompt,
    cardio::cancellation cancellation) {
  if (main_window == nullptr || main_window->ssh_prompt_state == nullptr ||
      cancellation.is_cancellation_requested()) {
    co_return SshUserPromptResponse{};
  }

  cancel_main_window_ssh_prompt(main_window);
  std::shared_ptr<MainWindowSshPromptState> state =
      main_window->ssh_prompt_state;
  auto source =
      std::make_shared<cardio::promise_source<SshUserPromptResponse>>();
  cardio::promise<SshUserPromptResponse> response = source->get_promise();
  auto request = std::make_shared<MainWindowSshPromptRequest>();
  request->source = source;
  request->input_required = prompt.input_required;
  state->request = request;

  const std::weak_ptr<MainWindowSshPromptState> weak_state = state;
  const std::weak_ptr<MainWindowSshPromptRequest> weak_request = request;
  request->cancellation_registration =
      cancellation.on_cancellation_requested(
          [weak_state, weak_request, source]() {
            std::shared_ptr<MainWindowSshPromptState> current_state =
                weak_state.lock();
            std::shared_ptr<MainWindowSshPromptRequest> current_request =
                weak_request.lock();
            if (current_state != nullptr && current_request != nullptr &&
                current_state->request == current_request) {
              complete_main_window_ssh_prompt(current_state.get(), {});
              return;
            }
            (void)source->try_resolve({});
          });

  gtk_label_set_text(GTK_LABEL(state->title_label),
                     prompt.title.empty() ? "SSH" : prompt.title.c_str());
  gtk_label_set_text(GTK_LABEL(state->message_label),
                     prompt.message.c_str());
  gtk_button_set_label(
      GTK_BUTTON(state->accept_button),
      prompt.kind == SshUserPromptKind::host_key ? "Accept" : "OK");
  gtk_entry_set_text(GTK_ENTRY(state->entry), "");
  gtk_entry_set_visibility(GTK_ENTRY(state->entry), prompt.echo);

  set_main_window_terminal_interactive(main_window, false);
  gtk_widget_set_no_show_all(state->panel, false);
  gtk_widget_show_all(state->panel);
  gtk_widget_set_no_show_all(state->panel, true);
  gtk_widget_set_visible(state->entry, prompt.input_required);
  gtk_widget_set_no_show_all(state->entry, !prompt.input_required);
  gtk_widget_grab_focus(prompt.input_required ? state->entry
                                             : state->accept_button);

  co_return co_await response;
}

void cancel_main_window_ssh_prompt(MainWindow *main_window) {
  if (main_window == nullptr || main_window->ssh_prompt_state == nullptr) {
    return;
  }
  complete_main_window_ssh_prompt(main_window->ssh_prompt_state.get(), {});
}

void set_main_window_transfer_progress_visible(MainWindow *main_window,
                                               bool visible) {
  if (main_window == nullptr ||
      main_window->transfer_progress_overlay == nullptr ||
      main_window->transfer_progress_notice == nullptr) {
    return;
  }

  if (!visible) {
    stop_main_window_transfer_progress_pulse(main_window);
  }

  gtk_widget_set_no_show_all(main_window->transfer_progress_overlay, !visible);
  gtk_widget_set_visible(main_window->transfer_progress_overlay, visible);
  gtk_widget_set_no_show_all(main_window->transfer_progress_notice, !visible);
  gtk_widget_set_visible(main_window->transfer_progress_notice, visible);
  if (visible) {
    if (main_window->transfer_cancel_button != nullptr) {
      gtk_widget_set_sensitive(main_window->transfer_cancel_button, TRUE);
    }
    gtk_widget_show_all(main_window->transfer_progress_overlay);
    set_main_window_transfer_progress(
        main_window, TerminalTransferProgress{
                         .mode = TerminalTransferProgressMode::indeterminate,
                         .fraction = std::nullopt,
                     });
  }
}

void set_main_window_transfer_cancel_callback(
    MainWindow *main_window, MainWindowTransferCancelCallback callback) {
  if (main_window == nullptr || main_window->transfer_progress_state == nullptr) {
    return;
  }
  main_window->transfer_progress_state->cancel = std::move(callback);
}

void set_main_window_transfer_progress(MainWindow *main_window,
                                       TerminalTransferProgress progress) {
  if (main_window == nullptr ||
      main_window->transfer_progress_bar == nullptr) {
    return;
  }
  const bool notice_visible =
      main_window->transfer_progress_notice != nullptr &&
      gtk_widget_get_visible(main_window->transfer_progress_notice);

  if (progress.mode == TerminalTransferProgressMode::determinate) {
    stop_main_window_transfer_progress_pulse(main_window);
    const double fraction =
        std::clamp(progress.fraction.value_or(0.0), 0.0, 1.0);
    gtk_progress_bar_set_fraction(
        GTK_PROGRESS_BAR(main_window->transfer_progress_bar), fraction);
    return;
  }

  gtk_progress_bar_set_fraction(
      GTK_PROGRESS_BAR(main_window->transfer_progress_bar), 0.0);
  if (notice_visible) {
    start_main_window_transfer_progress_pulse(main_window);
  }
}

void set_main_window_transfer_button_visible(MainWindow *main_window,
                                             bool visible) {
  if (main_window == nullptr || main_window->transfer_button == nullptr) {
    return;
  }

  gtk_widget_set_no_show_all(main_window->transfer_button, !visible);
  gtk_widget_set_visible(main_window->transfer_button, visible);
}

void set_main_window_transfer_button_sensitive(MainWindow *main_window,
                                               bool sensitive) {
  if (main_window == nullptr || main_window->transfer_button == nullptr) {
    return;
  }

  gtk_widget_set_sensitive(main_window->transfer_button, sensitive);
}

void set_main_window_status_text(MainWindow *main_window,
                                 const std::string &text) {
  if (main_window == nullptr || main_window->status_label == nullptr) {
    return;
  }

  gtk_label_set_text(GTK_LABEL(main_window->status_label), text.c_str());
}

void set_main_window_title(MainWindow *main_window, const std::string &title) {
  if (main_window == nullptr) {
    return;
  }

  main_window->base_title = title;
  apply_main_window_title(main_window);
}

void set_main_window_activity_indicator_connection_kind(
    MainWindow *main_window, TerminalConnectionKind kind) {
  if (main_window == nullptr) {
    return;
  }

  const bool serial_visible = kind == TerminalConnectionKind::serial;
  for (ActivityIndicatorId indicator : activity_indicator_ids) {
    const bool visible =
        !activity_indicator_is_serial_line(indicator) || serial_visible;
    const std::size_t index = activity_indicator_index(indicator);
    main_window->indicator_visible[index] = visible;
    GtkWidget *box = main_window->indicator_boxes[index];
    if (box == nullptr) {
      continue;
    }

    gtk_widget_set_no_show_all(box, !visible);
    gtk_widget_set_visible(box, visible);
    if (visible) {
      gtk_widget_show_all(box);
    } else {
      reset_activity_indicator_widget(&main_window->indicators[index]);
    }
  }
}

void set_main_window_activity_indicators_latched(MainWindow *main_window,
                                                 bool latch) {
  if (main_window == nullptr) {
    return;
  }

  for (ActivityIndicatorWidget &indicator : main_window->indicators) {
    if (indicator.mode == ActivityIndicatorMode::blink) {
      set_activity_indicator_widget_latched(&indicator, latch);
    }
  }
}

void deactivate_main_window_activity_indicators(MainWindow *main_window) {
  if (main_window == nullptr) {
    return;
  }

  for (ActivityIndicatorWidget &indicator : main_window->indicators) {
    release_activity_indicator_widget(&indicator);
  }
  main_window->indicator_boxes.fill(nullptr);
  main_window->indicator_images.fill(nullptr);
  main_window->indicator_labels.fill(nullptr);
  main_window->indicator_visible.fill(false);
}

void release_main_window(MainWindow *main_window) {
  if (main_window == nullptr) {
    return;
  }

  cancel_main_window_ssh_prompt(main_window);
  stop_main_window_transfer_progress_pulse(main_window);
  deactivate_main_window_activity_indicators(main_window);
  clear_main_window_exterior_background(main_window);
  clear_main_window_settings_background(main_window);
  clear_main_window_overlay_background(main_window);
  g_clear_object(&main_window->indicator_on_icon);
  g_clear_object(&main_window->indicator_off_icon);
  if (main_window->builder != nullptr) {
    g_object_unref(main_window->builder);
  }
  *main_window = {};
}

} // namespace elder_terms
