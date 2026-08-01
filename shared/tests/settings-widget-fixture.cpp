#include <elder-terms/settings-widget.h>

#include <clocale>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <atk/atk.h>
#include <gtk/gtk.h>

#include <elder-terms/localization.h>
#include <elder-terms/settings.h>
#include <elder-terms/settings/general-settings.h>

namespace elder_terms_settings_widget_fixture {

struct ConfigAssignment {
  std::string section;
  std::string key;
  std::string value;
};

struct FixtureOptions {
  bool is_runtime = false;
  bool has_save = false;
  bool show_actions = true;
  bool global_mode = false;
  std::string page = "general";
  std::vector<ConfigAssignment> connection_assignments;
  std::vector<ConfigAssignment> global_assignments;
  std::vector<ConfigAssignment> rebase_global_assignments;
};

struct FixtureState {
  elder_terms::SettingsWidgetState *settings_widget = nullptr;
  std::optional<elder_terms::SettingsStore> rebase_store;
  GtkWidget *window = nullptr;
};

static bool starts_with(const std::string &value, const std::string &prefix) {
  return value.rfind(prefix, 0) == 0;
}

static std::string option_value(const std::string &argument,
                                const std::string &prefix) {
  return argument.substr(prefix.size());
}

static ConfigAssignment parse_assignment(const std::string &argument) {
  const std::size_t equals = argument.find('=');
  const std::size_t period = argument.find('.');
  if (period == std::string::npos || equals == std::string::npos ||
      period == 0 || period + 1 >= equals) {
    throw std::invalid_argument(
        "setting assignment must use section.key=value: " + argument);
  }
  return {
      .section = argument.substr(0, period),
      .key = argument.substr(period + 1, equals - period - 1),
      .value = argument.substr(equals + 1),
  };
}

static void append_connection_assignment(
    FixtureOptions *options, const char *section, const char *key,
    const std::string &value) {
  options->connection_assignments.push_back({
      .section = section,
      .key = key,
      .value = value,
  });
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
    } else if (argument == "--global-mode") {
      options.global_mode = true;
    } else if (starts_with(argument, "--global=")) {
      options.global_assignments.push_back(
          parse_assignment(option_value(argument, "--global=")));
    } else if (starts_with(argument, "--rebase-global=")) {
      options.rebase_global_assignments.push_back(
          parse_assignment(option_value(argument, "--rebase-global=")));
    } else if (starts_with(argument, "--type=")) {
      append_connection_assignment(
          &options, "general", "type", option_value(argument, "--type="));
    } else if (starts_with(argument, "--open-connection=")) {
      append_connection_assignment(
          &options, "general", "open_connection",
          option_value(argument, "--open-connection="));
    } else if (starts_with(argument, "--width=")) {
      append_connection_assignment(
          &options, "terminal", "width", option_value(argument, "--width="));
    } else if (starts_with(argument, "--height=")) {
      append_connection_assignment(
          &options, "terminal", "height", option_value(argument, "--height="));
    } else if (starts_with(argument, "--zoom=")) {
      append_connection_assignment(
          &options, "terminal", "zoom", option_value(argument, "--zoom="));
    } else if (starts_with(argument, "--auto-close=")) {
      append_connection_assignment(
          &options, "terminal", "auto_close",
          option_value(argument, "--auto-close="));
    } else if (starts_with(argument, "--exterior-background=")) {
      append_connection_assignment(
          &options, "general", "exterior_background",
          option_value(argument, "--exterior-background="));
    } else if (starts_with(argument, "--background=")) {
      append_connection_assignment(
          &options, "general", "background",
          option_value(argument, "--background="));
    } else if (starts_with(argument, "--encoding=")) {
      append_connection_assignment(
          &options, "terminal", "encoding",
          option_value(argument, "--encoding="));
    } else if (starts_with(argument, "--backspace-code=")) {
      append_connection_assignment(
          &options, "terminal", "backspace_code",
          option_value(argument, "--backspace-code="));
    } else if (starts_with(argument, "--cursor-key-mode=")) {
      append_connection_assignment(
          &options, "terminal", "cursor_key_mode",
          option_value(argument, "--cursor-key-mode="));
    } else if (starts_with(argument, "--zoom-in-key=")) {
      append_connection_assignment(
          &options, "terminal", "zoom_in_key",
          option_value(argument, "--zoom-in-key="));
    } else if (starts_with(argument, "--zoom-out-key=")) {
      append_connection_assignment(
          &options, "terminal", "zoom_out_key",
          option_value(argument, "--zoom-out-key="));
    } else if (starts_with(argument, "--page=")) {
      options.page = option_value(argument, "--page=");
    } else if (starts_with(argument, "--telnet-address=")) {
      append_connection_assignment(
          &options, "telnet", "address",
          option_value(argument, "--telnet-address="));
    } else if (starts_with(argument, "--telnet-port=")) {
      append_connection_assignment(
          &options, "telnet", "port",
          option_value(argument, "--telnet-port="));
    } else if (starts_with(argument, "--telnet-terminal-type=")) {
      append_connection_assignment(
          &options, "telnet", "terminal_type",
          option_value(argument, "--telnet-terminal-type="));
    } else if (starts_with(argument, "--ssh-address=")) {
      append_connection_assignment(
          &options, "ssh", "address",
          option_value(argument, "--ssh-address="));
    } else if (starts_with(argument, "--ssh-port=")) {
      append_connection_assignment(
          &options, "ssh", "port", option_value(argument, "--ssh-port="));
    } else if (starts_with(argument, "--ssh-username=")) {
      append_connection_assignment(
          &options, "ssh", "username",
          option_value(argument, "--ssh-username="));
    } else if (starts_with(argument, "--ssh-identity-file=")) {
      append_connection_assignment(
          &options, "ssh", "identity_file",
          option_value(argument, "--ssh-identity-file="));
    } else if (starts_with(argument, "--ssh-terminal-type=")) {
      append_connection_assignment(
          &options, "ssh", "terminal_type",
          option_value(argument, "--ssh-terminal-type="));
    } else if (starts_with(argument, "--sftp-local-directory=")) {
      append_connection_assignment(
          &options, "sftp", "local_directory",
          option_value(argument, "--sftp-local-directory="));
    } else if (starts_with(argument, "--sftp-remote-directory=")) {
      append_connection_assignment(
          &options, "sftp", "remote_directory",
          option_value(argument, "--sftp-remote-directory="));
    } else if (starts_with(argument, "--serial-device=")) {
      append_connection_assignment(
          &options, "serial", "device",
          option_value(argument, "--serial-device="));
    } else if (starts_with(argument, "--serial-baudrate=")) {
      append_connection_assignment(
          &options, "serial", "baudrate",
          option_value(argument, "--serial-baudrate="));
    } else if (starts_with(argument, "--serial-bits=")) {
      append_connection_assignment(
          &options, "serial", "bits",
          option_value(argument, "--serial-bits="));
    } else if (starts_with(argument, "--serial-parity=")) {
      append_connection_assignment(
          &options, "serial", "parity",
          option_value(argument, "--serial-parity="));
    } else if (starts_with(argument, "--serial-stop-bit=")) {
      append_connection_assignment(
          &options, "serial", "stop_bit",
          option_value(argument, "--serial-stop-bit="));
    } else if (starts_with(argument, "--serial-flow-control=")) {
      append_connection_assignment(
          &options, "serial", "flow_control",
          option_value(argument, "--serial-flow-control="));
    } else if (starts_with(argument, "--serial-carrier-detect=")) {
      append_connection_assignment(
          &options, "serial", "carrier_detect",
          option_value(argument, "--serial-carrier-detect="));
    } else if (starts_with(argument, "--transfer-base-path=")) {
      append_connection_assignment(
          &options, "transfer", "base_path",
          option_value(argument, "--transfer-base-path="));
    } else if (starts_with(argument, "--text-send-bytes-per-second=")) {
      append_connection_assignment(
          &options, "transfer", "text_send_bytes_per_second",
          option_value(argument, "--text-send-bytes-per-second="));
    } else if (starts_with(argument, "--zmodem-autostart=")) {
      const std::string value =
          option_value(argument, "--zmodem-autostart=");
      append_connection_assignment(
          &options, "transfer", "zmodem_autostart",
          value == "enabled" ? "true"
                             : value == "disabled" ? "false" : value);
    } else if (starts_with(argument, "--log-enabled=")) {
      append_connection_assignment(
          &options, "log", "enabled",
          option_value(argument, "--log-enabled="));
    } else if (starts_with(argument, "--log-base-directory=")) {
      append_connection_assignment(
          &options, "log", "base_directory",
          option_value(argument, "--log-base-directory="));
    } else if (starts_with(argument, "--log-file-name-format=")) {
      append_connection_assignment(
          &options, "log", "file_name_format",
          option_value(argument, "--log-file-name-format="));
    } else if (starts_with(argument, "--log-mode=")) {
      append_connection_assignment(
          &options, "log", "mode",
          option_value(argument, "--log-mode="));
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

static void load_assignments(
    elder_terms::SettingsStore *store,
    const std::vector<ConfigAssignment> &assignments) {
  GKeyFile *key_file = g_key_file_new();
  for (const ConfigAssignment &assignment : assignments) {
    g_key_file_set_value(key_file, assignment.section.c_str(),
                         assignment.key.c_str(), assignment.value.c_str());
  }
  std::vector<std::string> warnings;
  elder_terms::load_settings_store_from_key_file(store, key_file, &warnings);
  g_key_file_free(key_file);
  if (!warnings.empty()) {
    throw std::invalid_argument(warnings.front());
  }
}

static elder_terms::SettingsStore create_default_store() {
  elder_terms::SettingsStore store =
      elder_terms::create_default_settings(
          elder_terms::TerminalDisplaySettings{
              .width = 80,
              .height = 24,
              .zoom = 1.0,
          },
          "fixture");
  return store;
}

static elder_terms::SettingsStore
create_global_store(const std::vector<ConfigAssignment> &assignments) {
  const std::filesystem::path missing_path =
      std::filesystem::temp_directory_path() /
      "elder-terms-settings-widget-fixture-missing-global.ini";
  elder_terms::SettingsStore store =
      elder_terms::load_global_settings(missing_path, 1.0).store;
  load_assignments(&store, assignments);
  return store;
}

static elder_terms::SettingsStore create_store(const FixtureOptions &options) {
  elder_terms::SettingsStore store = create_default_store();
  const elder_terms::SettingsStore global_store =
      create_global_store(options.global_assignments);
  elder_terms::rebase_settings_store_fallbacks(&store, global_store);

  std::vector<ConfigAssignment> connection_assignments =
      options.connection_assignments;
  connection_assignments.push_back({
      .section = "general",
      .key = "name",
      .value = "fixture",
  });
  load_assignments(&store, connection_assignments);
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

static GtkWidget *find_widget_by_name(GtkWidget *widget,
                                      const std::string &name) {
  if (widget == nullptr) {
    return nullptr;
  }
  const char *widget_name = gtk_widget_get_name(widget);
  if (widget_name != nullptr && widget_name == name) {
    return widget;
  }
  if (!GTK_IS_CONTAINER(widget)) {
    return nullptr;
  }

  GList *children = gtk_container_get_children(GTK_CONTAINER(widget));
  for (GList *child = children; child != nullptr; child = child->next) {
    GtkWidget *match =
        find_widget_by_name(GTK_WIDGET(child->data), name);
    if (match != nullptr) {
      g_list_free(children);
      return match;
    }
  }
  g_list_free(children);
  return nullptr;
}

static void print_color_picker_alpha(GtkWidget *window,
                                     const std::string &id,
                                     const char *name) {
  GtkWidget *widget = find_widget_by_name(window, id);
  std::cout << ' ' << name << '=';
  if (widget == nullptr || !GTK_IS_COLOR_CHOOSER(widget)) {
    std::cout << "missing";
    return;
  }
  std::cout << (gtk_color_chooser_get_use_alpha(
                    GTK_COLOR_CHOOSER(widget)) != FALSE
                    ? "true"
                    : "false");
}

static void print_color_picker_properties(GtkWidget *window,
                                          const std::string &id_prefix) {
  std::cout << "COLOR_PICKERS";
  print_color_picker_alpha(
      window, id_prefix + "_general_exterior_background_button",
      "exterior_use_alpha");
  print_color_picker_alpha(
      window, id_prefix + "_general_background_button",
      "background_use_alpha");
  std::cout << '\n';
  std::cout.flush();
}

static void select_initial_page(GtkWidget *window,
                                const std::string &page) {
  GtkWidget *notebook = find_notebook(window);
  if (notebook == nullptr || !GTK_IS_NOTEBOOK(notebook)) {
    return;
  }

  const std::string suffix = page.empty() ? "general" : page;
  GtkWidget *target =
      find_widget_by_name(window, "settings_" + suffix + "_page");
  if (target == nullptr) {
    target = find_widget_by_name(window,
                                 "global_settings_" + suffix + "_page");
  }
  const gint page_number = target == nullptr
                               ? 0
                               : gtk_notebook_page_num(GTK_NOTEBOOK(notebook),
                                                       target);
  if (page_number >= 0) {
    gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), page_number);
  }
}

static std::string
connection_type_name(elder_terms::ConnectionKind kind) {
  if (kind == elder_terms::ConnectionKind::telnet) {
    return "telnet";
  }
  if (kind == elder_terms::ConnectionKind::ssh) {
    return "ssh";
  }
  if (kind == elder_terms::ConnectionKind::serial) {
    return "serial";
  }
  if (kind == elder_terms::ConnectionKind::sftp) {
    return "sftp";
  }
  return "local";
}

static std::string zmodem_autostart_name(
    const elder_terms::SettingsStore &store) {
  const bool configured = elder_terms::setting_boolean_value_or_default(
      store, elder_terms::transfer_zmodem_autostart_setting_key(), false);
  if (!elder_terms::setting_has_configured_value(
          store, elder_terms::transfer_zmodem_autostart_setting_key()) &&
      !configured) {
    return "default";
  }
  return configured ? "enabled" : "disabled";
}

static const char *
setting_source_name(elder_terms::SettingValueSource source) {
  if (source == elder_terms::SettingValueSource::global) {
    return "global";
  }
  if (source == elder_terms::SettingValueSource::override) {
    return "override";
  }
  return "built-in";
}

static void print_setting_metadata(
    const elder_terms::SettingsStore &store, const char *name,
    const elder_terms::SettingKey &key) {
  std::cout << ' ' << name << "_source="
            << setting_source_name(elder_terms::setting_value_source(store, key))
            << ' ' << name << "_explicit="
            << (elder_terms::setting_has_explicit_value(store, key) ? "true"
                                                                    : "false");
}

static void print_entry_placeholders(GtkWidget *widget) {
  if (widget == nullptr) {
    return;
  }
  if (GTK_IS_ENTRY(widget)) {
    const char *id = gtk_widget_get_name(widget);
    const char *placeholder =
        gtk_entry_get_placeholder_text(GTK_ENTRY(widget));
    if (id != nullptr &&
        (starts_with(id, "settings_") ||
         starts_with(id, "global_settings_"))) {
      std::cout << "PLACEHOLDER " << id << '='
                << (placeholder == nullptr ? "" : placeholder) << '\n';
      gchar *tooltip = gtk_entry_get_icon_tooltip_text(
          GTK_ENTRY(widget), GTK_ENTRY_ICON_SECONDARY);
      if (tooltip != nullptr) {
        std::cout << "ICON_TOOLTIP " << id << '=' << tooltip << '\n';
        g_free(tooltip);
      }
    }
  }
  if (!GTK_IS_CONTAINER(widget)) {
    return;
  }
  GList *children = gtk_container_get_children(GTK_CONTAINER(widget));
  for (GList *child = children; child != nullptr; child = child->next) {
    print_entry_placeholders(GTK_WIDGET(child->data));
  }
  g_list_free(children);
}

static const char *terminal_log_mode_name(
    elder_terms::TerminalLogMode mode) {
  return mode == elder_terms::TerminalLogMode::cooked ? "cooked" : "raw";
}

static void print_store(const char *prefix,
                        const elder_terms::SettingsStore &store) {
  const elder_terms::TerminalDisplaySettings display =
      elder_terms::terminal_display_settings(store);
  const std::string exterior_background =
      elder_terms::setting_string_value_or_default(
          store,
          elder_terms::general_exterior_background_setting_key(),
          "none");
  const std::string background =
      elder_terms::setting_string_value_or_default(
          store, elder_terms::general_background_setting_key(), "none");
  const std::optional<elder_terms::TerminalConnectionProfile> profile =
      elder_terms::terminal_connection_profile(store);
  const elder_terms::TerminalTextSettings text_settings =
      profile.has_value()
          ? profile->text_settings
          : elder_terms::default_terminal_text_settings(
                elder_terms::TerminalConnectionKind::local_shell);
  const elder_terms::TelnetConnectionSettings telnet =
      elder_terms::telnet_connection_settings(store);
  const elder_terms::SshConnectionSettings ssh =
      elder_terms::ssh_connection_settings(store);
  const elder_terms::SftpConnectionSettings sftp =
      elder_terms::sftp_connection_settings(store);
  const elder_terms::SerialConnectionSettings serial =
      elder_terms::serial_connection_settings(store);
  const elder_terms::TerminalLogSettings log =
      elder_terms::terminal_log_settings(store);
  std::string macro_ids;
  for (const elder_terms::MacroRule &rule : store.macro_rules) {
    if (!macro_ids.empty()) {
      macro_ids += ',';
    }
    macro_ids += rule.id;
  }
  std::cout << prefix
            << " dirty="
            << (elder_terms::settings_store_is_dirty(store) ? "true" : "false")
            << " type="
            << connection_type_name(
                   elder_terms::general_connection_kind(store))
            << " name=" << elder_terms::general_connection_name(store)
            << " width=" << display.width << " height=" << display.height
            << " zoom=" << display.zoom
            << " encoding=" << text_settings.encoding
            << " backspace_code="
            << elder_terms::terminal_backspace_code_to_string(
                   text_settings.backspace_code)
            << " cursor_key_mode="
            << elder_terms::terminal_cursor_key_mode_to_string(
                   text_settings.cursor_key_mode)
            << " auto_close="
            << (elder_terms::terminal_auto_close(store) ? "true" : "false")
            << " exterior_background=" << exterior_background
            << " background=" << background
            << " zoom_in_key="
            << elder_terms::terminal_zoom_in_key(store)
            << " zoom_out_key="
            << elder_terms::terminal_zoom_out_key(store)
            << " telnet_address=" << telnet.address
            << " telnet_port=" << telnet.port
            << " telnet_terminal_type=" << telnet.terminal_type
            << " ssh_address=" << ssh.endpoint.address
            << " ssh_port=" << ssh.endpoint.port
            << " ssh_username=" << ssh.endpoint.username
            << " ssh_identity_file=" << ssh.endpoint.identity_file
            << " ssh_terminal_type=" << ssh.terminal_type
            << " sftp_local_directory=" << sftp.local_directory
            << " sftp_remote_directory=" << sftp.remote_directory
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
            << " startup_mode="
            << elder_terms::startup_mode_to_string(
                   elder_terms::application_startup_mode(store))
            << " ui_language="
            << elder_terms::application_ui_language_to_string(
                   elder_terms::application_ui_language(store))
            << " open_application="
            << elder_terms::application_open_hotkey_text(store)
            << " open_connection="
            << elder_terms::general_open_connection_hotkey_text(store)
            << " macro_count=" << store.macro_rules.size()
            << " macro_ids=" << macro_ids;
  print_setting_metadata(
      store, "ui_language",
      elder_terms::application_ui_language_setting_key());
  print_setting_metadata(
      store, "startup_mode",
      elder_terms::application_startup_mode_setting_key());
  print_setting_metadata(
      store, "open_application",
      elder_terms::application_open_hotkey_setting_key());
  print_setting_metadata(
      store, "open_connection",
      elder_terms::general_open_connection_hotkey_setting_key());
  print_setting_metadata(store, "type",
                         elder_terms::general_type_setting_key());
  print_setting_metadata(store, "width",
                         elder_terms::terminal_width_setting_key());
  print_setting_metadata(store, "height",
                         elder_terms::terminal_height_setting_key());
  print_setting_metadata(store, "zoom",
                         elder_terms::terminal_zoom_setting_key());
  print_setting_metadata(store, "auto_close",
                         elder_terms::terminal_auto_close_setting_key());
  print_setting_metadata(
      store, "exterior_background",
      elder_terms::general_exterior_background_setting_key());
  print_setting_metadata(store, "background",
                         elder_terms::general_background_setting_key());
  print_setting_metadata(store, "encoding",
                         elder_terms::terminal_encoding_setting_key());
  print_setting_metadata(store, "backspace_code",
                         elder_terms::terminal_backspace_code_setting_key());
  print_setting_metadata(store, "cursor_key_mode",
                         elder_terms::terminal_cursor_key_mode_setting_key());
  print_setting_metadata(store, "zoom_in_key",
                         elder_terms::terminal_zoom_in_key_setting_key());
  print_setting_metadata(store, "zoom_out_key",
                         elder_terms::terminal_zoom_out_key_setting_key());
  print_setting_metadata(store, "telnet_address",
                         elder_terms::telnet_address_setting_key());
  print_setting_metadata(store, "telnet_port",
                         elder_terms::telnet_port_setting_key());
  print_setting_metadata(store, "telnet_terminal_type",
                         elder_terms::telnet_terminal_type_setting_key());
  print_setting_metadata(store, "ssh_address",
                         elder_terms::ssh_address_setting_key());
  print_setting_metadata(store, "ssh_port",
                         elder_terms::ssh_port_setting_key());
  print_setting_metadata(store, "ssh_username",
                         elder_terms::ssh_username_setting_key());
  print_setting_metadata(store, "ssh_identity_file",
                         elder_terms::ssh_identity_file_setting_key());
  print_setting_metadata(store, "ssh_terminal_type",
                         elder_terms::ssh_terminal_type_setting_key());
  print_setting_metadata(store, "sftp_local_directory",
                         elder_terms::sftp_local_directory_setting_key());
  print_setting_metadata(store, "sftp_remote_directory",
                         elder_terms::sftp_remote_directory_setting_key());
  print_setting_metadata(store, "serial_device",
                         elder_terms::serial_device_setting_key());
  print_setting_metadata(store, "serial_baudrate",
                         elder_terms::serial_baudrate_setting_key());
  print_setting_metadata(store, "serial_bits",
                         elder_terms::serial_bits_setting_key());
  print_setting_metadata(store, "serial_parity",
                         elder_terms::serial_parity_setting_key());
  print_setting_metadata(store, "serial_stop_bit",
                         elder_terms::serial_stop_bit_setting_key());
  print_setting_metadata(store, "serial_flow_control",
                         elder_terms::serial_flow_control_setting_key());
  print_setting_metadata(store, "serial_carrier_detect",
                         elder_terms::serial_carrier_detect_setting_key());
  print_setting_metadata(store, "transfer_base_path",
                         elder_terms::transfer_base_path_setting_key());
  print_setting_metadata(
      store, "text_send_bytes_per_second",
      elder_terms::transfer_text_send_bytes_per_second_setting_key());
  print_setting_metadata(
      store, "zmodem_autostart",
      elder_terms::transfer_zmodem_autostart_setting_key());
  print_setting_metadata(store, "log_enabled",
                         elder_terms::terminal_log_enabled_setting_key());
  print_setting_metadata(store, "log_base_directory",
                         elder_terms::terminal_log_base_directory_setting_key());
  print_setting_metadata(
      store, "log_file_name_format",
      elder_terms::terminal_log_file_name_format_setting_key());
  print_setting_metadata(store, "log_mode",
                         elder_terms::terminal_log_mode_setting_key());
  std::cout << '\n';
  std::cout.flush();
}

static void on_rebase_clicked(GtkButton *, gpointer data) {
  auto *state = static_cast<FixtureState *>(data);
  if (state->settings_widget == nullptr || !state->rebase_store.has_value()) {
    return;
  }
  elder_terms::settings_widget_rebase_fallbacks(
      state->settings_widget, *state->rebase_store);
  print_store("REBASED",
              elder_terms::settings_widget_draft_store(state->settings_widget));
  print_entry_placeholders(state->window);
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
    const elder_terms::LocalizationInitializationResult localization =
        elder_terms::initialize_localization(
            elder_terms::ApplicationUiLanguage::system);
    for (const std::string &warning : localization.warnings) {
      std::cerr << warning << '\n';
    }
    gtk_disable_setlocale();
    gtk_init(&argc, &argv);
    const elder_terms_settings_widget_fixture::FixtureOptions options =
        elder_terms_settings_widget_fixture::parse_options(argc, argv);
    elder_terms_settings_widget_fixture::FixtureState state;

    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    state.window = window;
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
      elder_terms_settings_widget_fixture::print_entry_placeholders(
          state.window);
      std::cout.flush();
    };

