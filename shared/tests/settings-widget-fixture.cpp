#include <elder-terms/settings-widget.h>

#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <utility>

#include <atk/atk.h>
#include <gtk/gtk.h>

#include <elder-terms/settings.h>
#include <elder-terms/settings/general-settings.h>

namespace elder_terms_settings_widget_fixture {

struct FixtureOptions {
  bool is_runtime = false;
  bool has_save = false;
  bool show_actions = true;
  std::string type = "local";
  glong width = 80;
  glong height = 24;
  gdouble zoom = 1.0;
  bool auto_close = true;
  std::string encoding = "default";
  std::string backspace_code = "default";
  std::string cursor_key_mode = "default";
  std::string zoom_in_key = "ctrl+plus";
  std::string zoom_out_key = "ctrl+minus";
  std::string page = "general";
  std::string telnet_address = "127.0.0.1";
  gint64 telnet_port = 23;
  std::string serial_device = "/dev/ttyUSB0";
  gint64 serial_baudrate = 115200;
  gint64 serial_bits = 8;
  std::string serial_parity = "n";
  gint64 serial_stop_bit = 1;
  std::string serial_flow_control = "none";
  std::string serial_carrier_detect = "cd";
  std::string transfer_base_path;
  gint64 text_send_bytes_per_second = 1024;
  std::string zmodem_autostart = "default";
  bool log_enabled = false;
  std::string log_base_directory = "{XDG_DOCUMENTS}/logs/";
  std::string log_file_name_format = "{YYYYMMDD}_{hhmmss}_{fff}.txt";
  std::string log_mode = "raw";
};

struct FixtureState {
  elder_terms::SettingsWidgetState *settings_widget = nullptr;
};

static bool starts_with(const std::string &value, const std::string &prefix) {
  return value.rfind(prefix, 0) == 0;
}

static std::string option_value(const std::string &argument,
                                const std::string &prefix) {
  return argument.substr(prefix.size());
}

static bool parse_bool(const std::string &value) {
  return value == "1" || value == "true" || value == "yes" ||
         value == "on";
}

static FixtureOptions parse_options(int argc, char **argv) {
  FixtureOptions options;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--runtime") {
      options.is_runtime = true;
    } else if (argument == "--save") {
      options.has_save = true;
    } else if (argument == "--hide-actions") {
      options.show_actions = false;
    } else if (starts_with(argument, "--type=")) {
      options.type = option_value(argument, "--type=");
    } else if (starts_with(argument, "--width=")) {
      options.width = std::stol(option_value(argument, "--width="));
    } else if (starts_with(argument, "--height=")) {
      options.height = std::stol(option_value(argument, "--height="));
    } else if (starts_with(argument, "--zoom=")) {
      options.zoom = std::stod(option_value(argument, "--zoom="));
    } else if (starts_with(argument, "--auto-close=")) {
      options.auto_close = parse_bool(option_value(argument, "--auto-close="));
    } else if (starts_with(argument, "--encoding=")) {
      options.encoding = option_value(argument, "--encoding=");
    } else if (starts_with(argument, "--backspace-code=")) {
      options.backspace_code = option_value(argument, "--backspace-code=");
    } else if (starts_with(argument, "--cursor-key-mode=")) {
      options.cursor_key_mode = option_value(argument, "--cursor-key-mode=");
    } else if (starts_with(argument, "--zoom-in-key=")) {
      options.zoom_in_key = option_value(argument, "--zoom-in-key=");
    } else if (starts_with(argument, "--zoom-out-key=")) {
      options.zoom_out_key = option_value(argument, "--zoom-out-key=");
    } else if (starts_with(argument, "--page=")) {
      options.page = option_value(argument, "--page=");
    } else if (starts_with(argument, "--telnet-address=")) {
      options.telnet_address = option_value(argument, "--telnet-address=");
    } else if (starts_with(argument, "--telnet-port=")) {
      options.telnet_port = std::stoll(option_value(argument, "--telnet-port="));
    } else if (starts_with(argument, "--serial-device=")) {
      options.serial_device = option_value(argument, "--serial-device=");
    } else if (starts_with(argument, "--serial-baudrate=")) {
      options.serial_baudrate =
          std::stoll(option_value(argument, "--serial-baudrate="));
    } else if (starts_with(argument, "--serial-bits=")) {
      options.serial_bits = std::stoll(option_value(argument, "--serial-bits="));
    } else if (starts_with(argument, "--serial-parity=")) {
      options.serial_parity = option_value(argument, "--serial-parity=");
    } else if (starts_with(argument, "--serial-stop-bit=")) {
      options.serial_stop_bit =
          std::stoll(option_value(argument, "--serial-stop-bit="));
    } else if (starts_with(argument, "--serial-flow-control=")) {
      options.serial_flow_control =
          option_value(argument, "--serial-flow-control=");
    } else if (starts_with(argument, "--serial-carrier-detect=")) {
      options.serial_carrier_detect =
          option_value(argument, "--serial-carrier-detect=");
    } else if (starts_with(argument, "--transfer-base-path=")) {
      options.transfer_base_path =
          option_value(argument, "--transfer-base-path=");
    } else if (starts_with(argument, "--text-send-bytes-per-second=")) {
      options.text_send_bytes_per_second =
          std::stoll(option_value(argument, "--text-send-bytes-per-second="));
    } else if (starts_with(argument, "--zmodem-autostart=")) {
      options.zmodem_autostart =
          option_value(argument, "--zmodem-autostart=");
    } else if (starts_with(argument, "--log-enabled=")) {
      options.log_enabled =
          parse_bool(option_value(argument, "--log-enabled="));
    } else if (starts_with(argument, "--log-base-directory=")) {
      options.log_base_directory =
          option_value(argument, "--log-base-directory=");
    } else if (starts_with(argument, "--log-file-name-format=")) {
      options.log_file_name_format =
          option_value(argument, "--log-file-name-format=");
    } else if (starts_with(argument, "--log-mode=")) {
      options.log_mode = option_value(argument, "--log-mode=");
    }
  }
  return options;
}

