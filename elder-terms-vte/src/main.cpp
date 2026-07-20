#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtk/gtk.h>
#include <vte/vte.h>

#include <cardio.h>
#include <gestament/gtk.h>

#include <elder-terms/settings.h>
#include <elder-terms/settings-widget.h>

#include "launch-options.h"
#include "main-window.h"
#include "terminal-layout.h"
#include "terminal-log.h"
#include "terminal-transfer-runner.h"
#include "terminal-session.h"

struct ApplicationState {
  elder_terms::MainWindow *main_window = nullptr;
  elder_terms::TerminalSessionState *session_state = nullptr;
  elder_terms::TerminalLayoutState *layout_state = nullptr;
  elder_terms::TerminalLogState *log_state = nullptr;
  cardio::dispatcher_group_glib *dispatcher_group = nullptr;
  std::optional<cardio::promise<void>> shutdown_task;
  elder_terms::SettingsStore settings_store;
  std::optional<std::filesystem::path> config_path;
  elder_terms::TestOptions test_options;
  bool auto_close = true;
  bool connection_active = false;
  bool transfer_active = false;
  GtkWidget *window = nullptr;
  GtkWidget *settings_dialog = nullptr;
  guint settings_dialog_close_idle_id = 0;
  elder_terms::SettingsWidgetState *settings_widget = nullptr;
};

struct TransferMenuAction {
  elder_terms::TerminalTransferProtocol protocol =
      elder_terms::TerminalTransferProtocol::zmodem;
  elder_terms::TerminalTransferDirection direction =
      elder_terms::TerminalTransferDirection::send;
  elder_terms::TerminalTransferOptions options;
};

static void close_settings_dialog(ApplicationState *state);
static void schedule_settings_dialog_close(ApplicationState *state);
static void restore_terminal_focus(ApplicationState *state);

static cardio::promise<void>
stop_application_async(ApplicationState *state) {
  co_await elder_terms::stop_terminal_log_async(state->log_state);
  state->dispatcher_group->shutdown();
}

static void update_application_terminal_presentation(
    ApplicationState *state) {
  if (state == nullptr) {
    return;
  }

  elder_terms::set_main_window_terminal_interactive(
      state->main_window,
      state->connection_active && !state->transfer_active &&
          state->settings_dialog == nullptr);
  elder_terms::set_main_window_transfer_button_sensitive(
      state->main_window,
      state->connection_active && !state->transfer_active);
}

static void set_application_transfer_progress_visible(ApplicationState *state,
                                                      bool active) {
  if (state == nullptr) {
    return;
  }
  elder_terms::set_main_window_transfer_progress_visible(state->main_window,
                                                         active);
}

static void set_application_transfer_active(ApplicationState *state,
                                            bool active) {
  if (state == nullptr) {
    return;
  }

  state->transfer_active = active;
  update_application_terminal_presentation(state);
}

static void set_application_indicator_state(
    ApplicationState *state, elder_terms::ActivityIndicatorId indicator, bool active) {
  if (state == nullptr) {
    return;
  }

  if (indicator == elder_terms::ActivityIndicatorId::conn &&
      state->auto_close && !active) {
    return;
  }

  if (indicator == elder_terms::ActivityIndicatorId::conn) {
    state->connection_active = active;
    elder_terms::set_main_window_connection_active(state->main_window, active);
    update_application_terminal_presentation(state);
    return;
  }

  elder_terms::set_main_window_indicator_state(state->main_window, indicator, active);
}

static void restore_terminal_focus(ApplicationState *state) {
  if (state == nullptr || state->window == nullptr ||
      state->main_window == nullptr || state->main_window->terminal == nullptr) {
    return;
  }

  if (!gtk_widget_get_visible(state->window) ||
      !gtk_widget_get_visible(state->main_window->terminal)) {
    return;
  }

  gtk_window_present(GTK_WINDOW(state->window));
  gtk_widget_grab_focus(state->main_window->terminal);
}

