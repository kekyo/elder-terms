#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <utility>

#include <gtk/gtk.h>
#include <vte/vte.h>

#include <cardio.h>
#include <gestament/gtk.h>

#include <elder-terms/settings.h>
#include <elder-terms/settings-widget.h>

#include "launch-options.h"
#include "main-window.h"
#include "terminal-layout.h"
#include "terminal-session.h"

struct ApplicationState {
  elder_terms::TerminalSessionState *session_state = nullptr;
  elder_terms::TerminalLayoutState *layout_state = nullptr;
  cardio::dispatcher_group_glib *dispatcher_group = nullptr;
  elder_terms::SettingsStore settings_store;
  std::optional<std::filesystem::path> config_path;
  bool auto_close = true;
  GtkWidget *window = nullptr;
  GtkWidget *settings_dialog = nullptr;
  guint settings_dialog_close_idle_id = 0;
  elder_terms::SettingsWidgetState *settings_widget = nullptr;
};

static void close_settings_dialog(ApplicationState *state);
static void schedule_settings_dialog_close(ApplicationState *state);

static void on_main_window_destroy(GtkWidget *, gpointer user_data) {
  auto *state = static_cast<ApplicationState *>(user_data);
  close_settings_dialog(state);
  elder_terms::stop_terminal_session(state->session_state);
  state->dispatcher_group->shutdown();
}

static void on_settings_dialog_destroy(GtkWidget *, gpointer user_data) {
  auto *state = static_cast<ApplicationState *>(user_data);
  if (state->settings_dialog_close_idle_id != 0) {
    g_source_remove(state->settings_dialog_close_idle_id);
    state->settings_dialog_close_idle_id = 0;
  }
  if (state->settings_widget != nullptr) {
    elder_terms::destroy_settings_widget(state->settings_widget);
    state->settings_widget = nullptr;
  }
  state->settings_dialog = nullptr;
}

static void close_settings_dialog(ApplicationState *state) {
  if (state == nullptr) {
    return;
  }

  if (state->settings_dialog_close_idle_id != 0) {
    g_source_remove(state->settings_dialog_close_idle_id);
    state->settings_dialog_close_idle_id = 0;
  }

  if (state->settings_dialog == nullptr) {
    return;
  }

  gtk_widget_destroy(state->settings_dialog);
}

static gboolean close_settings_dialog_idle(gpointer user_data) {
  auto *state = static_cast<ApplicationState *>(user_data);
  if (state != nullptr) {
    state->settings_dialog_close_idle_id = 0;
  }
  close_settings_dialog(state);
  return G_SOURCE_REMOVE;
}

static void schedule_settings_dialog_close(ApplicationState *state) {
  if (state == nullptr || state->settings_dialog == nullptr ||
      state->settings_dialog_close_idle_id != 0) {
    return;
  }

  state->settings_dialog_close_idle_id =
      g_idle_add(close_settings_dialog_idle, state);
}

static void apply_runtime_settings(ApplicationState *state,
                                   const elder_terms::SettingsStore &store) {
  state->settings_store = store;
  state->auto_close = elder_terms::terminal_auto_close(state->settings_store);
  elder_terms::apply_terminal_session_connection_profile(
      state->session_state,
      elder_terms::terminal_connection_profile(state->settings_store));
  elder_terms::apply_terminal_display_settings(
      state->layout_state,
      elder_terms::terminal_display_settings(state->settings_store));
}

static bool save_runtime_settings(ApplicationState *state,
                                  const elder_terms::SettingsStore &store) {
  if (!state->config_path.has_value()) {
    return false;
  }

  const elder_terms::SettingsSaveResult save_result =
      elder_terms::save_settings(store, state->config_path.value());
  for (const std::string &warning : save_result.warnings) {
    std::cerr << warning << '\n';
  }
  if (!save_result.saved) {
    return false;
  }

  apply_runtime_settings(state, store);
  return true;
}

static void update_runtime_terminal_display_settings(
    ApplicationState *state,
    elder_terms::TerminalDisplaySettings terminal_display_settings) {
  elder_terms::set_setting_value(
      &state->settings_store, elder_terms::terminal_width_setting_key(),
      elder_terms::SettingValue{
          static_cast<gint64>(terminal_display_settings.width)});
  elder_terms::set_setting_value(
      &state->settings_store, elder_terms::terminal_height_setting_key(),
      elder_terms::SettingValue{
          static_cast<gint64>(terminal_display_settings.height)});
  elder_terms::set_setting_value(
      &state->settings_store, elder_terms::terminal_zoom_setting_key(),
      elder_terms::SettingValue{terminal_display_settings.zoom});

  if (state->settings_widget != nullptr) {
    elder_terms::update_settings_widget_store(state->settings_widget,
                                              state->settings_store);
  }
}