static void assign_accessible_id(GtkWidget *widget, const char *id) {
  if (widget == nullptr || id == nullptr || id[0] == '\0') {
    return;
  }

  gtk_widget_set_name(widget, id);
  if (g_object_class_find_property(G_OBJECT_GET_CLASS(widget),
                                   "accessible-id") != nullptr) {
    g_object_set(widget, "accessible-id", id, nullptr);
  }

  AtkObject *accessible = gtk_widget_get_accessible(widget);
  if (accessible != nullptr) {
    atk_object_set_accessible_id(accessible, id);
  }
}

static elder_terms::SettingsStore create_store(const FixtureOptions &options) {
  elder_terms::SettingsStore store =
      elder_terms::create_default_settings(
          elder_terms::TerminalDisplaySettings{
              .width = 80,
              .height = 24,
              .zoom = 1.0,
          });
  elder_terms::set_setting_value(
      &store, elder_terms::general_type_setting_key(),
      elder_terms::SettingValue{options.type});
  elder_terms::set_setting_value(
      &store, elder_terms::terminal_width_setting_key(),
      elder_terms::SettingValue{static_cast<gint64>(options.width)});
  elder_terms::set_setting_value(
      &store, elder_terms::terminal_height_setting_key(),
      elder_terms::SettingValue{static_cast<gint64>(options.height)});
  elder_terms::set_setting_value(&store, elder_terms::terminal_zoom_setting_key(),
                                 elder_terms::SettingValue{options.zoom});
  elder_terms::set_setting_value(
      &store, elder_terms::terminal_auto_close_setting_key(),
      elder_terms::SettingValue{options.auto_close});
  if (options.encoding != "default") {
    elder_terms::set_explicit_setting_value(
        &store, elder_terms::terminal_encoding_setting_key(),
        elder_terms::SettingValue{options.encoding});
  }
  if (options.backspace_code != "default") {
    elder_terms::set_explicit_setting_value(
        &store, elder_terms::terminal_backspace_code_setting_key(),
        elder_terms::SettingValue{options.backspace_code});
  }
  if (options.cursor_key_mode != "default") {
    elder_terms::set_explicit_setting_value(
        &store, elder_terms::terminal_cursor_key_mode_setting_key(),
        elder_terms::SettingValue{options.cursor_key_mode});
  }
  elder_terms::set_setting_value(
      &store, elder_terms::terminal_zoom_in_key_setting_key(),
      elder_terms::SettingValue{options.zoom_in_key});
  elder_terms::set_setting_value(
      &store, elder_terms::terminal_zoom_out_key_setting_key(),
      elder_terms::SettingValue{options.zoom_out_key});
  elder_terms::set_setting_value(
      &store, elder_terms::telnet_address_setting_key(),
      elder_terms::SettingValue{options.telnet_address});
  elder_terms::set_setting_value(
      &store, elder_terms::telnet_port_setting_key(),
      elder_terms::SettingValue{options.telnet_port});
  elder_terms::set_setting_value(
      &store, elder_terms::serial_device_setting_key(),
      elder_terms::SettingValue{options.serial_device});
  elder_terms::set_setting_value(
      &store, elder_terms::serial_baudrate_setting_key(),
      elder_terms::SettingValue{options.serial_baudrate});
  elder_terms::set_setting_value(
      &store, elder_terms::serial_bits_setting_key(),
      elder_terms::SettingValue{options.serial_bits});
  elder_terms::set_setting_value(
      &store, elder_terms::serial_parity_setting_key(),
      elder_terms::SettingValue{options.serial_parity});
  elder_terms::set_setting_value(
      &store, elder_terms::serial_stop_bit_setting_key(),
      elder_terms::SettingValue{options.serial_stop_bit});
  elder_terms::set_setting_value(
      &store, elder_terms::serial_flow_control_setting_key(),
      elder_terms::SettingValue{options.serial_flow_control});
  elder_terms::set_setting_value(
      &store, elder_terms::serial_carrier_detect_setting_key(),
      elder_terms::SettingValue{options.serial_carrier_detect});
  elder_terms::set_setting_value(
      &store, elder_terms::transfer_base_path_setting_key(),
      elder_terms::SettingValue{options.transfer_base_path});
  elder_terms::set_setting_value(
      &store, elder_terms::transfer_text_send_bytes_per_second_setting_key(),
      elder_terms::SettingValue{options.text_send_bytes_per_second});
  if (options.zmodem_autostart == "enabled" ||
      options.zmodem_autostart == "true") {
    elder_terms::set_explicit_setting_value(
        &store, elder_terms::transfer_zmodem_autostart_setting_key(),
        elder_terms::SettingValue{true});
  } else if (options.zmodem_autostart == "disabled" ||
             options.zmodem_autostart == "false") {
    elder_terms::set_explicit_setting_value(
        &store, elder_terms::transfer_zmodem_autostart_setting_key(),
        elder_terms::SettingValue{false});
  }
  elder_terms::set_setting_value(
      &store, elder_terms::terminal_log_enabled_setting_key(),
      elder_terms::SettingValue{options.log_enabled});
  elder_terms::set_setting_value(
      &store, elder_terms::terminal_log_base_directory_setting_key(),
      elder_terms::SettingValue{options.log_base_directory});
  elder_terms::set_setting_value(
      &store, elder_terms::terminal_log_file_name_format_setting_key(),
      elder_terms::SettingValue{options.log_file_name_format});
  elder_terms::set_setting_value(
      &store, elder_terms::terminal_log_mode_setting_key(),
      elder_terms::SettingValue{options.log_mode});
  return store;
}