static void on_main_window_destroy(GtkWidget *, gpointer user_data) {
  auto *state = static_cast<ApplicationState *>(user_data);
  state->window = nullptr;
  elder_terms::set_main_window_transfer_progress_visible(state->main_window,
                                                         false);
  elder_terms::deactivate_main_window_activity_indicators(state->main_window);
  close_settings_dialog(state);
  elder_terms::set_terminal_log_connection_active(state->log_state, false);
  elder_terms::stop_terminal_session(state->session_state);
  if (!state->shutdown_task.has_value()) {
    state->shutdown_task.emplace(stop_application_async(state));
  }
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
  update_application_terminal_presentation(state);
  restore_terminal_focus(state);
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
  elder_terms::apply_terminal_log_settings(
      state->log_state,
      elder_terms::terminal_log_settings(state->settings_store));
  state->auto_close = elder_terms::terminal_auto_close(state->settings_store);
  const auto connection_profile =
      elder_terms::terminal_connection_profile(state->settings_store);
  elder_terms::set_main_window_activity_indicator_connection_kind(
      state->main_window, connection_profile.kind);
  elder_terms::apply_terminal_session_connection_profile(
      state->session_state, connection_profile);
  elder_terms::set_terminal_session_zmodem_autostart(
      state->session_state,
      elder_terms::transfer_zmodem_autostart(state->settings_store));
  elder_terms::set_main_window_transfer_button_visible(
      state->main_window,
      elder_terms::terminal_session_supports_transfer(state->session_state));
  update_application_terminal_presentation(state);
  const std::string title =
      elder_terms::terminal_session_title(state->session_state);
  elder_terms::set_main_window_title(state->main_window, title);
  elder_terms::apply_terminal_display_settings(
      state->layout_state,
      elder_terms::terminal_display_settings(state->settings_store));
  elder_terms::apply_terminal_key_bindings(
      state->layout_state,
      elder_terms::terminal_key_bindings(state->settings_store));
}

static bool save_runtime_settings(
  ApplicationState *state, const elder_terms::SettingsStore &store) {
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
    elder_terms::update_settings_widget_store(
      state->settings_widget, state->settings_store);
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
  g_signal_connect(dialog, "destroy", G_CALLBACK(on_settings_dialog_destroy), state);
  update_application_terminal_presentation(state);
  gtk_widget_show_all(dialog);
}

static void on_settings_button_clicked(GtkButton *, gpointer user_data) {
  open_settings_dialog(static_cast<ApplicationState *>(user_data));
}

static gboolean emit_transfer_dialog_current_folder_uri(gpointer data) {
  auto *chooser = GTK_FILE_CHOOSER(data);
  char *uri = gtk_file_chooser_get_current_folder_uri(chooser);
  std::cout << "ELDER_TERMS_TRANSFER_DIALOG_CURRENT_FOLDER_URI="
            << (uri == nullptr ? "" : uri) << '\n'
            << std::flush;
  if (uri != nullptr) {
    g_free(uri);
  }
  return G_SOURCE_REMOVE;
}

static void schedule_transfer_dialog_probe(GtkFileChooser *chooser) {
  g_idle_add_full(G_PRIORITY_DEFAULT_IDLE,
                  emit_transfer_dialog_current_folder_uri,
                  g_object_ref(chooser), g_object_unref);
}

static void set_transfer_dialog_initial_folder(ApplicationState *state,
                                               GtkFileChooser *chooser) {
  const std::string base_path =
      elder_terms::transfer_base_path(state->settings_store);
  const std::string folder_uri =
      elder_terms::resolve_transfer_base_path_uri(base_path);
  if (folder_uri.empty()) {
    return;
  }

  if (!gtk_file_chooser_set_current_folder_uri(chooser, folder_uri.c_str())) {
    std::cerr << "Warning: failed to set transfer file dialog folder: "
              << folder_uri << '\n';
  }
}