static void open_settings_dialog(ApplicationState *state) {
  if (state->settings_dialog != nullptr) {
    gtk_window_present(GTK_WINDOW(state->settings_dialog));
    return;
  }

  GtkWidget *dialog = gtk_dialog_new();
  gestament_gtk_assign_accessible_id(dialog, "settings_dialog");
  gtk_window_set_title(GTK_WINDOW(dialog), "Settings");
  gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(state->window));
  gtk_window_set_default_size(GTK_WINDOW(dialog), 720, 420);

  elder_terms::SettingsWidgetCallbacks callbacks;
  callbacks.apply = [state](const elder_terms::SettingsStore &store) {
    apply_runtime_settings(state, store);
    schedule_settings_dialog_close(state);
  };
  if (state->config_path.has_value()) {
    callbacks.save = [state](const elder_terms::SettingsStore &store) {
      if (!save_runtime_settings(state, store)) {
        return false;
      }
      schedule_settings_dialog_close(state);
      return true;
    };
  }
  callbacks.cancel = [state]() { schedule_settings_dialog_close(state); };

  elder_terms::SettingsWidgetOptions options{
      .store = state->settings_store,
      .is_runtime = true,
      .callbacks = std::move(callbacks),
  };
  state->settings_widget =
      elder_terms::create_settings_widget(std::move(options));
  gtk_container_add(
      GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dialog))),
      elder_terms::settings_widget_root(state->settings_widget));

  state->settings_dialog = dialog;
  g_signal_connect(dialog, "destroy", G_CALLBACK(on_settings_dialog_destroy),
                   state);
  gtk_widget_show_all(dialog);
}

static void on_settings_button_clicked(GtkButton *, gpointer user_data) {
  open_settings_dialog(static_cast<ApplicationState *>(user_data));
}

int main(int argc, char **argv) {
  const auto launch_options =
    elder_terms::parse_launch_options(&argc, argv);
  gtk_init(&argc, &argv);

  g_type_ensure(VTE_TYPE_TERMINAL);

  cardio::dispatcher_group_glib dispatcher_group;
  cardio::dispatcher_host_glib dispatcher(dispatcher_group);

  auto main_window = elder_terms::load_main_window();
  if (!main_window.has_value()) {
    return 1;
  }

  auto vte_terminal = VTE_TERMINAL(main_window->terminal);
  const auto default_zoom =
    vte_terminal_get_font_scale(vte_terminal);
  const elder_terms::SettingsLoadOptions settings_load_options{
      .config_path = launch_options.config_path,
      .startup_config_path = launch_options.startup_config_path,
  };
  const auto settings_result =
    elder_terms::load_settings(settings_load_options, default_zoom);

  for (const std::string &warning : settings_result.warnings) {
    std::cerr << warning << '\n';
  }

  ApplicationState app_state{
      .session_state = nullptr,
      .layout_state = nullptr,
      .dispatcher_group = &dispatcher_group,
      .settings_store = settings_result.store,
      .config_path = launch_options.config_path,
      .auto_close = elder_terms::terminal_auto_close(settings_result.store),
      .window = main_window->window,
      .settings_dialog = nullptr,
      .settings_dialog_close_idle_id = 0,
      .settings_widget = nullptr,
  };

  const auto terminal_display_settings =
    elder_terms::terminal_display_settings(app_state.settings_store);
  const auto connection_profile =
    elder_terms::terminal_connection_profile(app_state.settings_store);

  vte_terminal_set_font_scale(
    vte_terminal, terminal_display_settings.zoom);
  vte_terminal_set_size(
    vte_terminal, terminal_display_settings.width, terminal_display_settings.height);

  if (!launch_options.test.fixture) {
    app_state.session_state = elder_terms::create_terminal_session(
      main_window->terminal,
      connection_profile,
      {
        .ended = [&app_state]() {
          if (app_state.auto_close && app_state.window != nullptr) {
            gtk_widget_destroy(app_state.window);
          }
        },
      });
  }

  app_state.layout_state = elder_terms::create_terminal_layout(
    *main_window, launch_options.test, terminal_display_settings,
    {
      .grid_size_changed = [&app_state](glong columns, glong rows) {
        elder_terms::resize_terminal_session(app_state.session_state, columns,
                                             rows);
      },
      .display_settings_changed =
          [&app_state](
              elder_terms::TerminalDisplaySettings terminal_settings) {
            update_runtime_terminal_display_settings(&app_state,
                                                     terminal_settings);
          },
    });

  elder_terms::connect_terminal_layout_signals(app_state.layout_state);

  g_signal_connect(
    main_window->window, "destroy", G_CALLBACK(on_main_window_destroy),
    &app_state);
  g_signal_connect(main_window->settings_button, "clicked",
                   G_CALLBACK(on_settings_button_clicked), &app_state);

  if (app_state.session_state != nullptr &&
    !elder_terms::start_terminal_session(app_state.session_state)) {
    std::cerr << "Warning: failed to start terminal session" << '\n';
  }

  gtk_widget_grab_focus(main_window->terminal);
  gtk_widget_show_all(main_window->window);

  elder_terms::start_terminal_layout(app_state.layout_state);

  dispatcher.park();

  elder_terms::destroy_terminal_layout(app_state.layout_state);
  elder_terms::destroy_terminal_session(app_state.session_state);
  elder_terms::release_main_window(&*main_window);

  return 0;
}