static GtkWidget *find_notebook(GtkWidget *widget) {
  if (widget == nullptr) {
    return nullptr;
  }
  if (GTK_IS_NOTEBOOK(widget)) {
    return widget;
  }
  if (!GTK_IS_CONTAINER(widget)) {
    return nullptr;
  }

  GList *children = gtk_container_get_children(GTK_CONTAINER(widget));
  for (GList *child = children; child != nullptr; child = child->next) {
    GtkWidget *match = find_notebook(GTK_WIDGET(child->data));
    if (match != nullptr) {
      g_list_free(children);
      return match;
    }
  }
  g_list_free(children);
  return nullptr;
}

static void select_initial_page(GtkWidget *window,
                                const std::string &page) {
  GtkWidget *notebook = find_notebook(window);
  if (notebook == nullptr || !GTK_IS_NOTEBOOK(notebook)) {
    return;
  }

  if (page == "terminal") {
    gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), 1);
  } else if (page == "telnet") {
    gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), 2);
  } else if (page == "serial") {
    gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), 3);
  } else if (page == "transfer") {
    gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), 4);
  } else if (page == "logging") {
    gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), 5);
  } else {
    gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), 0);
  }
}

static std::string connection_type_name(
    const elder_terms::TerminalConnectionProfile &profile) {
  if (profile.kind == elder_terms::TerminalConnectionKind::telnet) {
    return "telnet";
  }
  if (profile.kind == elder_terms::TerminalConnectionKind::serial) {
    return "serial";
  }
  return "local";
}