static std::vector<std::string> choose_transfer_files(
    ApplicationState *state, elder_terms::TerminalTransferProtocol protocol) {
  std::vector<std::string> uris;
  if (!state->test_options.transfer_source_uris.empty()) {
    uris = state->test_options.transfer_source_uris;
    return uris;
  }

  GtkFileChooserNative *dialog = gtk_file_chooser_native_new(
      "Select file", GTK_WINDOW(state->window), GTK_FILE_CHOOSER_ACTION_OPEN,
      "Open", "Cancel");
  GtkFileChooser *chooser = GTK_FILE_CHOOSER(dialog);
  gtk_file_chooser_set_local_only(chooser, FALSE);
  gtk_file_chooser_set_select_multiple(
      chooser,
      protocol != elder_terms::TerminalTransferProtocol::xmodem);
  set_transfer_dialog_initial_folder(state, chooser);
  if (state->test_options.transfer_dialog_probe) {
    schedule_transfer_dialog_probe(chooser);
  }

  const gint response = gtk_native_dialog_run(GTK_NATIVE_DIALOG(dialog));
  if (response == GTK_RESPONSE_ACCEPT) {
    GSList *files = gtk_file_chooser_get_files(chooser);
    for (GSList *item = files; item != nullptr; item = item->next) {
      GFile *file = G_FILE(item->data);
      char *uri = g_file_get_uri(file);
      if (uri != nullptr) {
        uris.emplace_back(uri);
        g_free(uri);
      }
      g_object_unref(file);
    }
    g_slist_free(files);
  }

  g_object_unref(dialog);
  return uris;
}

static bool start_transfer_request(ApplicationState *state,
                                   const TransferMenuAction &action,
                                   std::vector<std::string> source_uris) {
  elder_terms::TerminalTransferRequest request{
      .protocol = action.protocol,
      .direction = action.direction,
      .base_path = elder_terms::transfer_base_path(state->settings_store),
      .source_file_uris = std::move(source_uris),
      .options = action.options,
      .active =
          [state](bool active) {
            set_application_transfer_active(state, active);
            set_application_transfer_progress_visible(state, active);
          },
      .status =
          [state](const std::string &status) {
            elder_terms::set_main_window_status_text(state->main_window,
                                                     status);
          },
      .progress =
          [state](elder_terms::TerminalTransferProgress progress) {
            elder_terms::set_main_window_transfer_progress(state->main_window,
                                                           progress);
          },
      .finished =
          [state](bool) {
            restore_terminal_focus(state);
          },
  };

  return elder_terms::start_terminal_session_transfer(state->session_state,
                                                      std::move(request));
}

static void on_transfer_menu_item_activate(GtkMenuItem *item,
                                           gpointer user_data) {
  auto *state = static_cast<ApplicationState *>(user_data);
  const auto *action = static_cast<const TransferMenuAction *>(
      g_object_get_data(G_OBJECT(item), "elder-terms-transfer-action"));
  if (state == nullptr || action == nullptr) {
    return;
  }

  std::vector<std::string> source_uris;
  if (action->direction == elder_terms::TerminalTransferDirection::send) {
    source_uris = choose_transfer_files(state, action->protocol);
    if (source_uris.empty()) {
      return;
    }
  }

  if (!start_transfer_request(state, *action, std::move(source_uris))) {
    elder_terms::set_main_window_status_text(state->main_window,
                                             "Transfer unavailable");
  }
}

static void start_zmodem_auto_transfer(
    ApplicationState *state,
    elder_terms::TerminalTransferDirection direction) {
  if (state == nullptr || !state->connection_active || state->transfer_active ||
      !elder_terms::transfer_zmodem_autostart(state->settings_store)) {
    return;
  }

  TransferMenuAction action{
      .protocol = elder_terms::TerminalTransferProtocol::zmodem,
      .direction = direction,
      .options = elder_terms::TerminalTransferOptions{},
  };

  std::vector<std::string> source_uris;
  if (direction == elder_terms::TerminalTransferDirection::send) {
    source_uris = choose_transfer_files(
        state, elder_terms::TerminalTransferProtocol::zmodem);
    if (source_uris.empty()) {
      return;
    }
  }

  if (!start_transfer_request(state, action, std::move(source_uris))) {
    elder_terms::set_main_window_status_text(state->main_window,
                                             "Transfer unavailable");
  }
}