    elder_terms::SettingsStore store =
        options.global_mode
            ? elder_terms_settings_widget_fixture::create_global_store(
                  options.global_assignments)
            : elder_terms_settings_widget_fixture::create_store(options);
    if (!options.rebase_global_assignments.empty()) {
      state.rebase_store =
          elder_terms_settings_widget_fixture::create_global_store(
              options.rebase_global_assignments);
    }

    elder_terms::SettingsWidgetOptions widget_options{
        .store = std::move(store),
        .is_runtime = options.is_runtime,
        .show_actions = options.show_actions,
        .mode = options.global_mode
                    ? elder_terms::SettingsWidgetMode::global_defaults
                    : elder_terms::SettingsWidgetMode::connection,
        .id_prefix = options.global_mode ? "global_settings" : "settings",
        .callbacks = std::move(callbacks),
    };
    state.settings_widget =
        elder_terms::create_settings_widget(std::move(widget_options));
    GtkWidget *contents = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(window), contents);
    gtk_box_pack_start(
        GTK_BOX(contents),
        elder_terms::settings_widget_root(state.settings_widget), TRUE, TRUE,
        0);
    if (state.rebase_store.has_value()) {
      GtkWidget *rebase_button =
          gtk_button_new_with_label("Rebase test fallbacks");
      elder_terms_settings_widget_fixture::assign_accessible_id(
          rebase_button, "rebase_fallbacks_button");
      g_signal_connect(
          rebase_button, "clicked",
          G_CALLBACK(elder_terms_settings_widget_fixture::on_rebase_clicked),
          &state);
      gtk_box_pack_end(GTK_BOX(contents), rebase_button, FALSE, FALSE, 0);
    }
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
    elder_terms_settings_widget_fixture::print_entry_placeholders(window);
    elder_terms_settings_widget_fixture::print_color_picker_properties(
        window, options.global_mode ? "global_settings" : "settings");
    std::cout << "READY\n";
    std::cout.flush();
    gtk_main();
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