static std::string zmodem_autostart_name(
    const elder_terms::SettingsStore &store) {
  const bool configured = elder_terms::setting_boolean_value_or_default(
      store, elder_terms::transfer_zmodem_autostart_setting_key(), false);
  if (!elder_terms::setting_has_explicit_value(
          store, elder_terms::transfer_zmodem_autostart_setting_key()) &&
      !configured) {
    return "default";
  }
  return configured ? "enabled" : "disabled";
}

static const char *terminal_log_mode_name(
    elder_terms::TerminalLogMode mode) {
  return mode == elder_terms::TerminalLogMode::cooked ? "cooked" : "raw";
}

static void print_store(const char *prefix,
                        const elder_terms::SettingsStore &store) {
  const elder_terms::TerminalDisplaySettings display =
      elder_terms::terminal_display_settings(store);
  const elder_terms::TerminalConnectionProfile profile =
      elder_terms::terminal_connection_profile(store);
  const elder_terms::TelnetConnectionSettings telnet =
      elder_terms::telnet_connection_settings(store);
  const elder_terms::SerialConnectionSettings serial =
      elder_terms::serial_connection_settings(store);
  const elder_terms::TerminalLogSettings log =
      elder_terms::terminal_log_settings(store);
  std::cout << prefix << " type=" << connection_type_name(profile)
            << " width=" << display.width << " height=" << display.height
            << " zoom=" << display.zoom
            << " encoding=" << profile.text_settings.encoding
            << " backspace_code="
            << elder_terms::terminal_backspace_code_to_string(
                   profile.text_settings.backspace_code)
            << " cursor_key_mode="
            << elder_terms::terminal_cursor_key_mode_to_string(
                   profile.text_settings.cursor_key_mode)
            << " auto_close="
            << (elder_terms::terminal_auto_close(store) ? "true" : "false")
            << " zoom_in_key="
            << elder_terms::terminal_zoom_in_key(store)
            << " zoom_out_key="
            << elder_terms::terminal_zoom_out_key(store)
            << " telnet_address=" << telnet.address
            << " telnet_port=" << telnet.port
            << " serial_device=" << serial.device
            << " serial_baudrate=" << serial.baudrate
            << " serial_bits=" << serial.bits
            << " serial_parity="
            << elder_terms::serial_parity_to_string(serial.parity)
            << " serial_stop_bit=" << serial.stop_bit
            << " serial_flow_control="
            << elder_terms::serial_flow_control_to_string(
                   serial.flow_control)
            << " serial_carrier_detect="
            << elder_terms::serial_carrier_detect_to_string(
                   serial.carrier_detect)
            << " transfer_base_path=" << elder_terms::transfer_base_path(store)
            << " text_send_bytes_per_second="
            << elder_terms::transfer_text_send_bytes_per_second(store)
            << " zmodem_autostart=" << zmodem_autostart_name(store)
            << " log_enabled=" << (log.enabled ? "true" : "false")
            << " log_base_directory=" << log.base_directory
            << " log_file_name_format=" << log.file_name_format
            << " log_mode=" << terminal_log_mode_name(log.mode)
            << '\n';
  std::cout.flush();
}