static elder_terms::TerminalTransferOptions make_xmodem_transfer_options(
    elder_terms::TerminalTransferXmodemPacketSize packet_size,
    elder_terms::TerminalTransferXmodemChecksumMode checksum_mode) {
  elder_terms::TerminalTransferOptions options;
  options.xmodem_packet_size = packet_size;
  options.xmodem_checksum_mode = checksum_mode;
  return options;
}

static elder_terms::TerminalTransferOptions make_ymodem_transfer_options(
    elder_terms::TerminalTransferYmodemVariant variant) {
  elder_terms::TerminalTransferOptions options;
  options.ymodem_variant = variant;
  return options;
}

static GtkWidget *create_transfer_menu_item(
    ApplicationState *state, const char *id, const char *label,
    elder_terms::TerminalTransferProtocol protocol,
    elder_terms::TerminalTransferDirection direction,
    elder_terms::TerminalTransferOptions options) {
  GtkWidget *item = gtk_menu_item_new_with_label(label);
  gestament_gtk_assign_accessible_id(item, id);
  auto *action = new TransferMenuAction{
      .protocol = protocol,
      .direction = direction,
      .options = options,
  };
  g_object_set_data_full(G_OBJECT(item), "elder-terms-transfer-action",
                         action,
                         [](gpointer data) {
                           delete static_cast<TransferMenuAction *>(data);
                         });
  g_signal_connect(item, "activate",
                   G_CALLBACK(on_transfer_menu_item_activate), state);
  return item;
}

