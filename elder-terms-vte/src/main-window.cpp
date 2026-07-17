#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gestament/gtk.h>
#include <vte/vte.h>

#include "main-window.h"

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
    "}";
static constexpr const char *indicator_on_icon_file_name = "green-on.png";
static constexpr const char *indicator_off_icon_file_name = "green-off.png";

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
         main_window.disconnected_notice != nullptr &&
         main_window.disconnected_notice_background != nullptr &&
         main_window.disconnected_notice_label != nullptr &&
         main_window.transfer_progress_notice != nullptr &&
         main_window.transfer_progress_notice_background != nullptr &&
         main_window.transfer_progress_notice_label != nullptr &&
         main_window.transfer_progress_bar != nullptr &&
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

  g_object_unref(provider);
}

static std::string display_title(const MainWindow &main_window) {
  if (main_window.connection_active) {
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
  main_window.disconnected_notice =
      required_widget(main_window.builder, "disconnected_notice");
  main_window.disconnected_notice_background =
      required_widget(main_window.builder, "disconnected_notice_background");
  main_window.disconnected_notice_label =
      required_widget(main_window.builder, "disconnected_notice_label");
  main_window.transfer_progress_notice =
      required_widget(main_window.builder, "transfer_progress_notice");
  main_window.transfer_progress_notice_background =
      required_widget(main_window.builder, "transfer_progress_notice_background");
  main_window.transfer_progress_notice_label =
      required_widget(main_window.builder, "transfer_progress_notice_label");
  main_window.transfer_progress_bar =
      required_widget(main_window.builder, "transfer_progress_bar");
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
  apply_main_window_style(&main_window);
  create_activity_indicator_widgets(&main_window);
  if (!load_indicator_images(&main_window)) {
    release_main_window(&main_window);
    return std::nullopt;
  }
  set_main_window_activity_indicator_connection_kind(
      &main_window, TerminalConnectionKind::local_shell);
  set_main_window_connection_active(&main_window, false);
  set_main_window_transfer_button_visible(&main_window, false);

  return main_window;
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

void set_main_window_connection_active(MainWindow *main_window,
                                       bool connected) {
  if (main_window == nullptr) {
    return;
  }

  set_main_window_indicator_state(main_window, ActivityIndicatorId::conn,
                                  connected);
  main_window->connection_active = connected;
  set_main_window_terminal_interactive(main_window, connected);
  set_main_window_disconnected_notice_visible(main_window, !connected);
  apply_main_window_title(main_window);
}

void set_main_window_terminal_interactive(MainWindow *main_window,
                                          bool interactive) {
  if (main_window == nullptr) {
    return;
  }
  if (main_window->terminal == nullptr) {
    return;
  }

  vte_terminal_set_input_enabled(VTE_TERMINAL(main_window->terminal),
                                 interactive);
  set_main_window_terminal_dim_visible(main_window, !interactive);
}

void set_main_window_transfer_progress_visible(MainWindow *main_window,
                                               bool visible) {
  if (main_window == nullptr ||
      main_window->transfer_progress_notice == nullptr) {
    return;
  }

  if (!visible) {
    stop_main_window_transfer_progress_pulse(main_window);
  }

  gtk_widget_set_no_show_all(main_window->transfer_progress_notice, !visible);
  gtk_widget_set_visible(main_window->transfer_progress_notice, visible);
  if (visible) {
    gtk_widget_show_all(main_window->transfer_progress_notice);
    set_main_window_transfer_progress(
        main_window, TerminalTransferProgress{
                         .mode = TerminalTransferProgressMode::indeterminate,
                         .fraction = std::nullopt,
                     });
  }
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

  stop_main_window_transfer_progress_pulse(main_window);
  deactivate_main_window_activity_indicators(main_window);
  g_clear_object(&main_window->indicator_on_icon);
  g_clear_object(&main_window->indicator_off_icon);
  if (main_window->builder != nullptr) {
    g_object_unref(main_window->builder);
  }
  *main_window = {};
}

} // namespace elder_terms