static void on_window_destroy(GtkWidget *, gpointer data) {
  auto *state = static_cast<FixtureState *>(data);
  if (state->settings_widget != nullptr) {
    elder_terms::destroy_settings_widget(state->settings_widget);
    state->settings_widget = nullptr;
  }
  gtk_main_quit();
}

} // namespace elder_terms_settings_widget_fixture

int main(int argc, char **argv) {
  try {
    gtk_init(&argc, &argv);
    const elder_terms_settings_widget_fixture::FixtureOptions options =
        elder_terms_settings_widget_fixture::parse_options(argc, argv);
    elder_terms_settings_widget_fixture::FixtureState state;

    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    elder_terms_settings_widget_fixture::assign_accessible_id(
        window, "settings_widget_test_window");
    gtk_window_set_title(GTK_WINDOW(window), "Settings Widget Fixture");
    gtk_window_set_default_size(GTK_WINDOW(window), 720, 456);

    elder_terms::SettingsWidgetCallbacks callbacks;
    callbacks.apply =
        [](const elder_terms::SettingsStore &store) {
          elder_terms_settings_widget_fixture::print_store("APPLIED", store);
        };
    if (options.has_save) {
      callbacks.save =
          [](const elder_terms::SettingsStore &store) {
            elder_terms_settings_widget_fixture::print_store("SAVED", store);
            return true;
          };
    }
    callbacks.cancel =
        []() {
          std::cout << "CANCELLED\n";
          std::cout.flush();
        };
    callbacks.changed = [&state]() {
      const elder_terms::SettingsStore draft =
          elder_terms::settings_widget_draft_store(state.settings_widget);
      const elder_terms::TerminalDisplaySettings display =
          elder_terms::terminal_display_settings(draft);
      std::cout << "CHANGED dirty="
                << (elder_terms::settings_widget_is_dirty(
                        state.settings_widget)
                        ? "true"
                        : "false")
                << " valid="
                << (elder_terms::settings_widget_is_valid(
                        state.settings_widget)
                        ? "true"
                        : "false")
                << " width=" << display.width << '\n';
      std::cout.flush();
    };

    elder_terms::SettingsWidgetOptions widget_options{
        .store = elder_terms_settings_widget_fixture::create_store(options),
        .is_runtime = options.is_runtime,
        .show_actions = options.show_actions,
        .callbacks = std::move(callbacks),
    };
    state.settings_widget =
        elder_terms::create_settings_widget(std::move(widget_options));
    gtk_container_add(GTK_CONTAINER(window),
                      elder_terms::settings_widget_root(state.settings_widget));
    elder_terms_settings_widget_fixture::select_initial_page(window,
                                                             options.page);
    g_signal_connect(window, "destroy", G_CALLBACK(
                         elder_terms_settings_widget_fixture::on_window_destroy),
                     &state);

    gtk_widget_show_all(window);
    elder_terms_settings_widget_fixture::select_initial_page(window,
                                                             options.page);
    while (gtk_events_pending() != FALSE) {
      gtk_main_iteration_do(FALSE);
    }
    std::cout << "READY\n";
    std::cout.flush();
    gtk_main();
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