static void install_transfer_menu(ApplicationState *state) {
  GtkWidget *menu = gtk_menu_new();

  gtk_menu_shell_append(
      GTK_MENU_SHELL(menu),
      create_transfer_menu_item(
          state, "transfer_zmodem_send_item", "ZMODEM (send)",
          elder_terms::TerminalTransferProtocol::zmodem,
          elder_terms::TerminalTransferDirection::send,
          elder_terms::TerminalTransferOptions{}));
  gtk_menu_shell_append(
      GTK_MENU_SHELL(menu),
      create_transfer_menu_item(
          state, "transfer_ymodem_send_item", "YMODEM (send)",
          elder_terms::TerminalTransferProtocol::ymodem,
          elder_terms::TerminalTransferDirection::send,
          elder_terms::TerminalTransferOptions{}));
  gtk_menu_shell_append(
      GTK_MENU_SHELL(menu),
      create_transfer_menu_item(
          state, "transfer_xmodem_1k_send_item", "XMODEM 1K (send)",
          elder_terms::TerminalTransferProtocol::xmodem,
          elder_terms::TerminalTransferDirection::send,
          make_xmodem_transfer_options(
              elder_terms::TerminalTransferXmodemPacketSize::bytes_1024,
              elder_terms::TerminalTransferXmodemChecksumMode::automatic)));
  gtk_menu_shell_append(
      GTK_MENU_SHELL(menu),
      create_transfer_menu_item(
          state, "transfer_xmodem_send_item", "XMODEM (send)",
          elder_terms::TerminalTransferProtocol::xmodem,
          elder_terms::TerminalTransferDirection::send,
          make_xmodem_transfer_options(
              elder_terms::TerminalTransferXmodemPacketSize::bytes_128,
              elder_terms::TerminalTransferXmodemChecksumMode::automatic)));
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
  gtk_menu_shell_append(
      GTK_MENU_SHELL(menu),
      create_transfer_menu_item(
          state, "transfer_zmodem_receive_item", "ZMODEM (receive)",
          elder_terms::TerminalTransferProtocol::zmodem,
          elder_terms::TerminalTransferDirection::receive,
          elder_terms::TerminalTransferOptions{}));
  gtk_menu_shell_append(
      GTK_MENU_SHELL(menu),
      create_transfer_menu_item(
          state, "transfer_ymodem_g_receive_item", "YMODEM-g (receive)",
          elder_terms::TerminalTransferProtocol::ymodem,
          elder_terms::TerminalTransferDirection::receive,
          make_ymodem_transfer_options(
              elder_terms::TerminalTransferYmodemVariant::g)));
  gtk_menu_shell_append(
      GTK_MENU_SHELL(menu),
      create_transfer_menu_item(
          state, "transfer_ymodem_receive_item", "YMODEM (receive)",
          elder_terms::TerminalTransferProtocol::ymodem,
          elder_terms::TerminalTransferDirection::receive,
          make_ymodem_transfer_options(
              elder_terms::TerminalTransferYmodemVariant::standard)));
  gtk_menu_shell_append(
      GTK_MENU_SHELL(menu),
      create_transfer_menu_item(
          state, "transfer_xmodem_crc_receive_item", "XMODEM CRC (receive)",
          elder_terms::TerminalTransferProtocol::xmodem,
          elder_terms::TerminalTransferDirection::receive,
          make_xmodem_transfer_options(
              elder_terms::TerminalTransferXmodemPacketSize::bytes_128,
              elder_terms::TerminalTransferXmodemChecksumMode::crc)));
  gtk_menu_shell_append(
      GTK_MENU_SHELL(menu),
      create_transfer_menu_item(
          state, "transfer_xmodem_receive_item", "XMODEM (receive)",
          elder_terms::TerminalTransferProtocol::xmodem,
          elder_terms::TerminalTransferDirection::receive,
          make_xmodem_transfer_options(
              elder_terms::TerminalTransferXmodemPacketSize::bytes_128,
              elder_terms::TerminalTransferXmodemChecksumMode::checksum)));
  gtk_widget_show_all(menu);
  gtk_menu_button_set_popup(GTK_MENU_BUTTON(state->main_window->transfer_button),
                            menu);
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
  elder_terms::set_main_window_activity_indicators_latched(
      &*main_window, launch_options.test.latch_activity_indicators);

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
      .main_window = &*main_window,
      .session_state = nullptr,
      .layout_state = nullptr,
      .log_state = nullptr,
      .dispatcher_group = &dispatcher_group,
      .shutdown_task = std::nullopt,
      .settings_store = settings_result.store,
      .config_path = launch_options.config_path,
      .test_options = launch_options.test,
      .auto_close = elder_terms::terminal_auto_close(settings_result.store),
      .connection_active = false,
      .transfer_active = false,
      .window = main_window->window,
      .settings_dialog = nullptr,
      .settings_dialog_close_idle_id = 0,
      .settings_widget = nullptr,
  };

  app_state.log_state = elder_terms::create_terminal_log({
      .settings = elder_terms::terminal_log_settings(app_state.settings_store),
      .now = {},
      .active = [&app_state](bool active) {
        set_application_indicator_state(
            &app_state, elder_terms::ActivityIndicatorId::log, active);
      },
      .warning = {},
  });

  const auto terminal_display_settings =
    elder_terms::terminal_display_settings(app_state.settings_store);
  const auto terminal_key_bindings =
    elder_terms::terminal_key_bindings(app_state.settings_store);
  const auto connection_profile =
    elder_terms::terminal_connection_profile(app_state.settings_store);
  elder_terms::set_main_window_activity_indicator_connection_kind(
      &*main_window, connection_profile.kind);

  vte_terminal_set_font_scale(
    vte_terminal, terminal_display_settings.zoom);
  vte_terminal_set_size(
    vte_terminal, terminal_display_settings.width, terminal_display_settings.height);

  app_state.session_state = elder_terms::create_terminal_session(
    main_window->terminal,
    connection_profile,
    {
      .ended = [&app_state]() {
        elder_terms::set_terminal_log_connection_active(app_state.log_state,
                                                        false);
        if (!app_state.auto_close) {
          set_application_indicator_state(
              &app_state, elder_terms::ActivityIndicatorId::conn, false);
        }
        if (app_state.auto_close && app_state.window != nullptr) {
          gtk_widget_destroy(app_state.window);
        }
      },
      .activity = [&main_window](elder_terms::ActivityIndicatorId indicator) {
        elder_terms::note_main_window_activity(&*main_window, indicator);
      },
      .indicator_state =
          [&app_state](elder_terms::ActivityIndicatorId indicator, bool active) {
            if (indicator == elder_terms::ActivityIndicatorId::conn) {
              elder_terms::set_terminal_log_connection_active(
                  app_state.log_state, active);
            }
            set_application_indicator_state(&app_state, indicator, active);
          },
      .output =
          [&app_state](std::span<const unsigned char> raw_bytes,
                       std::span<const unsigned char> cooked_bytes) {
            elder_terms::write_terminal_log(app_state.log_state, raw_bytes,
                                            cooked_bytes);
          },
      .zmodem_auto_start =
          [&app_state](elder_terms::TerminalTransferDirection direction) {
            start_zmodem_auto_transfer(&app_state, direction);
          },
    });
  elder_terms::set_terminal_session_zmodem_autostart(
      app_state.session_state,
      elder_terms::transfer_zmodem_autostart(app_state.settings_store));
  install_transfer_menu(&app_state);
  elder_terms::set_main_window_transfer_button_visible(
      &*main_window,
      elder_terms::terminal_session_supports_transfer(
          app_state.session_state));
  update_application_terminal_presentation(&app_state);
  const std::string title =
      elder_terms::terminal_session_title(app_state.session_state);
  elder_terms::set_main_window_title(&*main_window, title);

  app_state.layout_state = elder_terms::create_terminal_layout(
    *main_window, launch_options.test, terminal_display_settings,
    terminal_key_bindings,
    {
      .grid_size_changed = [&app_state](glong columns, glong rows) {
        elder_terms::resize_terminal_session(
          app_state.session_state, columns, rows);
      },
      .display_settings_changed =
          [&app_state](
              elder_terms::TerminalDisplaySettings terminal_settings) {
            update_runtime_terminal_display_settings(
              &app_state, terminal_settings);
          },
    });

  elder_terms::connect_terminal_layout_signals(app_state.layout_state);

  g_signal_connect(
    main_window->window, "destroy",
    G_CALLBACK(on_main_window_destroy), &app_state);
  g_signal_connect(
    main_window->settings_button, "clicked",
    G_CALLBACK(on_settings_button_clicked), &app_state);

  bool session_started = false;
  if (!launch_options.test.fixture && app_state.session_state != nullptr) {
    session_started =
        elder_terms::start_terminal_session(app_state.session_state);
    if (!session_started) {
      std::cerr << "Warning: failed to start terminal session" << '\n';
    }
  }
  if (launch_options.test.fixture) {
    set_application_indicator_state(&app_state,
                                    elder_terms::ActivityIndicatorId::conn,
                                    true);
  } else if (session_started && app_state.auto_close) {
    set_application_indicator_state(&app_state,
                                    elder_terms::ActivityIndicatorId::conn,
                                    true);
  }

  gtk_widget_grab_focus(main_window->terminal);
  gtk_widget_show_all(main_window->window);

  elder_terms::start_terminal_layout(app_state.layout_state);

  dispatcher.park();

  elder_terms::destroy_terminal_layout(app_state.layout_state);
  elder_terms::destroy_terminal_session(app_state.session_state);
  app_state.shutdown_task.reset();
  elder_terms::destroy_terminal_log(app_state.log_state);
  elder_terms::release_main_window(&*main_window);

  return 0;
}
