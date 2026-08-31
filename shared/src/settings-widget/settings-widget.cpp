#include <elder-terms/settings-widget.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <atk/atk.h>

#include <elder-terms/key-binding-input-widget.h>
#include <elder-terms/serial-device-event-monitor.h>
#include <elder-terms/settings/general-settings.h>

#include "settings-presentation.h"

namespace elder_terms {

static constexpr char local_connection_type[] = "local";
static constexpr char telnet_connection_type[] = "telnet";
static constexpr char serial_connection_type[] = "serial";
static constexpr char serial_device_path_mode[] = "path";
static constexpr char serial_device_stable_id_mode[] = "by-id";
static constexpr char serial_device_physical_port_mode[] = "by-path";
static constexpr char serial_device_no_device_choice[] =
    "__elder_terms_no_device";
static constexpr char ssh_connection_type[] = "ssh";
static constexpr char sftp_connection_type[] = "sftp";
static constexpr char ftp_connection_type[] = "ftp";
static constexpr char zmodem_autostart_enabled[] = "enabled";
static constexpr char zmodem_autostart_disabled[] = "disabled";
static constexpr char terminal_text_default[] = "default";
static constexpr char terminal_backspace_auto[] = "auto";
static constexpr char terminal_backspace_bs[] = "bs";
static constexpr char terminal_backspace_del[] = "del";
static constexpr char terminal_cursor_normal[] = "normal";
static constexpr char terminal_cursor_trs80[] = "trs80";
static constexpr char terminal_return_auto[] = "auto";
static constexpr char terminal_return_cr[] = "cr";
static constexpr char terminal_return_lf[] = "lf";
static constexpr char terminal_return_crlf[] = "crlf";
static constexpr char terminal_log_raw[] = "raw";
static constexpr char terminal_log_cooked[] = "cooked";
static constexpr char terminal_font_default[] = "default";
static constexpr char terminal_font_custom[] = "custom";
static constexpr char general_color_none[] = "none";
static constexpr char general_color_custom[] = "custom";
static constexpr char inherit_choice[] = "inherit";
static constexpr char boolean_enabled[] = "enabled";
static constexpr char boolean_disabled[] = "disabled";
static constexpr char default_font_button_family[] = "Monospace";
static constexpr gint64 minimum_terminal_scrollback_lines = 1000;
static constexpr gint64 maximum_terminal_scrollback_lines = 100000;
static constexpr gint64 minimum_terminal_border_width = 1;
static constexpr gint64 maximum_terminal_border_width = 1000;

struct ConnectionSettingsPage {
  std::vector<const char *> connection_types;
  GtkWidget *page = nullptr;
  GtkWidget *tab_label = nullptr;
};

struct SettingsWidgetState {
  SettingsStore applied_store;
  SettingsStore draft_store;
  SettingsWidgetMode mode = SettingsWidgetMode::connection;
  std::string id_prefix = "settings";
  bool is_runtime = false;
  bool show_actions = true;
  bool synchronizing = false;
  SettingsWidgetCallbacks callbacks;
  GtkWidget *root = nullptr;
  GtkWidget *notebook = nullptr;
  GtkWidget *general_name_entry = nullptr;
  GtkWidget *general_type_combo = nullptr;
  KeyBindingInputWidgetState *general_open_connection_input = nullptr;
  GtkWidget *general_open_connection_reset_button = nullptr;
  GtkWidget *general_exterior_background_mode_combo = nullptr;
  GtkWidget *general_exterior_background_button = nullptr;
  GtkWidget *general_background_mode_combo = nullptr;
  GtkWidget *general_background_button = nullptr;
  GtkWidget *local_command_line_entry = nullptr;
  bool local_command_line_valid = true;
  GtkWidget *terminal_width_entry = nullptr;
  GtkWidget *terminal_height_entry = nullptr;
  GtkWidget *terminal_scrollback_lines_entry = nullptr;
  GtkWidget *terminal_zoom_entry = nullptr;
  GtkWidget *terminal_font_primary_mode_combo = nullptr;
  GtkWidget *terminal_font_primary_button = nullptr;
  GtkWidget *terminal_font_fallback_mode_combo = nullptr;
  GtkWidget *terminal_font_fallback_button = nullptr;
  GtkWidget *terminal_auto_close_combo = nullptr;
  GtkWidget *terminal_show_border_combo = nullptr;
  GtkWidget *terminal_border_width_entry = nullptr;
  GtkWidget *terminal_bell_sound_entry = nullptr;
  GtkWidget *terminal_bell_sound_dialog = nullptr;
  GtkWidget *terminal_encoding_combo = nullptr;
  GtkWidget *terminal_encoding_entry = nullptr;
  GtkWidget *terminal_backspace_code_combo = nullptr;
  GtkWidget *terminal_cursor_key_mode_combo = nullptr;
  GtkWidget *terminal_return_code_combo = nullptr;
  bool terminal_width_valid = true;
  bool terminal_height_valid = true;
  bool terminal_scrollback_lines_valid = true;
  bool terminal_border_width_valid = true;
  bool terminal_zoom_valid = true;
  bool terminal_encoding_valid = true;
  bool terminal_bell_sound_valid = true;
  KeyBindingInputWidgetState *terminal_zoom_in_key_input = nullptr;
  KeyBindingInputWidgetState *terminal_zoom_out_key_input = nullptr;
  KeyBindingInputWidgetState *terminal_send_break_key_input = nullptr;
  GtkWidget *terminal_zoom_in_key_reset_button = nullptr;
  GtkWidget *terminal_zoom_out_key_reset_button = nullptr;
  GtkWidget *terminal_send_break_key_reset_button = nullptr;
  GtkWidget *macro_list = nullptr;
  GtkWidget *macro_editor = nullptr;
  GtkWidget *macro_id_entry = nullptr;
  GtkWidget *macro_regex_entry = nullptr;
  GtkWidget *macro_action_combo = nullptr;
  GtkWidget *macro_send_panel = nullptr;
  GtkWidget *macro_send_view = nullptr;
  GtkWidget *macro_command_panel = nullptr;
  GtkWidget *macro_command_entry = nullptr;
  GtkWidget *macro_arguments_box = nullptr;
  GtkWidget *macro_argument_add_button = nullptr;
  GtkWidget *macro_remove_button = nullptr;
  GtkWidget *macro_move_up_button = nullptr;
  GtkWidget *macro_move_down_button = nullptr;
  int selected_macro = -1;
  unsigned int next_macro_number = 1;
  GtkWidget *telnet_address_entry = nullptr;
  GtkWidget *telnet_port_entry = nullptr;
  GtkWidget *telnet_terminal_type_entry = nullptr;
  bool telnet_port_valid = true;
  GtkWidget *ssh_address_entry = nullptr;
  GtkWidget *ssh_port_entry = nullptr;
  GtkWidget *ssh_username_entry = nullptr;
  GtkWidget *ssh_identity_file_entry = nullptr;
  GtkWidget *ssh_terminal_type_label = nullptr;
  GtkWidget *ssh_terminal_type_entry = nullptr;
  bool ssh_port_valid = true;
  GtkWidget *sftp_local_directory_entry = nullptr;
  GtkWidget *sftp_remote_directory_entry = nullptr;
  GtkWidget *ftp_address_entry = nullptr;
  GtkWidget *ftp_port_entry = nullptr;
  GtkWidget *ftp_username_entry = nullptr;
  GtkWidget *ftp_data_connection_mode_combo = nullptr;
  GtkWidget *ftp_local_directory_entry = nullptr;
  GtkWidget *ftp_remote_directory_entry = nullptr;
  bool ftp_port_valid = true;
  GtkWidget *serial_device_match_mode_combo = nullptr;
  GtkWidget *serial_device_combo = nullptr;
  GtkWidget *serial_stable_id_value = nullptr;
  GtkWidget *serial_usb_serial_value = nullptr;
  GtkWidget *serial_current_node_value = nullptr;
  std::vector<SerialDeviceChoice> serial_device_choices;
  std::unique_ptr<SerialDeviceEventMonitor> serial_device_event_monitor;
  bool serial_device_refresh_pending = false;
  GtkWidget *serial_baudrate_entry = nullptr;
  GtkWidget *serial_bits_combo = nullptr;
  GtkWidget *serial_parity_combo = nullptr;
  GtkWidget *serial_stop_bit_combo = nullptr;
  GtkWidget *serial_flow_control_combo = nullptr;
  GtkWidget *serial_carrier_detect_combo = nullptr;
  bool serial_baudrate_valid = true;
  GtkWidget *transfer_base_path_entry = nullptr;
  GtkWidget *transfer_text_send_rate_entry = nullptr;
  GtkWidget *transfer_text_send_follow_return_code_combo = nullptr;
  GtkWidget *transfer_zmodem_autostart_combo = nullptr;
  bool transfer_text_send_rate_valid = true;
  GtkWidget *log_enabled_combo = nullptr;
  GtkWidget *log_base_directory_entry = nullptr;
  GtkWidget *log_file_name_format_entry = nullptr;
  GtkWidget *log_mode_combo = nullptr;
  bool log_file_name_format_valid = true;
  GtkWidget *apply_button = nullptr;
  GtkWidget *save_button = nullptr;
  GtkWidget *cancel_button = nullptr;
  std::vector<ConnectionSettingsPage> connection_pages;
};

static void sync_serial_device_widgets(SettingsWidgetState *state);
static void refresh_serial_device_widgets(SettingsWidgetState *state);
static std::optional<SerialDeviceChoice> serial_device_choice_for_target(
    const std::vector<SerialDeviceChoice> &choices,
    const std::string &target);
static void sync_serial_device_metadata(
    SettingsWidgetState *state,
    const SerialConnectionSettings &serial);

static std::string widget_id(const SettingsWidgetState *state,
                             const char *suffix) {
  return state->id_prefix + "_" + suffix;
}

static std::string trim_ascii_whitespace(const std::string &value);

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

static GtkWidget *create_page_grid(const char *id) {
  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
  gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
  gtk_widget_set_margin_top(grid, 12);
  gtk_widget_set_margin_bottom(grid, 12);
  gtk_widget_set_margin_start(grid, 12);
  gtk_widget_set_margin_end(grid, 12);
  assign_accessible_id(grid, id);
  return grid;
}

static GtkWidget *create_row_label(const char *text) {
  GtkWidget *label = gtk_label_new(text);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0F);
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  return label;
}

static GtkWidget *attach_row(GtkWidget *grid, int row, const SettingKey &key,
                             GtkWidget *control) {
  const std::string label_text = setting_label(key);
  GtkWidget *label = create_row_label(label_text.c_str());
  gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);
  gtk_widget_set_hexpand(control, true);
  gtk_widget_set_halign(control, GTK_ALIGN_FILL);
  gtk_grid_attach(GTK_GRID(grid), control, 1, row, 1, 1);
  return label;
}

static GtkWidget *attach_text_row(GtkWidget *grid, int row,
                                  const char *text, GtkWidget *control) {
  GtkWidget *label = create_row_label(text);
  gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);
  gtk_widget_set_hexpand(control, true);
  gtk_widget_set_halign(control, GTK_ALIGN_FILL);
  gtk_grid_attach(GTK_GRID(grid), control, 1, row, 1, 1);
  return label;
}

static GtkWidget *create_entry(const std::string &id) {
  GtkWidget *entry = gtk_entry_new();
  assign_accessible_id(entry, id.c_str());
  return entry;
}

static void set_font_button_family(GtkWidget *button,
                                   const std::string &family) {
  if (button == nullptr || family.empty()) {
    return;
  }
  PangoFontDescription *font = pango_font_description_new();
  pango_font_description_set_family(font, family.c_str());
  gtk_font_chooser_set_font_desc(GTK_FONT_CHOOSER(button), font);
  pango_font_description_free(font);
}

static std::optional<std::string> font_button_family(GtkWidget *button) {
  if (button == nullptr) {
    return std::nullopt;
  }
  PangoFontDescription *font =
      gtk_font_chooser_get_font_desc(GTK_FONT_CHOOSER(button));
  if (font == nullptr) {
    return std::nullopt;
  }
  const char *family = pango_font_description_get_family(font);
  const std::string normalized =
      trim_ascii_whitespace(family == nullptr ? "" : family);
  pango_font_description_free(font);
  return normalized.empty() ? std::nullopt
                            : std::optional<std::string>{normalized};
}

static GtkWidget *create_font_family_button(const std::string &id,
                                            const char *title) {
  GtkWidget *button = gtk_font_button_new();
  assign_accessible_id(button, id.c_str());
  gtk_font_button_set_title(GTK_FONT_BUTTON(button), title);
  gtk_font_button_set_use_font(GTK_FONT_BUTTON(button), TRUE);
  gtk_font_button_set_use_size(GTK_FONT_BUTTON(button), FALSE);
  gtk_font_button_set_show_size(GTK_FONT_BUTTON(button), FALSE);
  gtk_font_button_set_show_style(GTK_FONT_BUTTON(button), FALSE);
  gtk_font_chooser_set_level(GTK_FONT_CHOOSER(button),
                             GTK_FONT_CHOOSER_LEVEL_FAMILY);
  gtk_widget_set_hexpand(button, TRUE);
  set_font_button_family(button, default_font_button_family);
  return button;
}

static GtkWidget *create_combo_box(const char *id) {
  GtkWidget *combo = gtk_combo_box_text_new();
  assign_accessible_id(combo, id);
  return combo;
}

static GtkWidget *create_metadata_value_label(const std::string &id) {
  GtkWidget *label = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(label), 0.0F);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
  gtk_label_set_selectable(GTK_LABEL(label), TRUE);
  assign_accessible_id(label, id.c_str());
  return label;
}

static GtkWidget *create_editable_combo_box(const char *combo_id,
                                            const char *entry_id,
                                            GtkWidget **entry) {
  GtkWidget *combo = gtk_combo_box_text_new_with_entry();
  assign_accessible_id(combo, combo_id);
  GtkWidget *child = gtk_bin_get_child(GTK_BIN(combo));
  if (child != nullptr) {
    assign_accessible_id(child, entry_id);
  }
  *entry = child;
  return combo;
}

static void append_combo_option(GtkWidget *combo, const char *id,
                                const char *label) {
  gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), id, label);
}

static std::string active_combo_id(GtkWidget *combo,
                                   const char *fallback) {
  const gchar *active_id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(combo));
  return active_id == nullptr ? fallback : active_id;
}

static std::string format_double(gdouble value) {
  std::ostringstream stream;
  stream << std::setprecision(15) << value;
  return stream.str();
}

static std::string format_setting_value(const SettingValue &value) {
  if (const auto *integer = std::get_if<gint64>(&value)) {
    return std::to_string(*integer);
  }
  if (const auto *floating = std::get_if<gdouble>(&value)) {
    return format_double(*floating);
  }
  if (const auto *text = std::get_if<std::string>(&value)) {
    return *text;
  }
  return std::get<bool>(value)
             ? settings_ui_text(SettingsUiText::enabled)
             : settings_ui_text(SettingsUiText::disabled);
}

static std::string setting_fallback_label(const SettingsStore &store,
                                          const SettingKey &key,
                                          const std::string &display_value) {
  return inherited_setting_label(display_value,
                                 setting_fallback_source(store, key));
}

static void sync_inheritable_entry(GtkWidget *entry,
                                   const SettingsStore &store,
                                   const SettingKey &key,
                                   const std::string &effective_value) {
  if (entry == nullptr) {
    return;
  }
  if (setting_has_explicit_value(store, key)) {
    gtk_entry_set_text(GTK_ENTRY(entry), effective_value.c_str());
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), nullptr);
    return;
  }
  gtk_entry_set_text(GTK_ENTRY(entry), "");
  const std::string placeholder =
      setting_fallback_label(store, key, effective_value);
  gtk_entry_set_placeholder_text(GTK_ENTRY(entry), placeholder.c_str());
}

static void set_entry_validation(GtkWidget *entry, bool valid,
                                 const std::string &reason) {
  if (entry == nullptr) {
    return;
  }
  GtkStyleContext *context = gtk_widget_get_style_context(entry);
  if (valid) {
    gtk_style_context_remove_class(context, GTK_STYLE_CLASS_ERROR);
  } else {
    gtk_style_context_add_class(context, GTK_STYLE_CLASS_ERROR);
  }
  gtk_entry_set_icon_from_icon_name(
      GTK_ENTRY(entry), GTK_ENTRY_ICON_SECONDARY,
      valid ? nullptr : "dialog-error-symbolic");
  const std::string message = settings_validation_message(reason);
  gtk_entry_set_icon_tooltip_text(GTK_ENTRY(entry),
                                  GTK_ENTRY_ICON_SECONDARY,
                                  valid ? nullptr : message.c_str());
}

static void update_string_entry(SettingsWidgetState *state, GtkWidget *entry,
                                const SettingKey &key) {
  const char *text = gtk_entry_get_text(GTK_ENTRY(entry));
  const std::string value = text == nullptr ? "" : text;
  if (value.empty()) {
    clear_explicit_setting_value(&state->draft_store, key);
    const std::string effective = format_setting_value(
        setting_value_or_default(state->draft_store, key,
                                 SettingValue{std::string()}));
    const bool previous_synchronizing = state->synchronizing;
    state->synchronizing = true;
    sync_inheritable_entry(entry, state->draft_store, key, effective);
    state->synchronizing = previous_synchronizing;
    return;
  }
  gtk_entry_set_placeholder_text(GTK_ENTRY(entry), nullptr);
  set_explicit_setting_value(&state->draft_store, key, SettingValue{value});
}

static bool parse_integer_entry(GtkWidget *entry, gint64 minimum,
                                gint64 maximum, gint64 *value,
                                std::string *reason) {
  const char *text = gtk_entry_get_text(GTK_ENTRY(entry));
  const std::string trimmed =
      trim_ascii_whitespace(text == nullptr ? "" : text);
  if (trimmed.empty()) {
    return false;
  }
  errno = 0;
  char *end = nullptr;
  const gint64 parsed = g_ascii_strtoll(trimmed.c_str(), &end, 10);
  if (errno == ERANGE || end == trimmed.c_str() || end == nullptr ||
      *end != '\0') {
    *reason = "Enter a whole number";
    return false;
  }
  if (parsed < minimum || parsed > maximum) {
    *reason = "Value must be between " + std::to_string(minimum) + " and " +
              std::to_string(maximum);
    return false;
  }
  *value = parsed;
  return true;
}

static bool parse_double_entry(GtkWidget *entry, gdouble minimum,
                               gdouble maximum, gdouble *value,
                               std::string *reason) {
  const char *text = gtk_entry_get_text(GTK_ENTRY(entry));
  const std::string trimmed =
      trim_ascii_whitespace(text == nullptr ? "" : text);
  if (trimmed.empty()) {
    return false;
  }
  errno = 0;
  char *end = nullptr;
  const gdouble parsed = g_ascii_strtod(trimmed.c_str(), &end);
  if (errno == ERANGE || end == trimmed.c_str() || end == nullptr ||
      *end != '\0' || !std::isfinite(parsed)) {
    *reason = "Enter a number";
    return false;
  }
  if (parsed < minimum || parsed > maximum) {
    *reason = "Value must be between " + format_double(minimum) + " and " +
              format_double(maximum);
    return false;
  }
  *value = parsed;
  return true;
}

static void sync_cleared_entry(SettingsWidgetState *state, GtkWidget *entry,
                               const SettingKey &key,
                               const std::string &effective_value) {
  const bool previous_synchronizing = state->synchronizing;
  state->synchronizing = true;
  sync_inheritable_entry(entry, state->draft_store, key, effective_value);
  state->synchronizing = previous_synchronizing;
}

static std::string connection_type_value(const SettingsStore &store) {
  return setting_string_value_or_default(store, general_type_setting_key(),
                                         local_connection_type);
}

static TerminalConnectionKind
connection_kind_value(const SettingsStore &store) {
  const std::string type = connection_type_value(store);
  if (type == telnet_connection_type) {
    return TerminalConnectionKind::telnet;
  }
  if (type == ssh_connection_type) {
    return TerminalConnectionKind::ssh;
  }
  if (type == sftp_connection_type) {
    return TerminalConnectionKind::ssh;
  }
  if (type == serial_connection_type) {
    return TerminalConnectionKind::serial;
  }
  return TerminalConnectionKind::local_shell;
}

static std::string trim_ascii_whitespace(const std::string &value) {
  const auto first = std::find_if_not(
      value.begin(), value.end(),
      [](unsigned char character) { return std::isspace(character) != 0; });
  const auto last = std::find_if_not(
                        value.rbegin(), value.rend(),
                        [](unsigned char character) {
                          return std::isspace(character) != 0;
                        })
                        .base();
  if (first >= last) {
    return {};
  }
  return std::string(first, last);
}

static std::string lower_ascii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return value;
}

static std::string terminal_encoding_fallback(
    const SettingsWidgetState *state,
    const TerminalTextSettings &built_in_defaults) {
  if (setting_fallback_source(state->draft_store,
                              terminal_encoding_setting_key()) ==
      SettingValueSource::global) {
    return std::get<std::string>(setting_fallback_value(
        state->draft_store, terminal_encoding_setting_key(),
        SettingValue{built_in_defaults.encoding}));
  }
  return built_in_defaults.encoding;
}

static std::string terminal_backspace_fallback(
    const SettingsWidgetState *state,
    const TerminalTextSettings &built_in_defaults) {
  if (setting_fallback_source(state->draft_store,
                              terminal_backspace_code_setting_key()) ==
      SettingValueSource::global) {
    return std::get<std::string>(setting_fallback_value(
        state->draft_store, terminal_backspace_code_setting_key(),
        SettingValue{std::string(terminal_backspace_code_to_string(
            built_in_defaults.backspace_code))}));
  }
  return terminal_backspace_code_to_string(built_in_defaults.backspace_code);
}

static std::string terminal_cursor_fallback(
    const SettingsWidgetState *state,
    const TerminalTextSettings &built_in_defaults) {
  if (setting_fallback_source(state->draft_store,
                              terminal_cursor_key_mode_setting_key()) ==
      SettingValueSource::global) {
    return std::get<std::string>(setting_fallback_value(
        state->draft_store, terminal_cursor_key_mode_setting_key(),
        SettingValue{std::string(terminal_cursor_key_mode_to_string(
            built_in_defaults.cursor_key_mode))}));
  }
  return terminal_cursor_key_mode_to_string(
      built_in_defaults.cursor_key_mode);
}

static std::string terminal_return_fallback(
    const SettingsWidgetState *state,
    const TerminalTextSettings &built_in_defaults) {
  if (setting_fallback_source(state->draft_store,
                              terminal_return_code_setting_key()) ==
      SettingValueSource::global) {
    return std::get<std::string>(setting_fallback_value(
        state->draft_store, terminal_return_code_setting_key(),
        SettingValue{std::string(terminal_return_code_to_string(
            built_in_defaults.return_code))}));
  }
  return terminal_return_code_to_string(built_in_defaults.return_code);
}

static void populate_terminal_encoding_combo(SettingsWidgetState *state,
                                              const TerminalTextSettings
                                                  &built_in_defaults) {
  if (state->terminal_encoding_combo == nullptr) {
    return;
  }

  gtk_combo_box_text_remove_all(
      GTK_COMBO_BOX_TEXT(state->terminal_encoding_combo));
  const std::string fallback =
      terminal_encoding_fallback(state, built_in_defaults);
  const std::string default_label = setting_fallback_label(
      state->draft_store, terminal_encoding_setting_key(), fallback);
  append_combo_option(state->terminal_encoding_combo, terminal_text_default,
                      default_label.c_str());

  const std::vector<std::string> choices = terminal_encoding_choices();
  for (const std::string &choice : choices) {
    append_combo_option(state->terminal_encoding_combo, choice.c_str(),
                        choice.c_str());
  }

  if (!setting_has_explicit_value(state->draft_store,
                                  terminal_encoding_setting_key())) {
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(state->terminal_encoding_combo),
                                terminal_text_default);
    gtk_entry_set_text(GTK_ENTRY(state->terminal_encoding_entry), "");
    gtk_entry_set_placeholder_text(GTK_ENTRY(state->terminal_encoding_entry),
                                   default_label.c_str());
    return;
  }

  const std::string configured = trim_ascii_whitespace(
      setting_string_value_or_default(state->draft_store,
                                      terminal_encoding_setting_key(),
                                      fallback));
  gtk_entry_set_placeholder_text(GTK_ENTRY(state->terminal_encoding_entry),
                                 nullptr);
  const std::string configured_key = lower_ascii(configured);
  const auto matching =
      std::find_if(choices.begin(), choices.end(),
                   [&configured_key](const std::string &choice) {
                     return lower_ascii(choice) == configured_key;
                   });
  if (matching != choices.end()) {
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(state->terminal_encoding_combo),
                                matching->c_str());
    return;
  }

  append_combo_option(state->terminal_encoding_combo, configured.c_str(),
                      configured.c_str());
  gtk_combo_box_set_active_id(GTK_COMBO_BOX(state->terminal_encoding_combo),
                              configured.c_str());
}

static void populate_terminal_special_code_combos(
    SettingsWidgetState *state,
    const TerminalTextSettings &built_in_defaults) {
  if (state->terminal_backspace_code_combo != nullptr) {
    gtk_combo_box_text_remove_all(
        GTK_COMBO_BOX_TEXT(state->terminal_backspace_code_combo));
    const std::string fallback =
        terminal_backspace_fallback(state, built_in_defaults);
    const std::string default_label = setting_fallback_label(
        state->draft_store, terminal_backspace_code_setting_key(),
        setting_choice_label(terminal_backspace_code_setting_key(),
                             fallback));
    append_combo_option(state->terminal_backspace_code_combo,
                        terminal_text_default, default_label.c_str());
    append_combo_option(state->terminal_backspace_code_combo,
                        terminal_backspace_auto,
                        setting_choice_label(
                            terminal_backspace_code_setting_key(),
                            terminal_backspace_auto)
                            .c_str());
    append_combo_option(state->terminal_backspace_code_combo,
                        terminal_backspace_bs,
                        setting_choice_label(
                            terminal_backspace_code_setting_key(),
                            terminal_backspace_bs)
                            .c_str());
    append_combo_option(state->terminal_backspace_code_combo,
                        terminal_backspace_del,
                        setting_choice_label(
                            terminal_backspace_code_setting_key(),
                            terminal_backspace_del)
                            .c_str());
    const char *active = terminal_text_default;
    if (setting_has_explicit_value(
            state->draft_store, terminal_backspace_code_setting_key())) {
      const std::string configured = setting_string_value_or_default(
          state->draft_store, terminal_backspace_code_setting_key(), fallback);
      if (configured == terminal_backspace_auto) {
        active = terminal_backspace_auto;
      } else if (configured == terminal_backspace_bs) {
        active = terminal_backspace_bs;
      } else {
        active = terminal_backspace_del;
      }
    }
    gtk_combo_box_set_active_id(
        GTK_COMBO_BOX(state->terminal_backspace_code_combo), active);
  }

  if (state->terminal_cursor_key_mode_combo != nullptr) {
    gtk_combo_box_text_remove_all(
        GTK_COMBO_BOX_TEXT(state->terminal_cursor_key_mode_combo));
    const std::string fallback =
        terminal_cursor_fallback(state, built_in_defaults);
    const std::string default_label = setting_fallback_label(
        state->draft_store, terminal_cursor_key_mode_setting_key(),
        setting_choice_label(terminal_cursor_key_mode_setting_key(),
                             fallback));
    append_combo_option(state->terminal_cursor_key_mode_combo,
                        terminal_text_default, default_label.c_str());
    append_combo_option(state->terminal_cursor_key_mode_combo,
                        terminal_cursor_normal,
                        setting_choice_label(
                            terminal_cursor_key_mode_setting_key(),
                            terminal_cursor_normal)
                            .c_str());
    append_combo_option(state->terminal_cursor_key_mode_combo,
                        terminal_cursor_trs80,
                        setting_choice_label(
                            terminal_cursor_key_mode_setting_key(),
                            terminal_cursor_trs80)
                            .c_str());
    const char *active = terminal_text_default;
    if (setting_has_explicit_value(
            state->draft_store, terminal_cursor_key_mode_setting_key())) {
      active = setting_string_value_or_default(
                   state->draft_store, terminal_cursor_key_mode_setting_key(),
                   fallback) == terminal_cursor_trs80
                   ? terminal_cursor_trs80
                   : terminal_cursor_normal;
    }
    gtk_combo_box_set_active_id(
        GTK_COMBO_BOX(state->terminal_cursor_key_mode_combo), active);
  }

  if (state->terminal_return_code_combo != nullptr) {
    gtk_combo_box_text_remove_all(
        GTK_COMBO_BOX_TEXT(state->terminal_return_code_combo));
    const std::string fallback =
        terminal_return_fallback(state, built_in_defaults);
    const std::string default_label = setting_fallback_label(
        state->draft_store, terminal_return_code_setting_key(),
        setting_choice_label(terminal_return_code_setting_key(), fallback));
    append_combo_option(state->terminal_return_code_combo,
                        terminal_text_default, default_label.c_str());
    for (const char *choice : {terminal_return_auto, terminal_return_cr,
                               terminal_return_lf, terminal_return_crlf}) {
      append_combo_option(
          state->terminal_return_code_combo, choice,
          setting_choice_label(terminal_return_code_setting_key(), choice)
              .c_str());
    }
    const char *active = terminal_text_default;
    if (setting_has_explicit_value(state->draft_store,
                                   terminal_return_code_setting_key())) {
      const std::string configured = setting_string_value_or_default(
          state->draft_store, terminal_return_code_setting_key(), fallback);
      if (configured == terminal_return_auto) {
        active = terminal_return_auto;
      } else if (configured == terminal_return_cr) {
        active = terminal_return_cr;
      } else if (configured == terminal_return_lf) {
        active = terminal_return_lf;
      } else {
        active = terminal_return_crlf;
      }
    }
    gtk_combo_box_set_active_id(
        GTK_COMBO_BOX(state->terminal_return_code_combo), active);
  }
}

static void sync_terminal_text_widgets(SettingsWidgetState *state) {
  const bool previous_synchronizing = state->synchronizing;
  state->synchronizing = true;
  const TerminalTextSettings defaults =
      default_terminal_text_settings(connection_kind_value(state->draft_store));
  populate_terminal_encoding_combo(state, defaults);
  populate_terminal_special_code_combos(state, defaults);
  state->terminal_encoding_valid = true;
  if (state->terminal_encoding_entry != nullptr) {
    gtk_entry_set_icon_from_icon_name(GTK_ENTRY(state->terminal_encoding_entry),
                                      GTK_ENTRY_ICON_SECONDARY, nullptr);
    gtk_entry_set_icon_tooltip_text(GTK_ENTRY(state->terminal_encoding_entry),
                                    GTK_ENTRY_ICON_SECONDARY, nullptr);
  }
  state->synchronizing = previous_synchronizing;
}

static void update_connection_pages(SettingsWidgetState *state) {
  const std::string active_type = connection_type_value(state->draft_store);
  for (const ConnectionSettingsPage &page : state->connection_pages) {
    const bool visible =
        state->mode == SettingsWidgetMode::global_defaults ||
        std::any_of(page.connection_types.begin(),
                    page.connection_types.end(),
                    [&active_type](const char *connection_type) {
                      return active_type == connection_type;
                    });
    gtk_widget_set_visible(page.page, visible);
    gtk_widget_set_visible(page.tab_label, visible);
  }
  const bool terminal_type_visible =
      state->mode == SettingsWidgetMode::global_defaults ||
      active_type == ssh_connection_type;
  if (state->ssh_terminal_type_label != nullptr) {
    gtk_widget_set_visible(state->ssh_terminal_type_label,
                           terminal_type_visible);
  }
  if (state->ssh_terminal_type_entry != nullptr) {
    gtk_widget_set_visible(state->ssh_terminal_type_entry,
                           terminal_type_visible);
  }

  const gint current_page = gtk_notebook_get_current_page(GTK_NOTEBOOK(
      state->notebook));
  GtkWidget *current_child =
      gtk_notebook_get_nth_page(GTK_NOTEBOOK(state->notebook), current_page);
  if (current_child != nullptr && !gtk_widget_get_visible(current_child)) {
    gtk_notebook_set_current_page(GTK_NOTEBOOK(state->notebook), 0);
  }
}

static bool ascii_blank(const std::string &value) {
  return std::all_of(value.begin(), value.end(), [](unsigned char character) {
    return g_ascii_isspace(character) != FALSE;
  });
}

static void update_general_name_from_widget(SettingsWidgetState *state) {
  const char *text =
      gtk_entry_get_text(GTK_ENTRY(state->general_name_entry));
  const std::string name = text == nullptr ? "" : text;
  if (ascii_blank(name)) {
    clear_explicit_setting_value(&state->draft_store,
                                 general_name_setting_key());
    return;
  }

  if (!setting_has_explicit_value(state->draft_store,
                                  general_name_setting_key()) &&
      name == general_connection_name(state->draft_store)) {
    return;
  }
  set_explicit_setting_value(&state->draft_store,
                             general_name_setting_key(), SettingValue{name});
}

static void on_tab_button_clicked(GtkButton *button, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  auto *page = static_cast<GtkWidget *>(
      g_object_get_data(G_OBJECT(button), "elder-terms-settings-page"));
  if (page == nullptr) {
    return;
  }

  const gint page_number =
      gtk_notebook_page_num(GTK_NOTEBOOK(state->notebook), page);
  if (page_number >= 0) {
    gtk_notebook_set_current_page(GTK_NOTEBOOK(state->notebook), page_number);
  }
}

static GtkWidget *create_tab_button(SettingsWidgetState *state,
                                    GtkWidget *page, const char *text,
                                    const char *id) {
  GtkWidget *button = gtk_button_new_with_label(text);
  gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NONE);
  gtk_widget_set_focus_on_click(button, FALSE);
  assign_accessible_id(button, id);
  g_object_set_data(G_OBJECT(button), "elder-terms-settings-page", page);
  g_signal_connect(button, "clicked", G_CALLBACK(on_tab_button_clicked), state);
  return button;
}

static void update_general_type_from_widget(SettingsWidgetState *state) {
  const std::string type =
      active_combo_id(state->general_type_combo, inherit_choice);
  if (type == inherit_choice) {
    clear_explicit_setting_value(&state->draft_store,
                                 general_type_setting_key());
  } else {
    set_explicit_setting_value(&state->draft_store,
                               general_type_setting_key(),
                               SettingValue{type});
  }
  update_connection_pages(state);
  sync_terminal_text_widgets(state);
}

static void
update_connection_hotkey_from_widget(SettingsWidgetState *state) {
  const std::string text =
      key_binding_input_widget_text(
          state->general_open_connection_input);
  std::string reason;
  const bool valid = global_hotkey_text_is_valid(text, &reason);
  set_key_binding_input_widget_external_error(
      state->general_open_connection_input,
      valid ? std::string() : reason);
  if (!valid) {
    return;
  }
  set_explicit_setting_value(
      &state->draft_store,
      general_open_connection_hotkey_setting_key(),
      SettingValue{text});
}

static void update_terminal_width_from_widget(SettingsWidgetState *state) {
  const char *text =
      gtk_entry_get_text(GTK_ENTRY(state->terminal_width_entry));
  if (trim_ascii_whitespace(text == nullptr ? "" : text).empty()) {
    clear_explicit_setting_value(&state->draft_store,
                                 terminal_width_setting_key());
    state->terminal_width_valid = true;
    set_entry_validation(state->terminal_width_entry, true, {});
    sync_cleared_entry(
        state, state->terminal_width_entry, terminal_width_setting_key(),
        std::to_string(setting_integer_value_or_default(
            state->draft_store, terminal_width_setting_key(), 80)));
    return;
  }
  gtk_entry_set_placeholder_text(GTK_ENTRY(state->terminal_width_entry),
                                 nullptr);
  gint64 value = 0;
  std::string reason;
  state->terminal_width_valid =
      parse_integer_entry(state->terminal_width_entry, 1, 1000, &value,
                          &reason);
  set_entry_validation(state->terminal_width_entry,
                       state->terminal_width_valid, reason);
  if (state->terminal_width_valid) {
    set_explicit_setting_value(&state->draft_store,
                               terminal_width_setting_key(),
                               SettingValue{value});
  }
}

static void update_terminal_height_from_widget(SettingsWidgetState *state) {
  const char *text =
      gtk_entry_get_text(GTK_ENTRY(state->terminal_height_entry));
  if (trim_ascii_whitespace(text == nullptr ? "" : text).empty()) {
    clear_explicit_setting_value(&state->draft_store,
                                 terminal_height_setting_key());
    state->terminal_height_valid = true;
    set_entry_validation(state->terminal_height_entry, true, {});
    sync_cleared_entry(
        state, state->terminal_height_entry, terminal_height_setting_key(),
        std::to_string(setting_integer_value_or_default(
            state->draft_store, terminal_height_setting_key(), 24)));
    return;
  }
  gtk_entry_set_placeholder_text(GTK_ENTRY(state->terminal_height_entry),
                                 nullptr);
  gint64 value = 0;
  std::string reason;
  state->terminal_height_valid =
      parse_integer_entry(state->terminal_height_entry, 1, 1000, &value,
                          &reason);
  set_entry_validation(state->terminal_height_entry,
                       state->terminal_height_valid, reason);
  if (state->terminal_height_valid) {
    set_explicit_setting_value(&state->draft_store,
                               terminal_height_setting_key(),
                               SettingValue{value});
  }
}

static void
update_terminal_scrollback_lines_from_widget(SettingsWidgetState *state) {
  const char *text = gtk_entry_get_text(
      GTK_ENTRY(state->terminal_scrollback_lines_entry));
  if (trim_ascii_whitespace(text == nullptr ? "" : text).empty()) {
    clear_explicit_setting_value(
        &state->draft_store, terminal_scrollback_lines_setting_key());
    state->terminal_scrollback_lines_valid = true;
    set_entry_validation(state->terminal_scrollback_lines_entry, true, {});
    sync_cleared_entry(
        state, state->terminal_scrollback_lines_entry,
        terminal_scrollback_lines_setting_key(),
        std::to_string(setting_integer_value_or_default(
            state->draft_store, terminal_scrollback_lines_setting_key(),
            10000)));
    return;
  }
  gtk_entry_set_placeholder_text(
      GTK_ENTRY(state->terminal_scrollback_lines_entry), nullptr);
  gint64 value = 0;
  std::string reason;
  state->terminal_scrollback_lines_valid = parse_integer_entry(
      state->terminal_scrollback_lines_entry,
      minimum_terminal_scrollback_lines, maximum_terminal_scrollback_lines,
      &value, &reason);
  set_entry_validation(state->terminal_scrollback_lines_entry,
                       state->terminal_scrollback_lines_valid, reason);
  if (state->terminal_scrollback_lines_valid) {
    set_explicit_setting_value(
        &state->draft_store, terminal_scrollback_lines_setting_key(),
        SettingValue{value});
  }
}

static void
update_terminal_border_width_from_widget(SettingsWidgetState *state) {
  const char *text =
      gtk_entry_get_text(GTK_ENTRY(state->terminal_border_width_entry));
  if (trim_ascii_whitespace(text == nullptr ? "" : text).empty()) {
    clear_explicit_setting_value(&state->draft_store,
                                 terminal_border_width_setting_key());
    state->terminal_border_width_valid = true;
    set_entry_validation(state->terminal_border_width_entry, true, {});
    sync_cleared_entry(
        state, state->terminal_border_width_entry,
        terminal_border_width_setting_key(),
        std::to_string(terminal_border_width(state->draft_store)));
    return;
  }
  gtk_entry_set_placeholder_text(
      GTK_ENTRY(state->terminal_border_width_entry), nullptr);
  gint64 value = 0;
  std::string reason;
  state->terminal_border_width_valid = parse_integer_entry(
      state->terminal_border_width_entry, minimum_terminal_border_width,
      maximum_terminal_border_width, &value, &reason);
  set_entry_validation(state->terminal_border_width_entry,
                       state->terminal_border_width_valid, reason);
  if (state->terminal_border_width_valid) {
    set_explicit_setting_value(&state->draft_store,
                               terminal_border_width_setting_key(),
                               SettingValue{value});
  }
}

static void update_terminal_zoom_from_widget(SettingsWidgetState *state) {
  const char *text =
      gtk_entry_get_text(GTK_ENTRY(state->terminal_zoom_entry));
  if (trim_ascii_whitespace(text == nullptr ? "" : text).empty()) {
    clear_explicit_setting_value(&state->draft_store,
                                 terminal_zoom_setting_key());
    state->terminal_zoom_valid = true;
    set_entry_validation(state->terminal_zoom_entry, true, {});
    sync_cleared_entry(
        state, state->terminal_zoom_entry, terminal_zoom_setting_key(),
        format_double(setting_double_value_or_default(
            state->draft_store, terminal_zoom_setting_key(), 1.0)));
    return;
  }
  gtk_entry_set_placeholder_text(GTK_ENTRY(state->terminal_zoom_entry),
                                 nullptr);
  gdouble value = 0.0;
  std::string reason;
  state->terminal_zoom_valid =
      parse_double_entry(state->terminal_zoom_entry, 0.1, 5.0, &value,
                         &reason);
  set_entry_validation(state->terminal_zoom_entry,
                       state->terminal_zoom_valid, reason);
  if (state->terminal_zoom_valid) {
    set_explicit_setting_value(&state->draft_store,
                               terminal_zoom_setting_key(),
                               SettingValue{value});
  }
}

static void update_terminal_auto_close_from_widget(
    SettingsWidgetState *state) {
  const std::string choice = active_combo_id(
      state->terminal_auto_close_combo, inherit_choice);
  if (choice == inherit_choice) {
    clear_explicit_setting_value(&state->draft_store,
                                 terminal_auto_close_setting_key());
    return;
  }
  set_explicit_setting_value(
      &state->draft_store, terminal_auto_close_setting_key(),
      SettingValue{choice == boolean_enabled});
}

static void update_terminal_show_border_from_widget(
    SettingsWidgetState *state) {
  const std::string choice = active_combo_id(
      state->terminal_show_border_combo, inherit_choice);
  if (choice == inherit_choice) {
    clear_explicit_setting_value(&state->draft_store,
                                 terminal_show_border_setting_key());
    return;
  }
  set_explicit_setting_value(
      &state->draft_store, terminal_show_border_setting_key(),
      SettingValue{choice == boolean_enabled});
}

static void update_terminal_bell_sound_from_widget(
    SettingsWidgetState *state) {
  const char *text =
      gtk_entry_get_text(GTK_ENTRY(state->terminal_bell_sound_entry));
  const std::string value = text == nullptr ? "" : text;
  if (value.empty()) {
    clear_explicit_setting_value(&state->draft_store,
                                 terminal_bell_sound_setting_key());
    state->terminal_bell_sound_valid = true;
    set_entry_validation(state->terminal_bell_sound_entry, true, {});
    sync_cleared_entry(
        state, state->terminal_bell_sound_entry,
        terminal_bell_sound_setting_key(),
        setting_string_value_or_default(
            state->draft_store, terminal_bell_sound_setting_key(),
            "default"));
    return;
  }

  gtk_entry_set_placeholder_text(GTK_ENTRY(state->terminal_bell_sound_entry),
                                 nullptr);
  std::string reason;
  state->terminal_bell_sound_valid =
      terminal_bell_sound_is_valid(value, &reason);
  set_entry_validation(state->terminal_bell_sound_entry,
                       state->terminal_bell_sound_valid, reason);
  if (state->terminal_bell_sound_valid) {
    set_explicit_setting_value(&state->draft_store,
                               terminal_bell_sound_setting_key(),
                               SettingValue{value});
  }
}

static void sync_terminal_font_controls(SettingsWidgetState *state) {
  const TerminalFontFamilies fonts = terminal_font_families(state->draft_store);
  const std::string primary =
      fonts.primary_family.value_or(default_font_button_family);
  const std::string fallback = fonts.fallback_family.value_or(primary);

  const auto populate_mode_combo = [state](GtkWidget *combo,
                                            const SettingKey &key) {
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(combo));
    const std::string fallback_value = std::get<std::string>(
        setting_fallback_value(state->draft_store, key,
                               SettingValue{std::string()}));
    const std::string fallback_display =
        fallback_value.empty()
            ? std::string()
            : fallback_value == terminal_font_default
                  ? settings_ui_text(SettingsUiText::use_built_in_default)
                  : settings_ui_text(SettingsUiText::custom_font);
    const std::string inherited = setting_fallback_label(
        state->draft_store, key, fallback_display);
    append_combo_option(combo, inherit_choice, inherited.c_str());
    append_combo_option(
        combo, terminal_font_default,
        settings_ui_text(SettingsUiText::use_built_in_default));
    append_combo_option(combo, terminal_font_custom,
                        settings_ui_text(SettingsUiText::custom_font));

    const std::string effective = setting_string_value_or_default(
        state->draft_store, key, std::string());
    const char *active = inherit_choice;
    if (setting_has_explicit_value(state->draft_store, key)) {
      active = effective == terminal_font_default ? terminal_font_default
                                                   : terminal_font_custom;
    }
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), active);
  };

  populate_mode_combo(state->terminal_font_primary_mode_combo,
                      terminal_font_primary_family_setting_key());
  populate_mode_combo(state->terminal_font_fallback_mode_combo,
                      terminal_font_fallback_family_setting_key());
  set_font_button_family(state->terminal_font_primary_button, primary);
  set_font_button_family(state->terminal_font_fallback_button, fallback);
  gtk_widget_set_sensitive(state->terminal_font_primary_button, TRUE);
  gtk_widget_set_sensitive(state->terminal_font_fallback_button, TRUE);
}

static void update_terminal_font_mode_from_widget(
    SettingsWidgetState *state, GtkWidget *mode_combo,
    GtkWidget *button, const SettingKey &key) {
  const std::string choice = active_combo_id(mode_combo, inherit_choice);
  if (choice == inherit_choice) {
    clear_explicit_setting_value(&state->draft_store, key);
  } else if (choice == terminal_font_default) {
    set_explicit_setting_value(
        &state->draft_store, key,
        SettingValue{std::string(terminal_font_default)});
  } else {
    const std::optional<std::string> family = font_button_family(button);
    if (family.has_value()) {
      set_explicit_setting_value(&state->draft_store, key,
                                 SettingValue{family.value()});
    }
  }

  const bool previous_synchronizing = state->synchronizing;
  state->synchronizing = true;
  sync_terminal_font_controls(state);
  state->synchronizing = previous_synchronizing;
}

static void update_terminal_font_family_from_widget(
    SettingsWidgetState *state, GtkWidget *mode_combo,
    GtkWidget *button, const SettingKey &key) {
  const std::optional<std::string> family = font_button_family(button);
  if (!family.has_value()) {
    return;
  }

  const bool previous_synchronizing = state->synchronizing;
  state->synchronizing = true;
  gtk_combo_box_set_active_id(GTK_COMBO_BOX(mode_combo),
                              terminal_font_custom);
  state->synchronizing = previous_synchronizing;
  set_explicit_setting_value(&state->draft_store, key,
                             SettingValue{family.value()});
}

enum class GeneralColorField {
  exterior_background,
  background,
};

static SettingKey general_color_setting_key(GeneralColorField field) {
  return field == GeneralColorField::exterior_background
             ? general_exterior_background_setting_key()
             : general_background_setting_key();
}

static GtkWidget *general_color_mode_combo(
    SettingsWidgetState *state, GeneralColorField field) {
  return field == GeneralColorField::exterior_background
             ? state->general_exterior_background_mode_combo
             : state->general_background_mode_combo;
}

static GtkWidget *general_color_button(
    SettingsWidgetState *state, GeneralColorField field) {
  return field == GeneralColorField::exterior_background
             ? state->general_exterior_background_button
             : state->general_background_button;
}

static void set_color_button_rgb(
    GtkWidget *button, const std::optional<RgbColor> &color);

static gint rgb_channel_value(gdouble channel) {
  return std::clamp(
      static_cast<gint>(std::lround(channel * 255.0)), 0, 255);
}

static std::string color_button_rgb(GtkWidget *button) {
  GdkRGBA color{};
  gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(button), &color);
  std::ostringstream stream;
  stream << '#' << std::uppercase << std::hex << std::setfill('0')
         << std::setw(2) << rgb_channel_value(color.red)
         << std::setw(2) << rgb_channel_value(color.green)
         << std::setw(2) << rgb_channel_value(color.blue);
  return stream.str();
}

static void update_general_color_mode_from_widget(
    SettingsWidgetState *state, GeneralColorField field) {
  GtkWidget *combo = general_color_mode_combo(state, field);
  GtkWidget *button = general_color_button(state, field);
  const SettingKey key = general_color_setting_key(field);
  const std::string choice = active_combo_id(combo, inherit_choice);
  if (choice == inherit_choice) {
    clear_explicit_setting_value(&state->draft_store, key);
    const GeneralColorSettings colors =
        general_color_settings(state->draft_store);
    set_color_button_rgb(
        button, field == GeneralColorField::exterior_background
                    ? colors.exterior_background
                    : colors.background);
  } else if (choice == general_color_none) {
    set_explicit_setting_value(
        &state->draft_store, key,
        SettingValue{std::string(general_color_none)});
  } else {
    set_explicit_setting_value(&state->draft_store, key,
                               SettingValue{color_button_rgb(button)});
  }
}

static void update_general_color_from_picker(
    SettingsWidgetState *state, GeneralColorField field) {
  GtkWidget *combo = general_color_mode_combo(state, field);
  GtkWidget *button = general_color_button(state, field);
  const bool previous_synchronizing = state->synchronizing;
  state->synchronizing = true;
  gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), general_color_custom);
  state->synchronizing = previous_synchronizing;
  set_explicit_setting_value(
      &state->draft_store, general_color_setting_key(field),
      SettingValue{color_button_rgb(button)});
}

static void set_terminal_encoding_validation(
    SettingsWidgetState *state, bool valid, const std::string &reason) {
  state->terminal_encoding_valid = valid;
  if (state->terminal_encoding_entry == nullptr) {
    return;
  }
  gtk_entry_set_icon_from_icon_name(
      GTK_ENTRY(state->terminal_encoding_entry), GTK_ENTRY_ICON_SECONDARY,
      valid ? nullptr : "dialog-error-symbolic");
  const std::string message = settings_validation_message(reason);
  gtk_entry_set_icon_tooltip_text(
      GTK_ENTRY(state->terminal_encoding_entry), GTK_ENTRY_ICON_SECONDARY,
      valid ? nullptr : message.c_str());
}

static void update_terminal_encoding_from_widget(SettingsWidgetState *state) {
  if (state->terminal_encoding_combo == nullptr ||
      state->terminal_encoding_entry == nullptr) {
    return;
  }

  const char *active_id = gtk_combo_box_get_active_id(
      GTK_COMBO_BOX(state->terminal_encoding_combo));
  const char *entry_text =
      gtk_entry_get_text(GTK_ENTRY(state->terminal_encoding_entry));
  const std::string entered = entry_text == nullptr ? "" : entry_text;
  if ((active_id != nullptr &&
       std::string(active_id) == terminal_text_default) ||
      trim_ascii_whitespace(entered).empty()) {
    clear_explicit_setting_value(&state->draft_store,
                                 terminal_encoding_setting_key());
    set_terminal_encoding_validation(state, true, {});
    const TerminalTextSettings built_in_defaults =
        default_terminal_text_settings(
            connection_kind_value(state->draft_store));
    const std::string fallback =
        terminal_encoding_fallback(state, built_in_defaults);
    const std::string placeholder = setting_fallback_label(
        state->draft_store, terminal_encoding_setting_key(), fallback);
    const bool previous_synchronizing = state->synchronizing;
    state->synchronizing = true;
    gtk_entry_set_text(GTK_ENTRY(state->terminal_encoding_entry), "");
    gtk_entry_set_placeholder_text(GTK_ENTRY(state->terminal_encoding_entry),
                                   placeholder.c_str());
    state->synchronizing = previous_synchronizing;
    return;
  }

  gtk_entry_set_placeholder_text(GTK_ENTRY(state->terminal_encoding_entry),
                                 nullptr);
  std::string encoding = trim_ascii_whitespace(entered);
  if (active_id != nullptr &&
      std::string(active_id) != terminal_text_default &&
      entered == active_id) {
    encoding = active_id;
  }
  std::string reason;
  if (!terminal_encoding_name_is_valid(encoding, &reason)) {
    set_terminal_encoding_validation(state, false, reason);
    return;
  }

  set_terminal_encoding_validation(state, true, {});
  set_explicit_setting_value(&state->draft_store,
                             terminal_encoding_setting_key(),
                             SettingValue{std::move(encoding)});
}

static void update_terminal_backspace_code_from_widget(
    SettingsWidgetState *state) {
  const std::string choice = active_combo_id(
      state->terminal_backspace_code_combo, terminal_text_default);
  if (choice == terminal_text_default) {
    clear_explicit_setting_value(&state->draft_store,
                                 terminal_backspace_code_setting_key());
    return;
  }
  set_explicit_setting_value(&state->draft_store,
                             terminal_backspace_code_setting_key(),
                             SettingValue{choice});
}

static void update_terminal_cursor_key_mode_from_widget(
    SettingsWidgetState *state) {
  const std::string choice = active_combo_id(
      state->terminal_cursor_key_mode_combo, terminal_text_default);
  if (choice == terminal_text_default) {
    clear_explicit_setting_value(&state->draft_store,
                                 terminal_cursor_key_mode_setting_key());
    return;
  }
  set_explicit_setting_value(&state->draft_store,
                             terminal_cursor_key_mode_setting_key(),
                             SettingValue{choice});
}

static void update_terminal_return_code_from_widget(
    SettingsWidgetState *state) {
  const std::string choice = active_combo_id(
      state->terminal_return_code_combo, terminal_text_default);
  if (choice == terminal_text_default) {
    clear_explicit_setting_value(&state->draft_store,
                                 terminal_return_code_setting_key());
    return;
  }
  set_explicit_setting_value(&state->draft_store,
                             terminal_return_code_setting_key(),
                             SettingValue{choice});
}

static bool terminal_key_binding_inputs_conflict(
    SettingsWidgetState *state) {
  const auto binding_text = [state](KeyBindingInputWidgetState *input,
                                    const SettingKey &key,
                                    const std::string &effective) {
    return setting_has_explicit_value(state->draft_store, key)
               ? key_binding_input_widget_text(input)
               : effective;
  };
  const std::array<KeyBindingParseResult, 3> bindings = {
      parse_key_binding(binding_text(
          state->terminal_zoom_in_key_input,
          terminal_zoom_in_key_setting_key(),
          terminal_zoom_in_key(state->draft_store))),
      parse_key_binding(binding_text(
          state->terminal_zoom_out_key_input,
          terminal_zoom_out_key_setting_key(),
          terminal_zoom_out_key(state->draft_store))),
      parse_key_binding(binding_text(
          state->terminal_send_break_key_input,
          terminal_send_break_key_setting_key(),
          terminal_send_break_key(state->draft_store))),
  };
  for (std::size_t left = 0; left < bindings.size(); ++left) {
    if (!bindings[left].error.empty() ||
        !bindings[left].binding.has_value()) {
      continue;
    }
    for (std::size_t right = left + 1; right < bindings.size(); ++right) {
      if (bindings[right].error.empty() &&
          bindings[right].binding.has_value() &&
          key_bindings_equal(*bindings[left].binding,
                             *bindings[right].binding)) {
        return true;
      }
    }
  }
  return false;
}

static bool terminal_key_binding_inputs_valid(
    const SettingsWidgetState *state) {
  return key_binding_input_widget_is_valid(
             state->terminal_zoom_in_key_input) &&
         key_binding_input_widget_is_valid(
             state->terminal_zoom_out_key_input) &&
         key_binding_input_widget_is_valid(
             state->terminal_send_break_key_input);
}

static bool connection_hotkey_input_valid(
    const SettingsWidgetState *state) {
  return state->general_open_connection_input == nullptr ||
         key_binding_input_widget_is_valid(
             state->general_open_connection_input);
}

static bool macro_rules_are_valid(const SettingsWidgetState *state) {
  for (std::size_t index = 0; index < state->draft_store.macro_rules.size();
       ++index) {
    const MacroRule &rule = state->draft_store.macro_rules[index];
    std::string reason;
    if (!macro_rule_is_valid(rule, &reason)) {
      return false;
    }
    const bool duplicated = std::any_of(
        state->draft_store.macro_rules.begin() + index + 1,
        state->draft_store.macro_rules.end(),
        [&rule](const MacroRule &other) { return other.id == rule.id; });
    if (duplicated) {
      return false;
    }
  }
  return true;
}

static bool settings_inputs_valid(const SettingsWidgetState *state) {
  return state->local_command_line_valid && state->terminal_width_valid &&
         state->terminal_height_valid &&
         state->terminal_scrollback_lines_valid &&
         state->terminal_border_width_valid && state->terminal_zoom_valid &&
         state->terminal_encoding_valid &&
         state->terminal_bell_sound_valid &&
         state->telnet_port_valid && state->ssh_port_valid &&
         state->ftp_port_valid &&
         state->serial_baudrate_valid &&
         state->transfer_text_send_rate_valid &&
         terminal_key_binding_inputs_valid(state) &&
         connection_hotkey_input_valid(state) &&
         state->log_file_name_format_valid && macro_rules_are_valid(state);
}

static void notify_changed(SettingsWidgetState *state) {
  if (!state->synchronizing && state->callbacks.changed) {
    state->callbacks.changed();
  }
}

static void update_action_sensitivity(SettingsWidgetState *state) {
  const gboolean sensitive = settings_inputs_valid(state) ? TRUE : FALSE;
  if (state->apply_button != nullptr) {
    gtk_widget_set_sensitive(state->apply_button, sensitive);
  }
  if (state->save_button != nullptr) {
    gtk_widget_set_sensitive(state->save_button, sensitive);
  }
}

static void update_terminal_key_binding_validation(
    SettingsWidgetState *state) {
  constexpr char conflict_error[] =
      "Terminal actions must use different key bindings";
  const bool conflict = terminal_key_binding_inputs_conflict(state);
  set_key_binding_input_widget_external_error(
      state->terminal_zoom_in_key_input,
      conflict ? conflict_error : std::string());
  set_key_binding_input_widget_external_error(
      state->terminal_zoom_out_key_input,
      conflict ? conflict_error : std::string());
  set_key_binding_input_widget_external_error(
      state->terminal_send_break_key_input,
      conflict ? conflict_error : std::string());
  update_action_sensitivity(state);
}

static void update_terminal_zoom_in_key_from_widget(
    SettingsWidgetState *state) {
  if (!key_binding_input_widget_is_valid(
          state->terminal_zoom_in_key_input)) {
    return;
  }
  set_explicit_setting_value(
      &state->draft_store, terminal_zoom_in_key_setting_key(),
      SettingValue{
          key_binding_input_widget_text(state->terminal_zoom_in_key_input)});
}

static void update_terminal_zoom_out_key_from_widget(
    SettingsWidgetState *state) {
  if (!key_binding_input_widget_is_valid(
          state->terminal_zoom_out_key_input)) {
    return;
  }
  set_explicit_setting_value(
      &state->draft_store, terminal_zoom_out_key_setting_key(),
      SettingValue{
          key_binding_input_widget_text(state->terminal_zoom_out_key_input)});
}

static void update_terminal_send_break_key_from_widget(
    SettingsWidgetState *state) {
  if (!key_binding_input_widget_is_valid(
          state->terminal_send_break_key_input)) {
    return;
  }
  set_explicit_setting_value(
      &state->draft_store, terminal_send_break_key_setting_key(),
      SettingValue{key_binding_input_widget_text(
          state->terminal_send_break_key_input)});
}

static void update_local_command_line_from_widget(
    SettingsWidgetState *state) {
  const char *text =
      gtk_entry_get_text(GTK_ENTRY(state->local_command_line_entry));
  const std::string command_line = text == nullptr ? "" : text;
  if (trim_ascii_whitespace(command_line).empty()) {
    clear_explicit_setting_value(&state->draft_store,
                                 local_command_line_setting_key());
    state->local_command_line_valid = true;
    set_entry_validation(state->local_command_line_entry, true, {});
    sync_cleared_entry(
        state, state->local_command_line_entry,
        local_command_line_setting_key(),
        setting_string_value_or_default(
            state->draft_store, local_command_line_setting_key(),
            std::string()));
    return;
  }

  gtk_entry_set_placeholder_text(
      GTK_ENTRY(state->local_command_line_entry), nullptr);
  std::string reason;
  state->local_command_line_valid =
      local_command_line_is_valid(command_line, &reason);
  set_entry_validation(state->local_command_line_entry,
                       state->local_command_line_valid, reason);
  if (state->local_command_line_valid) {
    set_explicit_setting_value(&state->draft_store,
                               local_command_line_setting_key(),
                               SettingValue{command_line});
  }
}

static void update_telnet_address_from_widget(SettingsWidgetState *state) {
  update_string_entry(state, state->telnet_address_entry,
                      telnet_address_setting_key());
}

static void update_telnet_port_from_widget(SettingsWidgetState *state) {
  const char *text = gtk_entry_get_text(GTK_ENTRY(state->telnet_port_entry));
  if (trim_ascii_whitespace(text == nullptr ? "" : text).empty()) {
    clear_explicit_setting_value(&state->draft_store,
                                 telnet_port_setting_key());
    state->telnet_port_valid = true;
    set_entry_validation(state->telnet_port_entry, true, {});
    sync_cleared_entry(
        state, state->telnet_port_entry, telnet_port_setting_key(),
        std::to_string(setting_integer_value_or_default(
            state->draft_store, telnet_port_setting_key(), 23)));
    return;
  }
  gtk_entry_set_placeholder_text(GTK_ENTRY(state->telnet_port_entry),
                                 nullptr);
  gint64 value = 0;
  std::string reason;
  state->telnet_port_valid =
      parse_integer_entry(state->telnet_port_entry, 1, 65535, &value,
                          &reason);
  set_entry_validation(state->telnet_port_entry,
                       state->telnet_port_valid, reason);
  if (state->telnet_port_valid) {
    set_explicit_setting_value(&state->draft_store,
                               telnet_port_setting_key(),
                               SettingValue{value});
  }
}

static void update_telnet_terminal_type_from_widget(
    SettingsWidgetState *state) {
  const char *text =
      gtk_entry_get_text(GTK_ENTRY(state->telnet_terminal_type_entry));
  const std::string terminal_type = text == nullptr ? "" : text;
  if (ascii_blank(terminal_type)) {
    clear_explicit_setting_value(&state->draft_store,
                                 telnet_terminal_type_setting_key());
    sync_cleared_entry(
        state, state->telnet_terminal_type_entry,
        telnet_terminal_type_setting_key(),
        telnet_connection_settings(state->draft_store).terminal_type);
    return;
  }
  gtk_entry_set_placeholder_text(
      GTK_ENTRY(state->telnet_terminal_type_entry), nullptr);
  set_explicit_setting_value(&state->draft_store,
                             telnet_terminal_type_setting_key(),
                             SettingValue{terminal_type});
}

static void update_ssh_address_from_widget(SettingsWidgetState *state) {
  update_string_entry(state, state->ssh_address_entry,
                      ssh_address_setting_key());
}

static void update_ssh_port_from_widget(SettingsWidgetState *state) {
  const char *text = gtk_entry_get_text(GTK_ENTRY(state->ssh_port_entry));
  if (trim_ascii_whitespace(text == nullptr ? "" : text).empty()) {
    clear_explicit_setting_value(&state->draft_store,
                                 ssh_port_setting_key());
    state->ssh_port_valid = true;
    set_entry_validation(state->ssh_port_entry, true, {});
    sync_cleared_entry(
        state, state->ssh_port_entry, ssh_port_setting_key(),
        std::to_string(setting_integer_value_or_default(
            state->draft_store, ssh_port_setting_key(), 22)));
    return;
  }
  gtk_entry_set_placeholder_text(GTK_ENTRY(state->ssh_port_entry), nullptr);
  gint64 value = 0;
  std::string reason;
  state->ssh_port_valid =
      parse_integer_entry(state->ssh_port_entry, 1, 65535, &value,
                          &reason);
  set_entry_validation(state->ssh_port_entry, state->ssh_port_valid, reason);
  if (state->ssh_port_valid) {
    set_explicit_setting_value(&state->draft_store, ssh_port_setting_key(),
                               SettingValue{value});
  }
}

static void update_ssh_username_from_widget(SettingsWidgetState *state) {
  update_string_entry(state, state->ssh_username_entry,
                      ssh_username_setting_key());
}

static void update_ssh_identity_file_from_widget(
    SettingsWidgetState *state) {
  update_string_entry(state, state->ssh_identity_file_entry,
                      ssh_identity_file_setting_key());
}

static void update_ssh_terminal_type_from_widget(
    SettingsWidgetState *state) {
  const char *text =
      gtk_entry_get_text(GTK_ENTRY(state->ssh_terminal_type_entry));
  const std::string terminal_type = text == nullptr ? "" : text;
  if (ascii_blank(terminal_type)) {
    clear_explicit_setting_value(&state->draft_store,
                                 ssh_terminal_type_setting_key());
    sync_cleared_entry(
        state, state->ssh_terminal_type_entry,
        ssh_terminal_type_setting_key(),
        ssh_connection_settings(state->draft_store).terminal_type);
    return;
  }
  gtk_entry_set_placeholder_text(GTK_ENTRY(state->ssh_terminal_type_entry),
                                 nullptr);
  set_explicit_setting_value(&state->draft_store,
                             ssh_terminal_type_setting_key(),
                             SettingValue{terminal_type});
}

static void update_sftp_local_directory_from_widget(
    SettingsWidgetState *state) {
  update_string_entry(state, state->sftp_local_directory_entry,
                      sftp_local_directory_setting_key());
}

static void update_sftp_remote_directory_from_widget(
    SettingsWidgetState *state) {
  update_string_entry(state, state->sftp_remote_directory_entry,
                      sftp_remote_directory_setting_key());
}

static void update_ftp_address_from_widget(SettingsWidgetState *state) {
  update_string_entry(state, state->ftp_address_entry,
                      ftp_address_setting_key());
}

static void update_ftp_port_from_widget(SettingsWidgetState *state) {
  const char *text = gtk_entry_get_text(GTK_ENTRY(state->ftp_port_entry));
  if (trim_ascii_whitespace(text == nullptr ? "" : text).empty()) {
    clear_explicit_setting_value(&state->draft_store,
                                 ftp_port_setting_key());
    state->ftp_port_valid = true;
    set_entry_validation(state->ftp_port_entry, true, {});
    sync_cleared_entry(
        state, state->ftp_port_entry, ftp_port_setting_key(),
        std::to_string(setting_integer_value_or_default(
            state->draft_store, ftp_port_setting_key(), 21)));
    return;
  }
  gtk_entry_set_placeholder_text(GTK_ENTRY(state->ftp_port_entry), nullptr);
  gint64 value = 0;
  std::string reason;
  state->ftp_port_valid =
      parse_integer_entry(state->ftp_port_entry, 1, 65535, &value, &reason);
  set_entry_validation(state->ftp_port_entry, state->ftp_port_valid, reason);
  if (state->ftp_port_valid) {
    set_explicit_setting_value(&state->draft_store, ftp_port_setting_key(),
                               SettingValue{value});
  }
}

static void update_ftp_username_from_widget(SettingsWidgetState *state) {
  update_string_entry(state, state->ftp_username_entry,
                      ftp_username_setting_key());
}

static void
update_ftp_data_connection_mode_from_widget(SettingsWidgetState *state) {
  const std::string choice = active_combo_id(
      state->ftp_data_connection_mode_combo, inherit_choice);
  if (choice == inherit_choice) {
    clear_explicit_setting_value(
        &state->draft_store, ftp_data_connection_mode_setting_key());
    return;
  }
  set_explicit_setting_value(
      &state->draft_store, ftp_data_connection_mode_setting_key(),
      SettingValue{choice});
}

static void
update_ftp_local_directory_from_widget(SettingsWidgetState *state) {
  update_string_entry(state, state->ftp_local_directory_entry,
                      ftp_local_directory_setting_key());
}

static void
update_ftp_remote_directory_from_widget(SettingsWidgetState *state) {
  update_string_entry(state, state->ftp_remote_directory_entry,
                      ftp_remote_directory_setting_key());
}

static void update_serial_device_from_widget(SettingsWidgetState *state) {
  const std::string target =
      active_combo_id(state->serial_device_combo, inherit_choice);
  if (target == inherit_choice) {
    clear_explicit_setting_value(&state->draft_store,
                                 serial_device_setting_key());
    clear_explicit_setting_value(&state->draft_store,
                                 serial_device_usb_serial_setting_key());
  } else if (target == serial_device_no_device_choice) {
    set_explicit_setting_value(&state->draft_store,
                               serial_device_setting_key(),
                               SettingValue{std::string()});
    set_explicit_setting_value(&state->draft_store,
                               serial_device_usb_serial_setting_key(),
                               SettingValue{std::string()});
  } else {
    const std::optional<SerialDeviceChoice> choice =
        serial_device_choice_for_target(state->serial_device_choices, target);
    set_explicit_setting_value(&state->draft_store,
                               serial_device_setting_key(),
                               SettingValue{target});
    set_explicit_setting_value(
        &state->draft_store, serial_device_usb_serial_setting_key(),
        SettingValue{choice.has_value()
                         ? choice->usb_serial.value_or("")
                         : std::string()});
  }
  sync_serial_device_metadata(
      state, serial_connection_settings(state->draft_store));
}

static void update_serial_device_match_mode_from_widget(
    SettingsWidgetState *state) {
  const SerialConnectionSettings previous =
      serial_connection_settings(state->draft_store);
  const bool device_was_explicit = setting_has_explicit_value(
      state->draft_store, serial_device_setting_key());
  const std::string mode = active_combo_id(
      state->serial_device_match_mode_combo, inherit_choice);
  if (mode == inherit_choice) {
    clear_explicit_setting_value(&state->draft_store,
                                 serial_device_match_mode_setting_key());
  } else {
    set_explicit_setting_value(&state->draft_store,
                               serial_device_match_mode_setting_key(),
                               SettingValue{mode});
  }

  const SerialDeviceMatchMode next_mode =
      serial_connection_settings(state->draft_store).device_match_mode;
  if (!previous.device.empty() &&
      (device_was_explicit || mode != inherit_choice)) {
    const SerialDevicePaths paths = host_serial_device_paths();
    const std::optional<std::string> mapped =
        resolve_serial_device_target_for_mode(next_mode, previous.device,
                                              paths);
    if (mapped.has_value()) {
      set_explicit_setting_value(&state->draft_store,
                                 serial_device_setting_key(),
                                 SettingValue{*mapped});
      const std::vector<SerialDeviceChoice> choices =
          list_serial_device_choices(next_mode, paths);
      const std::optional<SerialDeviceChoice> choice =
          serial_device_choice_for_target(choices, *mapped);
      set_explicit_setting_value(
          &state->draft_store, serial_device_usb_serial_setting_key(),
          SettingValue{choice.has_value()
                           ? choice->usb_serial.value_or("")
                           : previous.device_usb_serial.value_or("")});
    } else {
      set_explicit_setting_value(&state->draft_store,
                                 serial_device_setting_key(),
                                 SettingValue{std::string()});
      set_explicit_setting_value(&state->draft_store,
                                 serial_device_usb_serial_setting_key(),
                                 SettingValue{std::string()});
    }
  }
  sync_serial_device_widgets(state);
}

static void update_serial_baudrate_from_widget(SettingsWidgetState *state) {
  const char *text =
      gtk_entry_get_text(GTK_ENTRY(state->serial_baudrate_entry));
  if (trim_ascii_whitespace(text == nullptr ? "" : text).empty()) {
    clear_explicit_setting_value(&state->draft_store,
                                 serial_baudrate_setting_key());
    state->serial_baudrate_valid = true;
    set_entry_validation(state->serial_baudrate_entry, true, {});
    sync_cleared_entry(
        state, state->serial_baudrate_entry,
        serial_baudrate_setting_key(),
        std::to_string(setting_integer_value_or_default(
            state->draft_store, serial_baudrate_setting_key(), 115200)));
    return;
  }
  gtk_entry_set_placeholder_text(GTK_ENTRY(state->serial_baudrate_entry),
                                 nullptr);
  gint64 value = 0;
  std::string reason;
  state->serial_baudrate_valid = parse_integer_entry(
      state->serial_baudrate_entry, 150, 8000000, &value, &reason);
  set_entry_validation(state->serial_baudrate_entry,
                       state->serial_baudrate_valid, reason);
  if (state->serial_baudrate_valid) {
    set_explicit_setting_value(&state->draft_store,
                               serial_baudrate_setting_key(),
                               SettingValue{value});
  }
}

static void update_serial_bits_from_widget(SettingsWidgetState *state) {
  const std::string bits =
      active_combo_id(state->serial_bits_combo, inherit_choice);
  if (bits == inherit_choice) {
    clear_explicit_setting_value(&state->draft_store,
                                 serial_bits_setting_key());
    return;
  }
  set_explicit_setting_value(
      &state->draft_store, serial_bits_setting_key(),
      SettingValue{static_cast<gint64>(std::stoll(bits))});
}

static void update_serial_parity_from_widget(SettingsWidgetState *state) {
  const std::string parity =
      active_combo_id(state->serial_parity_combo, inherit_choice);
  if (parity == inherit_choice) {
    clear_explicit_setting_value(&state->draft_store,
                                 serial_parity_setting_key());
    return;
  }
  set_explicit_setting_value(&state->draft_store,
                             serial_parity_setting_key(),
                             SettingValue{parity});
}

static void update_serial_stop_bit_from_widget(SettingsWidgetState *state) {
  const std::string stop_bit =
      active_combo_id(state->serial_stop_bit_combo, inherit_choice);
  if (stop_bit == inherit_choice) {
    clear_explicit_setting_value(&state->draft_store,
                                 serial_stop_bit_setting_key());
    return;
  }
  set_explicit_setting_value(
      &state->draft_store, serial_stop_bit_setting_key(),
      SettingValue{static_cast<gint64>(std::stoll(stop_bit))});
}

static void update_serial_flow_control_from_widget(
    SettingsWidgetState *state) {
  const std::string flow_control =
      active_combo_id(state->serial_flow_control_combo, inherit_choice);
  if (flow_control == inherit_choice) {
    clear_explicit_setting_value(&state->draft_store,
                                 serial_flow_control_setting_key());
    return;
  }
  set_explicit_setting_value(&state->draft_store,
                             serial_flow_control_setting_key(),
                             SettingValue{flow_control});
}

static void update_serial_carrier_detect_from_widget(
    SettingsWidgetState *state) {
  const std::string carrier_detect =
      active_combo_id(state->serial_carrier_detect_combo, inherit_choice);
  if (carrier_detect == inherit_choice) {
    clear_explicit_setting_value(&state->draft_store,
                                 serial_carrier_detect_setting_key());
    return;
  }
  set_explicit_setting_value(&state->draft_store,
                             serial_carrier_detect_setting_key(),
                             SettingValue{carrier_detect});
}

static void update_transfer_base_path_from_widget(
    SettingsWidgetState *state) {
  update_string_entry(state, state->transfer_base_path_entry,
                      transfer_base_path_setting_key());
}

static void update_transfer_text_send_rate_from_widget(
    SettingsWidgetState *state) {
  const char *text =
      gtk_entry_get_text(GTK_ENTRY(state->transfer_text_send_rate_entry));
  if (trim_ascii_whitespace(text == nullptr ? "" : text).empty()) {
    clear_explicit_setting_value(
        &state->draft_store,
        transfer_text_send_bytes_per_second_setting_key());
    state->transfer_text_send_rate_valid = true;
    set_entry_validation(state->transfer_text_send_rate_entry, true, {});
    sync_cleared_entry(
        state, state->transfer_text_send_rate_entry,
        transfer_text_send_bytes_per_second_setting_key(),
        std::to_string(setting_integer_value_or_default(
            state->draft_store,
            transfer_text_send_bytes_per_second_setting_key(), 1024)));
    return;
  }
  gtk_entry_set_placeholder_text(
      GTK_ENTRY(state->transfer_text_send_rate_entry), nullptr);
  gint64 value = 0;
  std::string reason;
  state->transfer_text_send_rate_valid = parse_integer_entry(
      state->transfer_text_send_rate_entry, 1, 8000000, &value, &reason);
  set_entry_validation(state->transfer_text_send_rate_entry,
                       state->transfer_text_send_rate_valid, reason);
  if (state->transfer_text_send_rate_valid) {
    set_explicit_setting_value(
        &state->draft_store,
        transfer_text_send_bytes_per_second_setting_key(),
        SettingValue{value});
  }
}

static void update_transfer_zmodem_autostart_from_widget(
    SettingsWidgetState *state) {
  const std::string choice = active_combo_id(
      state->transfer_zmodem_autostart_combo, inherit_choice);
  if (choice == inherit_choice) {
    clear_explicit_setting_value(&state->draft_store,
                                 transfer_zmodem_autostart_setting_key());
    return;
  }

  set_explicit_setting_value(
      &state->draft_store, transfer_zmodem_autostart_setting_key(),
      SettingValue{choice == zmodem_autostart_enabled});
}

static void update_transfer_text_send_follow_return_code_from_widget(
    SettingsWidgetState *state) {
  const std::string choice = active_combo_id(
      state->transfer_text_send_follow_return_code_combo, inherit_choice);
  if (choice == inherit_choice) {
    clear_explicit_setting_value(
        &state->draft_store,
        transfer_text_send_follow_return_code_setting_key());
    return;
  }
  set_explicit_setting_value(
      &state->draft_store,
      transfer_text_send_follow_return_code_setting_key(),
      SettingValue{choice == boolean_enabled});
}

static void update_log_enabled_from_widget(SettingsWidgetState *state) {
  const std::string choice =
      active_combo_id(state->log_enabled_combo, inherit_choice);
  if (choice == inherit_choice) {
    clear_explicit_setting_value(&state->draft_store,
                                 terminal_log_enabled_setting_key());
    return;
  }
  set_explicit_setting_value(
      &state->draft_store, terminal_log_enabled_setting_key(),
      SettingValue{choice == boolean_enabled});
}

static void update_log_base_directory_from_widget(
    SettingsWidgetState *state) {
  update_string_entry(state, state->log_base_directory_entry,
                      terminal_log_base_directory_setting_key());
}

static void set_log_file_name_format_validation(
    SettingsWidgetState *state, bool valid, const std::string &reason) {
  state->log_file_name_format_valid = valid;
  gtk_entry_set_icon_from_icon_name(
      GTK_ENTRY(state->log_file_name_format_entry), GTK_ENTRY_ICON_SECONDARY,
      valid ? nullptr : "dialog-error-symbolic");
  const std::string message = settings_validation_message(reason);
  gtk_entry_set_icon_tooltip_text(
      GTK_ENTRY(state->log_file_name_format_entry), GTK_ENTRY_ICON_SECONDARY,
      valid ? nullptr : message.c_str());
}

static void update_log_file_name_format_from_widget(
    SettingsWidgetState *state) {
  const char *text =
      gtk_entry_get_text(GTK_ENTRY(state->log_file_name_format_entry));
  const std::string format = text == nullptr ? "" : text;
  if (format.empty()) {
    clear_explicit_setting_value(
        &state->draft_store,
        terminal_log_file_name_format_setting_key());
    set_log_file_name_format_validation(state, true, {});
    sync_cleared_entry(
        state, state->log_file_name_format_entry,
        terminal_log_file_name_format_setting_key(),
        terminal_log_settings(state->draft_store).file_name_format);
    return;
  }
  gtk_entry_set_placeholder_text(
      GTK_ENTRY(state->log_file_name_format_entry), nullptr);
  std::string reason;
  if (!terminal_log_file_name_format_is_valid(format, &reason)) {
    set_log_file_name_format_validation(state, false, reason);
    return;
  }

  set_log_file_name_format_validation(state, true, {});
  set_explicit_setting_value(&state->draft_store,
                             terminal_log_file_name_format_setting_key(),
                             SettingValue{format});
}

static void update_log_mode_from_widget(SettingsWidgetState *state) {
  const std::string mode =
      active_combo_id(state->log_mode_combo, inherit_choice);
  if (mode == inherit_choice) {
    clear_explicit_setting_value(&state->draft_store,
                                 terminal_log_mode_setting_key());
    return;
  }
  set_explicit_setting_value(&state->draft_store,
                             terminal_log_mode_setting_key(),
                             SettingValue{mode});
}

static void on_macro_argument_changed(GtkEditable *editable, gpointer data);
static void on_macro_argument_move_up_clicked(GtkButton *button,
                                              gpointer data);
static void on_macro_argument_move_down_clicked(GtkButton *button,
                                                gpointer data);
static void on_macro_argument_remove_clicked(GtkButton *button,
                                             gpointer data);

static MacroRule *selected_macro_rule(SettingsWidgetState *state) {
  if (state->selected_macro < 0 ||
      static_cast<std::size_t>(state->selected_macro) >=
          state->draft_store.macro_rules.size()) {
    return nullptr;
  }
  return &state->draft_store.macro_rules[state->selected_macro];
}

static void clear_container(GtkWidget *container) {
  GList *children = gtk_container_get_children(GTK_CONTAINER(container));
  for (GList *child = children; child != nullptr; child = child->next) {
    gtk_widget_destroy(GTK_WIDGET(child->data));
  }
  g_list_free(children);
}

static void set_text_view_validation(GtkWidget *view, bool valid,
                                     const std::string &reason) {
  GtkStyleContext *context = gtk_widget_get_style_context(view);
  if (valid) {
    gtk_style_context_remove_class(context, GTK_STYLE_CLASS_ERROR);
    gtk_widget_set_tooltip_text(view, nullptr);
    return;
  }
  gtk_style_context_add_class(context, GTK_STYLE_CLASS_ERROR);
  const std::string message = settings_validation_message(reason);
  gtk_widget_set_tooltip_text(view, message.c_str());
}

static bool macro_id_is_duplicated(const SettingsWidgetState *state,
                                   const MacroRule *selected) {
  return std::count_if(
             state->draft_store.macro_rules.begin(),
             state->draft_store.macro_rules.end(),
             [selected](const MacroRule &rule) {
               return rule.id == selected->id;
             }) > 1;
}

static void update_macro_validation(SettingsWidgetState *state) {
  const MacroRule *rule = selected_macro_rule(state);
  if (rule == nullptr) {
    set_entry_validation(state->macro_id_entry, true, {});
    set_entry_validation(state->macro_regex_entry, true, {});
    set_entry_validation(state->macro_command_entry, true, {});
    set_text_view_validation(state->macro_send_view, true, {});
    update_action_sensitivity(state);
    return;
  }

  std::string id_reason;
  bool id_valid = macro_rule_id_is_valid(rule->id, &id_reason);
  if (id_valid && macro_id_is_duplicated(state, rule)) {
    id_valid = false;
    id_reason = "identifier must be unique";
  }
  set_entry_validation(state->macro_id_entry, id_valid, id_reason);

  std::string regex_reason;
  bool regex_valid = !rule->pattern.empty();
  if (!regex_valid) {
    regex_reason = "regular expression must not be empty";
  } else {
    GError *error = nullptr;
    GRegex *regex = g_regex_new(rule->pattern.c_str(), G_REGEX_DEFAULT,
                                G_REGEX_MATCH_DEFAULT, &error);
    if (regex == nullptr) {
      regex_valid = false;
      regex_reason = error == nullptr || error->message == nullptr
                         ? "invalid regular expression"
                         : std::string(error->message);
    }
    if (regex != nullptr) {
      g_regex_unref(regex);
    }
    g_clear_error(&error);
  }
  set_entry_validation(state->macro_regex_entry, regex_valid, regex_reason);

  std::string rule_reason;
  const bool rule_valid = macro_rule_is_valid(*rule, &rule_reason);
  if (std::holds_alternative<MacroSendAction>(rule->action)) {
    set_entry_validation(state->macro_command_entry, true, {});
    set_text_view_validation(state->macro_send_view, rule_valid,
                             rule_reason);
  } else {
    set_text_view_validation(state->macro_send_view, true, {});
    set_entry_validation(state->macro_command_entry, rule_valid,
                         rule_reason);
  }
  update_action_sensitivity(state);
}

static void update_macro_rule_button_sensitivity(
    SettingsWidgetState *state) {
  const bool selected = selected_macro_rule(state) != nullptr;
  const bool can_move_up = selected && state->selected_macro > 0;
  const bool can_move_down =
      selected && static_cast<std::size_t>(state->selected_macro + 1) <
                      state->draft_store.macro_rules.size();
  gtk_widget_set_sensitive(state->macro_remove_button, selected);
  gtk_widget_set_sensitive(state->macro_move_up_button, can_move_up);
  gtk_widget_set_sensitive(state->macro_move_down_button, can_move_down);
}

static int macro_argument_index(GtkWidget *widget) {
  return GPOINTER_TO_INT(
             g_object_get_data(G_OBJECT(widget), "elder-terms-macro-index")) -
         1;
}

static void rebuild_macro_arguments(SettingsWidgetState *state) {
  clear_container(state->macro_arguments_box);
  const MacroRule *rule = selected_macro_rule(state);
  const auto *command =
      rule == nullptr ? nullptr
                      : std::get_if<MacroCommandAction>(&rule->action);
  if (command == nullptr) {
    return;
  }

  for (std::size_t index = 0; index < command->arguments.size(); ++index) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *entry = create_entry(
        widget_id(state, ("macro_argument_" + std::to_string(index) +
                          "_entry")
                             .c_str()));
    gtk_entry_set_text(GTK_ENTRY(entry), command->arguments[index].c_str());
    gtk_widget_set_hexpand(entry, TRUE);
    g_object_set_data(G_OBJECT(entry), "elder-terms-macro-index",
                      GINT_TO_POINTER(static_cast<int>(index) + 1));
    g_signal_connect(entry, "changed", G_CALLBACK(on_macro_argument_changed),
                     state);
    gtk_box_pack_start(GTK_BOX(row), entry, TRUE, TRUE, 0);

    const auto create_argument_button =
        [state, index](const char *label, const char *suffix,
                       SettingsUiText tooltip, GCallback callback) {
          GtkWidget *button = gtk_button_new_with_label(label);
          const std::string id =
              widget_id(state, ("macro_argument_" + std::to_string(index) +
                                suffix)
                                   .c_str());
          assign_accessible_id(button, id.c_str());
          gtk_widget_set_tooltip_text(button, settings_ui_text(tooltip));
          g_object_set_data(G_OBJECT(button), "elder-terms-macro-index",
                            GINT_TO_POINTER(static_cast<int>(index) + 1));
          g_signal_connect(button, "clicked", callback, state);
          return button;
        };
    GtkWidget *up = create_argument_button(
        "↑", "_move_up_button", SettingsUiText::macro_move_up,
        G_CALLBACK(on_macro_argument_move_up_clicked));
    GtkWidget *down = create_argument_button(
        "↓", "_move_down_button", SettingsUiText::macro_move_down,
        G_CALLBACK(on_macro_argument_move_down_clicked));
    GtkWidget *remove = create_argument_button(
        "−", "_remove_button", SettingsUiText::macro_remove_argument,
        G_CALLBACK(on_macro_argument_remove_clicked));
    gtk_widget_set_sensitive(up, index > 0);
    gtk_widget_set_sensitive(down, index + 1 < command->arguments.size());
    gtk_box_pack_start(GTK_BOX(row), up, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), down, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), remove, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(state->macro_arguments_box), row, FALSE, FALSE,
                       0);
    gtk_widget_show_all(row);
  }
  gtk_widget_show_all(state->macro_arguments_box);
}

static void sync_macro_editor(SettingsWidgetState *state) {
  if (state->macro_editor == nullptr) {
    return;
  }
  const bool previous_synchronizing = state->synchronizing;
  state->synchronizing = true;
  const MacroRule *rule = selected_macro_rule(state);
  const bool selected = rule != nullptr;
  gtk_widget_set_sensitive(state->macro_editor, selected);
  gtk_entry_set_text(GTK_ENTRY(state->macro_id_entry),
                     selected ? rule->id.c_str() : "");
  gtk_entry_set_text(GTK_ENTRY(state->macro_regex_entry),
                     selected ? rule->pattern.c_str() : "");

  const bool sends = selected &&
                     std::holds_alternative<MacroSendAction>(rule->action);
  gtk_combo_box_set_active_id(GTK_COMBO_BOX(state->macro_action_combo),
                              sends ? "send" : "command");
  const std::string send_text =
      sends ? std::get<MacroSendAction>(rule->action).text : std::string();
  gtk_text_buffer_set_text(
      gtk_text_view_get_buffer(GTK_TEXT_VIEW(state->macro_send_view)),
      send_text.c_str(), static_cast<gint>(send_text.size()));
  const auto *command =
      selected ? std::get_if<MacroCommandAction>(&rule->action) : nullptr;
  gtk_entry_set_text(GTK_ENTRY(state->macro_command_entry),
                     command == nullptr ? "" : command->command.c_str());
  rebuild_macro_arguments(state);
  gtk_widget_set_visible(state->macro_send_panel, selected && sends);
  gtk_widget_set_visible(state->macro_command_panel,
                         selected && !sends);
  state->synchronizing = previous_synchronizing;
  update_macro_rule_button_sensitivity(state);
  update_macro_validation(state);
}

static void update_next_macro_number(SettingsWidgetState *state) {
  for (const MacroRule &rule : state->draft_store.macro_rules) {
    constexpr char prefix[] = "rule";
    if (!rule.id.starts_with(prefix) || rule.id.size() == sizeof(prefix) - 1) {
      continue;
    }
    const std::string suffix = rule.id.substr(sizeof(prefix) - 1);
    if (!std::all_of(suffix.begin(), suffix.end(), [](unsigned char value) {
          return std::isdigit(value) != 0;
        })) {
      continue;
    }
    const guint64 number = g_ascii_strtoull(suffix.c_str(), nullptr, 10);
    if (number < std::numeric_limits<unsigned int>::max()) {
      state->next_macro_number =
          std::max(state->next_macro_number,
                   static_cast<unsigned int>(number) + 1);
    }
  }
}

static void rebuild_macro_list(SettingsWidgetState *state) {
  if (state->macro_list == nullptr) {
    return;
  }
  const bool previous_synchronizing = state->synchronizing;
  state->synchronizing = true;
  clear_container(state->macro_list);
  if (state->draft_store.macro_rules.empty()) {
    state->selected_macro = -1;
  } else if (state->selected_macro < 0 ||
             static_cast<std::size_t>(state->selected_macro) >=
                 state->draft_store.macro_rules.size()) {
    state->selected_macro = 0;
  }

  GtkListBoxRow *selected_row = nullptr;
  for (std::size_t index = 0; index < state->draft_store.macro_rules.size();
       ++index) {
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *label =
        gtk_label_new(state->draft_store.macro_rules[index].id.c_str());
    gtk_label_set_xalign(GTK_LABEL(label), 0.0F);
    gtk_container_add(GTK_CONTAINER(row), label);
    g_object_set_data(G_OBJECT(row), "elder-terms-macro-index",
                      GINT_TO_POINTER(static_cast<int>(index) + 1));
    gtk_container_add(GTK_CONTAINER(state->macro_list), row);
    if (static_cast<int>(index) == state->selected_macro) {
      selected_row = GTK_LIST_BOX_ROW(row);
    }
  }
  gtk_widget_show_all(state->macro_list);
  if (selected_row != nullptr) {
    gtk_list_box_select_row(GTK_LIST_BOX(state->macro_list), selected_row);
  }
  update_next_macro_number(state);
  state->synchronizing = previous_synchronizing;
  sync_macro_editor(state);
}

static void mark_macro_rules_changed(SettingsWidgetState *state) {
  state->draft_store.macro_rules_dirty = true;
  update_macro_validation(state);
  notify_changed(state);
}

static void on_macro_list_row_selected(GtkListBox *, GtkListBoxRow *row,
                                       gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (state->synchronizing) {
    return;
  }
  state->selected_macro =
      row == nullptr ? -1 : macro_argument_index(GTK_WIDGET(row));
  sync_macro_editor(state);
}

static void on_macro_id_changed(GtkEditable *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  MacroRule *rule = selected_macro_rule(state);
  if (state->synchronizing || rule == nullptr) {
    return;
  }
  const char *text = gtk_entry_get_text(GTK_ENTRY(state->macro_id_entry));
  rule->id = text == nullptr ? "" : text;
  state->draft_store.macro_rules_dirty = true;
  rebuild_macro_list(state);
  notify_changed(state);
}

static void on_macro_regex_changed(GtkEditable *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  MacroRule *rule = selected_macro_rule(state);
  if (state->synchronizing || rule == nullptr) {
    return;
  }
  const char *text = gtk_entry_get_text(GTK_ENTRY(state->macro_regex_entry));
  rule->pattern = text == nullptr ? "" : text;
  mark_macro_rules_changed(state);
}

static void on_macro_action_changed(GtkComboBox *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  MacroRule *rule = selected_macro_rule(state);
  if (state->synchronizing || rule == nullptr) {
    return;
  }
  if (active_combo_id(state->macro_action_combo, "send") == "command") {
    if (!std::holds_alternative<MacroCommandAction>(rule->action)) {
      rule->action = MacroCommandAction{};
    }
  } else if (!std::holds_alternative<MacroSendAction>(rule->action)) {
    rule->action = MacroSendAction{};
  }
  state->draft_store.macro_rules_dirty = true;
  sync_macro_editor(state);
  notify_changed(state);
}

static void on_macro_send_changed(GtkTextBuffer *buffer, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  MacroRule *rule = selected_macro_rule(state);
  if (state->synchronizing || rule == nullptr) {
    return;
  }
  auto *send = std::get_if<MacroSendAction>(&rule->action);
  if (send == nullptr) {
    return;
  }
  GtkTextIter start;
  GtkTextIter end;
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  gchar *text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
  send->text = text == nullptr ? "" : text;
  g_free(text);
  mark_macro_rules_changed(state);
}

static void on_macro_command_changed(GtkEditable *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  MacroRule *rule = selected_macro_rule(state);
  if (state->synchronizing || rule == nullptr) {
    return;
  }
  auto *command = std::get_if<MacroCommandAction>(&rule->action);
  if (command == nullptr) {
    return;
  }
  const char *text =
      gtk_entry_get_text(GTK_ENTRY(state->macro_command_entry));
  command->command = text == nullptr ? "" : text;
  mark_macro_rules_changed(state);
}

static void on_macro_add_clicked(GtkButton *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  std::string id;
  do {
    id = "rule" + std::to_string(state->next_macro_number++);
  } while (std::any_of(
      state->draft_store.macro_rules.begin(),
      state->draft_store.macro_rules.end(),
      [&id](const MacroRule &rule) { return rule.id == id; }));
  state->draft_store.macro_rules.push_back(MacroRule{
      .id = std::move(id),
      .pattern = {},
      .action = MacroSendAction{},
  });
  state->draft_store.macro_rules_dirty = true;
  state->selected_macro =
      static_cast<int>(state->draft_store.macro_rules.size()) - 1;
  rebuild_macro_list(state);
  notify_changed(state);
}

static void on_macro_remove_clicked(GtkButton *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (selected_macro_rule(state) == nullptr) {
    return;
  }
  state->draft_store.macro_rules.erase(
      state->draft_store.macro_rules.begin() + state->selected_macro);
  if (static_cast<std::size_t>(state->selected_macro) >=
      state->draft_store.macro_rules.size()) {
    --state->selected_macro;
  }
  state->draft_store.macro_rules_dirty = true;
  rebuild_macro_list(state);
  notify_changed(state);
}

static void move_selected_macro(SettingsWidgetState *state, int offset) {
  const int destination = state->selected_macro + offset;
  if (selected_macro_rule(state) == nullptr || destination < 0 ||
      static_cast<std::size_t>(destination) >=
          state->draft_store.macro_rules.size()) {
    return;
  }
  std::swap(state->draft_store.macro_rules[state->selected_macro],
            state->draft_store.macro_rules[destination]);
  state->selected_macro = destination;
  state->draft_store.macro_rules_dirty = true;
  rebuild_macro_list(state);
  notify_changed(state);
}

static void on_macro_move_up_clicked(GtkButton *, gpointer data) {
  move_selected_macro(static_cast<SettingsWidgetState *>(data), -1);
}

static void on_macro_move_down_clicked(GtkButton *, gpointer data) {
  move_selected_macro(static_cast<SettingsWidgetState *>(data), 1);
}

static void on_macro_argument_add_clicked(GtkButton *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  MacroRule *rule = selected_macro_rule(state);
  auto *command =
      rule == nullptr ? nullptr
                      : std::get_if<MacroCommandAction>(&rule->action);
  if (command == nullptr) {
    return;
  }
  command->arguments.emplace_back();
  state->draft_store.macro_rules_dirty = true;
  rebuild_macro_arguments(state);
  update_macro_validation(state);
  notify_changed(state);
}

static void on_macro_argument_changed(GtkEditable *editable, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  MacroRule *rule = selected_macro_rule(state);
  auto *command =
      rule == nullptr ? nullptr
                      : std::get_if<MacroCommandAction>(&rule->action);
  const int index = macro_argument_index(GTK_WIDGET(editable));
  if (state->synchronizing || command == nullptr || index < 0 ||
      static_cast<std::size_t>(index) >= command->arguments.size()) {
    return;
  }
  const char *text = gtk_entry_get_text(GTK_ENTRY(editable));
  command->arguments[index] = text == nullptr ? "" : text;
  mark_macro_rules_changed(state);
}

static void move_macro_argument(SettingsWidgetState *state, GtkWidget *widget,
                                int offset) {
  MacroRule *rule = selected_macro_rule(state);
  auto *command =
      rule == nullptr ? nullptr
                      : std::get_if<MacroCommandAction>(&rule->action);
  const int index = macro_argument_index(widget);
  const int destination = index + offset;
  if (command == nullptr || index < 0 || destination < 0 ||
      static_cast<std::size_t>(index) >= command->arguments.size() ||
      static_cast<std::size_t>(destination) >= command->arguments.size()) {
    return;
  }
  std::swap(command->arguments[index], command->arguments[destination]);
  state->draft_store.macro_rules_dirty = true;
  rebuild_macro_arguments(state);
  update_macro_validation(state);
  notify_changed(state);
}

static void on_macro_argument_move_up_clicked(GtkButton *button,
                                              gpointer data) {
  move_macro_argument(static_cast<SettingsWidgetState *>(data),
                      GTK_WIDGET(button), -1);
}

static void on_macro_argument_move_down_clicked(GtkButton *button,
                                                gpointer data) {
  move_macro_argument(static_cast<SettingsWidgetState *>(data),
                      GTK_WIDGET(button), 1);
}

static void on_macro_argument_remove_clicked(GtkButton *button,
                                             gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  MacroRule *rule = selected_macro_rule(state);
  auto *command =
      rule == nullptr ? nullptr
                      : std::get_if<MacroCommandAction>(&rule->action);
  const int index = macro_argument_index(GTK_WIDGET(button));
  if (command == nullptr || index < 0 ||
      static_cast<std::size_t>(index) >= command->arguments.size()) {
    return;
  }
  command->arguments.erase(command->arguments.begin() + index);
  state->draft_store.macro_rules_dirty = true;
  rebuild_macro_arguments(state);
  update_macro_validation(state);
  notify_changed(state);
}

struct ComboOption {
  const char *id;
  std::string label;
};

static void populate_inheritable_combo(
    GtkWidget *combo, const SettingsStore &store, const SettingKey &key,
    const std::string &fallback_display,
    const std::vector<ComboOption> &options, const std::string &effective_id) {
  gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(combo));
  const std::string inherited =
      setting_fallback_label(store, key, fallback_display);
  append_combo_option(combo, inherit_choice, inherited.c_str());
  for (const ComboOption &option : options) {
    append_combo_option(combo, option.id, option.label.c_str());
  }
  gtk_combo_box_set_active_id(
      GTK_COMBO_BOX(combo),
      setting_has_explicit_value(store, key) ? effective_id.c_str()
                                             : inherit_choice);
}

static void populate_boolean_combo(GtkWidget *combo,
                                   const SettingsStore &store,
                                   const SettingKey &key,
                                   bool effective_value) {
  const bool fallback =
      std::get<bool>(setting_fallback_value(store, key, SettingValue{false}));
  populate_inheritable_combo(
      combo, store, key,
      fallback ? settings_ui_text(SettingsUiText::enabled)
               : settings_ui_text(SettingsUiText::disabled),
      {
          {.id = boolean_enabled,
           .label = settings_ui_text(SettingsUiText::enabled)},
          {.id = boolean_disabled,
           .label = settings_ui_text(SettingsUiText::disabled)},
      },
      effective_value ? boolean_enabled : boolean_disabled);
}

static std::string general_color_label(const std::string &value) {
  return value == general_color_none
             ? settings_ui_text(SettingsUiText::no_color)
             : settings_ui_text(SettingsUiText::custom_color);
}

static void set_color_button_rgb(
    GtkWidget *button, const std::optional<RgbColor> &color) {
  const RgbColor effective =
      color.value_or(RgbColor{.red = 0, .green = 0, .blue = 0});
  const GdkRGBA rgba{
      .red = static_cast<gdouble>(effective.red) / 255.0,
      .green = static_cast<gdouble>(effective.green) / 255.0,
      .blue = static_cast<gdouble>(effective.blue) / 255.0,
      .alpha = 1.0,
  };
  gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(button), &rgba);
}

static void sync_general_color_control(
    SettingsWidgetState *state, GeneralColorField field,
    const std::optional<RgbColor> &effective_color) {
  GtkWidget *combo = general_color_mode_combo(state, field);
  GtkWidget *button = general_color_button(state, field);
  if (combo == nullptr || button == nullptr) {
    return;
  }

  const SettingKey key = general_color_setting_key(field);
  const std::string effective = setting_string_value_or_default(
      state->draft_store, key, general_color_none);
  const std::string fallback = std::get<std::string>(
      setting_fallback_value(state->draft_store, key,
                             SettingValue{std::string(general_color_none)}));
  populate_inheritable_combo(
      combo, state->draft_store, key, general_color_label(fallback),
      {
          {.id = general_color_none,
           .label = settings_ui_text(SettingsUiText::no_color)},
          {.id = general_color_custom,
           .label = settings_ui_text(SettingsUiText::custom_color)},
      },
      effective == general_color_none ? general_color_none
                                      : general_color_custom);
  set_color_button_rgb(button, effective_color);
  gtk_widget_set_sensitive(button, TRUE);
}

static std::string connection_type_label(const std::string &type) {
  return setting_choice_label(general_type_setting_key(), type);
}

static void sync_general_type_combo(SettingsWidgetState *state) {
  const std::string fallback = std::get<std::string>(setting_fallback_value(
      state->draft_store, general_type_setting_key(),
      SettingValue{std::string(local_connection_type)}));
  const std::string effective = connection_type_value(state->draft_store);
  populate_inheritable_combo(
      state->general_type_combo, state->draft_store,
      general_type_setting_key(), connection_type_label(fallback),
      {
          {.id = local_connection_type,
           .label = connection_type_label(local_connection_type)},
          {.id = telnet_connection_type,
           .label = connection_type_label(telnet_connection_type)},
          {.id = serial_connection_type,
           .label = connection_type_label(serial_connection_type)},
          {.id = ssh_connection_type,
           .label = connection_type_label(ssh_connection_type)},
          {.id = sftp_connection_type,
           .label = connection_type_label(sftp_connection_type)},
          {.id = ftp_connection_type,
           .label = connection_type_label(ftp_connection_type)},
      },
      effective);
}

static bool zmodem_fallback_value(const SettingsStore &store) {
  SettingsStore fallback_store = store;
  clear_explicit_setting_value(&fallback_store,
                               transfer_zmodem_autostart_setting_key());
  return transfer_zmodem_autostart(fallback_store);
}

static void sync_zmodem_combo(SettingsWidgetState *state) {
  const bool fallback = zmodem_fallback_value(state->draft_store);
  const bool effective = transfer_zmodem_autostart(state->draft_store);
  populate_inheritable_combo(
      state->transfer_zmodem_autostart_combo, state->draft_store,
      transfer_zmodem_autostart_setting_key(),
      fallback ? settings_ui_text(SettingsUiText::enabled)
               : settings_ui_text(SettingsUiText::disabled),
      {
          {.id = zmodem_autostart_enabled,
           .label = settings_ui_text(SettingsUiText::enabled)},
          {.id = zmodem_autostart_disabled,
           .label = settings_ui_text(SettingsUiText::disabled)},
      },
      effective ? zmodem_autostart_enabled : zmodem_autostart_disabled);
}

static std::optional<SerialDeviceChoice> serial_device_choice_for_target(
    const std::vector<SerialDeviceChoice> &choices,
    const std::string &target) {
  const auto selected = std::find_if(
      choices.begin(), choices.end(),
      [&target](const SerialDeviceChoice &choice) {
        return choice.target_path == target;
      });
  return selected == choices.end()
             ? std::nullopt
             : std::optional<SerialDeviceChoice>(*selected);
}

static std::optional<SerialDeviceChoice> serial_device_choice_for_usb_serial(
    const std::vector<SerialDeviceChoice> &choices,
    const std::optional<std::string> &usb_serial) {
  if (!usb_serial.has_value() || usb_serial->empty()) {
    return std::nullopt;
  }
  std::optional<SerialDeviceChoice> selected;
  for (const SerialDeviceChoice &choice : choices) {
    if (choice.usb_serial != usb_serial) {
      continue;
    }
    if (selected.has_value()) {
      return std::nullopt;
    }
    selected = choice;
  }
  return selected;
}

static std::string serial_device_choice_label(
    const std::vector<SerialDeviceChoice> &choices,
    const std::string &target) {
  const std::optional<SerialDeviceChoice> choice =
      serial_device_choice_for_target(choices, target);
  return choice.has_value() ? choice->display_label : target;
}

static void populate_serial_device_match_mode_combo(
    SettingsWidgetState *state,
    const SerialConnectionSettings &serial) {
  const SettingKey key = serial_device_match_mode_setting_key();
  const std::string fallback = std::get<std::string>(setting_fallback_value(
      state->draft_store, key,
      SettingValue{std::string(serial_device_stable_id_mode)}));
  populate_inheritable_combo(
      state->serial_device_match_mode_combo, state->draft_store, key,
      setting_choice_label(key, fallback),
      {
          {.id = serial_device_path_mode,
           .label = setting_choice_label(key, serial_device_path_mode)},
          {.id = serial_device_stable_id_mode,
           .label = setting_choice_label(key, serial_device_stable_id_mode)},
          {.id = serial_device_physical_port_mode,
           .label =
               setting_choice_label(key, serial_device_physical_port_mode)},
      },
      serial_device_match_mode_to_string(serial.device_match_mode));
}

static void populate_serial_device_combo(
    SettingsWidgetState *state,
    const SerialConnectionSettings &serial) {
  state->serial_device_choices =
      list_serial_device_choices(serial.device_match_mode);
  gtk_combo_box_text_remove_all(
      GTK_COMBO_BOX_TEXT(state->serial_device_combo));

  const SettingKey key = serial_device_setting_key();
  const std::string fallback = std::get<std::string>(setting_fallback_value(
      state->draft_store, key, SettingValue{std::string()}));
  const std::string fallback_display =
      fallback.empty()
          ? settings_ui_text(SettingsUiText::serial_no_device)
          : serial_device_choice_label(state->serial_device_choices, fallback);
  const std::string inherited =
      setting_fallback_label(state->draft_store, key, fallback_display);
  append_combo_option(state->serial_device_combo, inherit_choice,
                      inherited.c_str());

  const bool explicit_device =
      setting_has_explicit_value(state->draft_store, key);
  if (!fallback.empty() || (explicit_device && serial.device.empty())) {
    append_combo_option(state->serial_device_combo,
                        serial_device_no_device_choice,
                        settings_ui_text(SettingsUiText::serial_no_device));
  }
  for (const SerialDeviceChoice &choice : state->serial_device_choices) {
    append_combo_option(state->serial_device_combo,
                        choice.target_path.c_str(),
                        choice.display_label.c_str());
  }

  if (!explicit_device) {
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(state->serial_device_combo),
                                inherit_choice);
    return;
  }
  if (serial.device.empty()) {
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(state->serial_device_combo),
                                serial_device_no_device_choice);
    return;
  }
  if (!serial_device_choice_for_target(state->serial_device_choices,
                                       serial.device)
           .has_value()) {
    append_combo_option(state->serial_device_combo, serial.device.c_str(),
                        serial.device.c_str());
  }
  gtk_combo_box_set_active_id(GTK_COMBO_BOX(state->serial_device_combo),
                              serial.device.c_str());
}

static void set_serial_metadata_label(GtkWidget *label,
                                      const std::optional<std::string> &value) {
  const char *text =
      value.has_value() && !value->empty()
          ? value->c_str()
          : settings_ui_text(SettingsUiText::unavailable);
  gtk_label_set_text(GTK_LABEL(label), text);
  gtk_widget_set_tooltip_text(label, text);
}

static void sync_serial_device_metadata(
    SettingsWidgetState *state,
    const SerialConnectionSettings &serial) {
  const SerialDevicePaths paths = host_serial_device_paths();
  std::optional<SerialDeviceChoice> selected =
      serial_device_choice_for_target(state->serial_device_choices,
                                      serial.device);
  if (!selected.has_value()) {
    selected = serial_device_choice_for_usb_serial(
        state->serial_device_choices, serial.device_usb_serial);
  }

  std::optional<std::string> current_node =
      selected.has_value()
          ? selected->current_node
          : resolve_serial_device_current_node(serial.device);
  std::optional<std::string> usb_serial =
      selected.has_value() && selected->usb_serial.has_value()
          ? selected->usb_serial
          : serial.device_usb_serial;
  std::optional<std::string> stable_id;
  if (selected.has_value()) {
    stable_id = resolve_serial_device_target_for_mode(
        SerialDeviceMatchMode::stable_id, selected->target_path, paths);
  } else {
    stable_id = resolve_serial_device_target_for_mode(
        SerialDeviceMatchMode::stable_id, serial.device, paths);
  }
  if ((!stable_id.has_value() || stable_id->empty()) &&
      usb_serial.has_value()) {
    const std::vector<SerialDeviceChoice> stable_choices =
        list_serial_device_choices(SerialDeviceMatchMode::stable_id, paths);
    const std::optional<SerialDeviceChoice> stable_choice =
        serial_device_choice_for_usb_serial(stable_choices, usb_serial);
    if (stable_choice.has_value()) {
      stable_id = stable_choice->target_path;
      if (!current_node.has_value()) {
        current_node = stable_choice->current_node;
      }
    }
  }

  set_serial_metadata_label(state->serial_stable_id_value, stable_id);
  set_serial_metadata_label(state->serial_usb_serial_value, usb_serial);
  set_serial_metadata_label(state->serial_current_node_value, current_node);
}

static void sync_serial_device_widgets(SettingsWidgetState *state) {
  if (state->serial_device_match_mode_combo == nullptr ||
      state->serial_device_combo == nullptr) {
    return;
  }
  const bool previous_synchronizing = state->synchronizing;
  state->synchronizing = true;
  const SerialConnectionSettings serial =
      serial_connection_settings(state->draft_store);
  populate_serial_device_match_mode_combo(state, serial);
  populate_serial_device_combo(state, serial);
  sync_serial_device_metadata(state, serial);
  state->synchronizing = previous_synchronizing;
}

static bool serial_device_combo_popup_is_shown(
    const SettingsWidgetState *state) {
  gboolean shown = FALSE;
  g_object_get(state->serial_device_combo, "popup-shown", &shown, nullptr);
  return shown != FALSE;
}

static void refresh_serial_device_widgets(SettingsWidgetState *state) {
  if (state == nullptr || state->serial_device_combo == nullptr) {
    return;
  }
  if (serial_device_combo_popup_is_shown(state)) {
    state->serial_device_refresh_pending = true;
    return;
  }
  state->serial_device_refresh_pending = false;
  sync_serial_device_widgets(state);
}

static void sync_key_binding_widget(
    SettingsWidgetState *state, KeyBindingInputWidgetState *input,
    const SettingKey &key, const std::string &effective_value) {
  const bool explicit_value =
      setting_has_explicit_value(state->draft_store, key);
  const std::string text = explicit_value ? effective_value : std::string();
  set_key_binding_input_widget_text(input, text);
  set_key_binding_input_widget_empty_clear_enabled(
      input, !explicit_value && !effective_value.empty());
  GtkWidget *entry = key_binding_input_widget_root(input);
  if (explicit_value && effective_value.empty()) {
    gtk_entry_set_placeholder_text(
        GTK_ENTRY(entry), settings_ui_text(SettingsUiText::disabled));
    return;
  }
  if (explicit_value) {
    gtk_entry_set_placeholder_text(
        GTK_ENTRY(entry),
        settings_ui_text(SettingsUiText::press_key_combination));
    return;
  }
  const std::string placeholder =
      setting_fallback_label(
          state->draft_store, key,
          effective_value.empty()
              ? settings_ui_text(SettingsUiText::disabled)
              : effective_value);
  gtk_entry_set_placeholder_text(GTK_ENTRY(entry), placeholder.c_str());
}

static void sync_key_binding_reset_button(
    SettingsWidgetState *state, GtkWidget *button, const SettingKey &key) {
  gtk_widget_set_sensitive(
      button, setting_has_explicit_value(state->draft_store, key));
  const char *tooltip =
      setting_fallback_source(state->draft_store, key) ==
              SettingValueSource::global
          ? settings_ui_text(SettingsUiText::use_global_default)
          : settings_ui_text(SettingsUiText::use_built_in_default);
  gtk_widget_set_tooltip_text(button, tooltip);
}

static void sync_terminal_type_entries(SettingsWidgetState *state) {
  const bool previous_synchronizing = state->synchronizing;
  state->synchronizing = true;
  if (state->telnet_terminal_type_entry != nullptr) {
    sync_inheritable_entry(
        state->telnet_terminal_type_entry, state->draft_store,
        telnet_terminal_type_setting_key(),
        telnet_connection_settings(state->draft_store).terminal_type);
  }
  if (state->ssh_terminal_type_entry != nullptr) {
    sync_inheritable_entry(
        state->ssh_terminal_type_entry, state->draft_store,
        ssh_terminal_type_setting_key(),
        ssh_connection_settings(state->draft_store).terminal_type);
  }
  state->synchronizing = previous_synchronizing;
}

static void sync_widgets_from_draft(SettingsWidgetState *state) {
  const TerminalDisplaySettings display =
      terminal_display_settings(state->draft_store);
  const GeneralColorSettings colors =
      general_color_settings(state->draft_store);
  const TelnetConnectionSettings telnet =
      telnet_connection_settings(state->draft_store);
  const SshConnectionSettings ssh =
      ssh_connection_settings(state->draft_store);
  const SftpConnectionSettings sftp =
      sftp_connection_settings(state->draft_store);
  const FtpConnectionSettings ftp =
      ftp_connection_settings(state->draft_store);
  const SerialConnectionSettings serial =
      serial_connection_settings(state->draft_store);
  const TerminalLogSettings log = terminal_log_settings(state->draft_store);

  if (state->general_name_entry != nullptr) {
    gtk_entry_set_text(GTK_ENTRY(state->general_name_entry),
                       general_connection_name(state->draft_store).c_str());
  }
  if (state->general_type_combo != nullptr) {
    sync_general_type_combo(state);
  }
  if (state->general_open_connection_input != nullptr) {
    sync_key_binding_widget(
        state, state->general_open_connection_input,
        general_open_connection_hotkey_setting_key(),
        general_open_connection_hotkey_text(state->draft_store));
    sync_key_binding_reset_button(
        state, state->general_open_connection_reset_button,
        general_open_connection_hotkey_setting_key());
    set_key_binding_input_widget_external_error(
        state->general_open_connection_input, {});
  }
  sync_terminal_text_widgets(state);
  if (state->terminal_width_entry != nullptr) {
    sync_inheritable_entry(state->terminal_width_entry, state->draft_store,
                           terminal_width_setting_key(),
                           std::to_string(display.width));
    state->terminal_width_valid = true;
    set_entry_validation(state->terminal_width_entry, true, {});
  }
  if (state->terminal_height_entry != nullptr) {
    sync_inheritable_entry(state->terminal_height_entry, state->draft_store,
                           terminal_height_setting_key(),
                           std::to_string(display.height));
    state->terminal_height_valid = true;
    set_entry_validation(state->terminal_height_entry, true, {});
  }
  if (state->terminal_scrollback_lines_entry != nullptr) {
    sync_inheritable_entry(
        state->terminal_scrollback_lines_entry, state->draft_store,
        terminal_scrollback_lines_setting_key(),
        std::to_string(display.scrollback_lines));
    state->terminal_scrollback_lines_valid = true;
    set_entry_validation(state->terminal_scrollback_lines_entry, true, {});
  }
  if (state->terminal_zoom_entry != nullptr) {
    sync_inheritable_entry(state->terminal_zoom_entry, state->draft_store,
                           terminal_zoom_setting_key(),
                           format_double(display.zoom));
    state->terminal_zoom_valid = true;
    set_entry_validation(state->terminal_zoom_entry, true, {});
  }
  if (state->terminal_font_primary_mode_combo != nullptr &&
      state->terminal_font_fallback_mode_combo != nullptr &&
      state->terminal_font_primary_button != nullptr &&
      state->terminal_font_fallback_button != nullptr) {
    sync_terminal_font_controls(state);
  }
  if (state->terminal_auto_close_combo != nullptr) {
    populate_boolean_combo(state->terminal_auto_close_combo,
                           state->draft_store,
                           terminal_auto_close_setting_key(),
                           terminal_auto_close(state->draft_store));
  }
  if (state->terminal_show_border_combo != nullptr) {
    populate_boolean_combo(state->terminal_show_border_combo,
                           state->draft_store,
                           terminal_show_border_setting_key(),
                           terminal_show_border(state->draft_store));
  }
  if (state->terminal_border_width_entry != nullptr) {
    sync_inheritable_entry(
        state->terminal_border_width_entry, state->draft_store,
        terminal_border_width_setting_key(),
        std::to_string(terminal_border_width(state->draft_store)));
    state->terminal_border_width_valid = true;
    set_entry_validation(state->terminal_border_width_entry, true, {});
  }
  if (state->terminal_bell_sound_entry != nullptr) {
    sync_inheritable_entry(
        state->terminal_bell_sound_entry, state->draft_store,
        terminal_bell_sound_setting_key(),
        setting_string_value_or_default(
            state->draft_store, terminal_bell_sound_setting_key(),
            "default"));
    state->terminal_bell_sound_valid = true;
    set_entry_validation(state->terminal_bell_sound_entry, true, {});
  }
  sync_general_color_control(
      state, GeneralColorField::exterior_background,
      colors.exterior_background);
  sync_general_color_control(state, GeneralColorField::background,
                             colors.background);
  if (state->local_command_line_entry != nullptr) {
    sync_inheritable_entry(
        state->local_command_line_entry, state->draft_store,
        local_command_line_setting_key(),
        setting_string_value_or_default(
            state->draft_store, local_command_line_setting_key(),
            std::string()));
    state->local_command_line_valid = true;
    set_entry_validation(state->local_command_line_entry, true, {});
  }
  sync_terminal_type_entries(state);
  if (state->terminal_zoom_in_key_input != nullptr) {
    sync_key_binding_widget(
        state, state->terminal_zoom_in_key_input,
        terminal_zoom_in_key_setting_key(),
        terminal_zoom_in_key(state->draft_store));
    sync_key_binding_reset_button(
        state, state->terminal_zoom_in_key_reset_button,
        terminal_zoom_in_key_setting_key());
  }
  if (state->terminal_zoom_out_key_input != nullptr) {
    sync_key_binding_widget(
        state, state->terminal_zoom_out_key_input,
        terminal_zoom_out_key_setting_key(),
        terminal_zoom_out_key(state->draft_store));
    sync_key_binding_reset_button(
        state, state->terminal_zoom_out_key_reset_button,
        terminal_zoom_out_key_setting_key());
  }
  if (state->terminal_send_break_key_input != nullptr) {
    sync_key_binding_widget(
        state, state->terminal_send_break_key_input,
        terminal_send_break_key_setting_key(),
        terminal_send_break_key(state->draft_store));
    sync_key_binding_reset_button(
        state, state->terminal_send_break_key_reset_button,
        terminal_send_break_key_setting_key());
  }
  if (state->telnet_address_entry != nullptr) {
    sync_inheritable_entry(state->telnet_address_entry, state->draft_store,
                           telnet_address_setting_key(), telnet.address);
  }
  if (state->telnet_port_entry != nullptr) {
    sync_inheritable_entry(state->telnet_port_entry, state->draft_store,
                           telnet_port_setting_key(),
                           std::to_string(telnet.port));
    state->telnet_port_valid = true;
    set_entry_validation(state->telnet_port_entry, true, {});
  }
  if (state->ssh_address_entry != nullptr) {
    sync_inheritable_entry(state->ssh_address_entry, state->draft_store,
                           ssh_address_setting_key(), ssh.endpoint.address);
  }
  if (state->ssh_port_entry != nullptr) {
    sync_inheritable_entry(state->ssh_port_entry, state->draft_store,
                           ssh_port_setting_key(),
                           std::to_string(ssh.endpoint.port));
    state->ssh_port_valid = true;
    set_entry_validation(state->ssh_port_entry, true, {});
  }
  if (state->ssh_username_entry != nullptr) {
    sync_inheritable_entry(state->ssh_username_entry, state->draft_store,
                           ssh_username_setting_key(),
                           ssh.endpoint.username);
  }
  if (state->ssh_identity_file_entry != nullptr) {
    sync_inheritable_entry(state->ssh_identity_file_entry,
                           state->draft_store,
                           ssh_identity_file_setting_key(),
                           ssh.endpoint.identity_file);
  }
  if (state->sftp_local_directory_entry != nullptr) {
    sync_inheritable_entry(state->sftp_local_directory_entry,
                           state->draft_store,
                           sftp_local_directory_setting_key(),
                           sftp.local_directory);
  }
  if (state->sftp_remote_directory_entry != nullptr) {
    sync_inheritable_entry(state->sftp_remote_directory_entry,
                           state->draft_store,
                           sftp_remote_directory_setting_key(),
                           sftp.remote_directory);
  }
  if (state->ftp_address_entry != nullptr) {
    sync_inheritable_entry(state->ftp_address_entry, state->draft_store,
                           ftp_address_setting_key(), ftp.address);
  }
  if (state->ftp_port_entry != nullptr) {
    sync_inheritable_entry(state->ftp_port_entry, state->draft_store,
                           ftp_port_setting_key(),
                           std::to_string(ftp.port));
    state->ftp_port_valid = true;
    set_entry_validation(state->ftp_port_entry, true, {});
  }
  if (state->ftp_username_entry != nullptr) {
    sync_inheritable_entry(state->ftp_username_entry, state->draft_store,
                           ftp_username_setting_key(), ftp.username);
  }
  if (state->ftp_data_connection_mode_combo != nullptr) {
    const SettingKey key = ftp_data_connection_mode_setting_key();
    const std::string effective =
        ftp_data_connection_mode_to_string(ftp.data_connection_mode);
    const std::string fallback = std::get<std::string>(
        setting_fallback_value(state->draft_store, key,
                               SettingValue{std::string("passive")}));
    populate_inheritable_combo(
        state->ftp_data_connection_mode_combo, state->draft_store, key,
        setting_choice_label(key, fallback),
        {
            {.id = "passive",
             .label = setting_choice_label(key, "passive")},
            {.id = "active", .label = setting_choice_label(key, "active")},
        },
        effective);
  }
  if (state->ftp_local_directory_entry != nullptr) {
    sync_inheritable_entry(state->ftp_local_directory_entry,
                           state->draft_store,
                           ftp_local_directory_setting_key(),
                           ftp.local_directory);
  }
  if (state->ftp_remote_directory_entry != nullptr) {
    sync_inheritable_entry(state->ftp_remote_directory_entry,
                           state->draft_store,
                           ftp_remote_directory_setting_key(),
                           ftp.remote_directory);
  }
  sync_serial_device_widgets(state);
  if (state->serial_baudrate_entry != nullptr) {
    sync_inheritable_entry(state->serial_baudrate_entry,
                           state->draft_store,
                           serial_baudrate_setting_key(),
                           std::to_string(serial.baudrate));
    state->serial_baudrate_valid = true;
    set_entry_validation(state->serial_baudrate_entry, true, {});
  }
  if (state->serial_bits_combo != nullptr) {
    const std::string bits = std::to_string(serial.bits);
    const std::string fallback = format_setting_value(setting_fallback_value(
        state->draft_store, serial_bits_setting_key(),
        SettingValue{static_cast<gint64>(8)}));
    populate_inheritable_combo(
        state->serial_bits_combo, state->draft_store,
        serial_bits_setting_key(), fallback,
        {
            {.id = "5", .label = "5"},
            {.id = "6", .label = "6"},
            {.id = "7", .label = "7"},
            {.id = "8", .label = "8"},
        },
        bits);
  }
  if (state->serial_parity_combo != nullptr) {
    const std::string parity = serial_parity_to_string(serial.parity);
    const SettingKey key = serial_parity_setting_key();
    const std::string fallback = setting_choice_label(
        key, format_setting_value(setting_fallback_value(
                 state->draft_store, key,
                 SettingValue{std::string("n")})));
    populate_inheritable_combo(
        state->serial_parity_combo, state->draft_store, key, fallback,
        {
            {.id = "n", .label = setting_choice_label(key, "n")},
            {.id = "e", .label = setting_choice_label(key, "e")},
            {.id = "o", .label = setting_choice_label(key, "o")},
        },
        parity);
  }
  if (state->serial_stop_bit_combo != nullptr) {
    const std::string stop_bit = std::to_string(serial.stop_bit);
    const std::string fallback = format_setting_value(setting_fallback_value(
        state->draft_store, serial_stop_bit_setting_key(),
        SettingValue{static_cast<gint64>(1)}));
    populate_inheritable_combo(
        state->serial_stop_bit_combo, state->draft_store,
        serial_stop_bit_setting_key(), fallback,
        {
            {.id = "1", .label = "1"},
            {.id = "2", .label = "2"},
        },
        stop_bit);
  }
  if (state->serial_flow_control_combo != nullptr) {
    const std::string flow_control =
        serial_flow_control_to_string(serial.flow_control);
    const SettingKey key = serial_flow_control_setting_key();
    const std::string fallback = setting_choice_label(
        key, format_setting_value(setting_fallback_value(
                 state->draft_store, key,
                 SettingValue{std::string("none")})));
    populate_inheritable_combo(
        state->serial_flow_control_combo, state->draft_store, key, fallback,
        {
            {.id = "none", .label = setting_choice_label(key, "none")},
            {.id = "xon", .label = setting_choice_label(key, "xon")},
            {.id = "hard", .label = setting_choice_label(key, "hard")},
        },
        flow_control);
  }
  if (state->serial_carrier_detect_combo != nullptr) {
    const std::string carrier_detect =
        serial_carrier_detect_to_string(serial.carrier_detect);
    const SettingKey key = serial_carrier_detect_setting_key();
    const std::string fallback = setting_choice_label(
        key, format_setting_value(setting_fallback_value(
                 state->draft_store, key,
                 SettingValue{std::string("cd")})));
    populate_inheritable_combo(
        state->serial_carrier_detect_combo, state->draft_store, key, fallback,
        {
            {.id = "cd", .label = setting_choice_label(key, "cd")},
            {.id = "cts", .label = setting_choice_label(key, "cts")},
            {.id = "dsr", .label = setting_choice_label(key, "dsr")},
            {.id = "ignore", .label = setting_choice_label(key, "ignore")},
        },
        carrier_detect);
  }
  if (state->transfer_base_path_entry != nullptr) {
    sync_inheritable_entry(state->transfer_base_path_entry,
                           state->draft_store,
                           transfer_base_path_setting_key(),
                           transfer_base_path(state->draft_store));
  }
  if (state->transfer_text_send_rate_entry != nullptr) {
    sync_inheritable_entry(
        state->transfer_text_send_rate_entry, state->draft_store,
        transfer_text_send_bytes_per_second_setting_key(),
        std::to_string(
            transfer_text_send_bytes_per_second(state->draft_store)));
    state->transfer_text_send_rate_valid = true;
    set_entry_validation(state->transfer_text_send_rate_entry, true, {});
  }
  if (state->transfer_zmodem_autostart_combo != nullptr) {
    sync_zmodem_combo(state);
  }
  if (state->transfer_text_send_follow_return_code_combo != nullptr) {
    populate_boolean_combo(
        state->transfer_text_send_follow_return_code_combo,
        state->draft_store,
        transfer_text_send_follow_return_code_setting_key(),
        transfer_text_send_follow_return_code(state->draft_store));
  }
  if (state->log_enabled_combo != nullptr) {
    populate_boolean_combo(state->log_enabled_combo, state->draft_store,
                           terminal_log_enabled_setting_key(), log.enabled);
  }
  if (state->log_base_directory_entry != nullptr) {
    sync_inheritable_entry(state->log_base_directory_entry,
                           state->draft_store,
                           terminal_log_base_directory_setting_key(),
                           log.base_directory);
  }
  if (state->log_file_name_format_entry != nullptr) {
    sync_inheritable_entry(state->log_file_name_format_entry,
                           state->draft_store,
                           terminal_log_file_name_format_setting_key(),
                           log.file_name_format);
    set_log_file_name_format_validation(state, true, {});
  }
  if (state->log_mode_combo != nullptr) {
    const std::string effective = terminal_log_mode_to_string(log.mode);
    const SettingKey key = terminal_log_mode_setting_key();
    const std::string fallback_value =
        std::get<std::string>(setting_fallback_value(
            state->draft_store, key,
            SettingValue{std::string(terminal_log_raw)}));
    populate_inheritable_combo(
        state->log_mode_combo, state->draft_store, key,
        setting_choice_label(key, fallback_value),
        {
            {.id = terminal_log_raw,
             .label = setting_choice_label(key, terminal_log_raw)},
            {.id = terminal_log_cooked,
             .label = setting_choice_label(key, terminal_log_cooked)},
        },
        effective);
  }
  if (state->macro_list != nullptr) {
    rebuild_macro_list(state);
  }
  if (state->notebook != nullptr) {
    update_connection_pages(state);
  }
}

static void on_general_type_changed(GtkComboBox *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (state->synchronizing) {
    return;
  }
  update_general_type_from_widget(state);
  const bool previous_synchronizing = state->synchronizing;
  state->synchronizing = true;
  sync_zmodem_combo(state);
  state->synchronizing = previous_synchronizing;
  notify_changed(state);
}

static void on_general_name_changed(GtkEditable *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (state->synchronizing) {
    return;
  }
  update_general_name_from_widget(state);
  notify_changed(state);
}

static gboolean on_general_name_focus_out(GtkWidget *, GdkEventFocus *,
                                          gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  const char *text =
      gtk_entry_get_text(GTK_ENTRY(state->general_name_entry));
  if (text != nullptr && !ascii_blank(text)) {
    return FALSE;
  }

  const bool previous_synchronizing = state->synchronizing;
  state->synchronizing = true;
  gtk_entry_set_text(GTK_ENTRY(state->general_name_entry),
                     general_connection_name(state->draft_store).c_str());
  state->synchronizing = previous_synchronizing;
  return FALSE;
}

static void on_connection_hotkey_changed(SettingsWidgetState *state) {
  if (state->synchronizing) {
    return;
  }
  update_connection_hotkey_from_widget(state);
  if (key_binding_input_widget_is_valid(
          state->general_open_connection_input)) {
    const std::string effective =
        general_open_connection_hotkey_text(state->draft_store);
    set_key_binding_input_widget_empty_clear_enabled(
        state->general_open_connection_input, false);
    gtk_entry_set_placeholder_text(
        GTK_ENTRY(key_binding_input_widget_root(
            state->general_open_connection_input)),
        effective.empty()
            ? settings_ui_text(SettingsUiText::disabled)
            : settings_ui_text(SettingsUiText::press_key_combination));
    sync_key_binding_reset_button(
        state, state->general_open_connection_reset_button,
        general_open_connection_hotkey_setting_key());
  }
  update_action_sensitivity(state);
  notify_changed(state);
}

static void on_connection_hotkey_reset_clicked(GtkButton *,
                                               gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  clear_explicit_setting_value(
      &state->draft_store,
      general_open_connection_hotkey_setting_key());
  const bool previous_synchronizing = state->synchronizing;
  state->synchronizing = true;
  sync_key_binding_widget(
      state, state->general_open_connection_input,
      general_open_connection_hotkey_setting_key(),
      general_open_connection_hotkey_text(state->draft_store));
  sync_key_binding_reset_button(
      state, state->general_open_connection_reset_button,
      general_open_connection_hotkey_setting_key());
  set_key_binding_input_widget_external_error(
      state->general_open_connection_input, {});
  state->synchronizing = previous_synchronizing;
  update_action_sensitivity(state);
  notify_changed(state);
}

static void on_terminal_width_changed(GtkEditable *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (state->synchronizing) {
    return;
  }
  update_terminal_width_from_widget(state);
  update_action_sensitivity(state);
  notify_changed(state);
}

static void on_terminal_height_changed(GtkEditable *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (state->synchronizing) {
    return;
  }
  update_terminal_height_from_widget(state);
  update_action_sensitivity(state);
  notify_changed(state);
}

static void on_terminal_scrollback_lines_changed(GtkEditable *,
                                                 gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (state->synchronizing) {
    return;
  }
  update_terminal_scrollback_lines_from_widget(state);
  update_action_sensitivity(state);
  notify_changed(state);
}

static void on_terminal_zoom_changed(GtkEditable *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (state->synchronizing) {
    return;
  }
  update_terminal_zoom_from_widget(state);
  update_action_sensitivity(state);
  notify_changed(state);
}

static void on_terminal_auto_close_changed(GtkComboBox *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (state->synchronizing) {
    return;
  }
  update_terminal_auto_close_from_widget(state);
  notify_changed(state);
}

static void on_terminal_show_border_changed(GtkComboBox *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (state->synchronizing) {
    return;
  }
  update_terminal_show_border_from_widget(state);
  notify_changed(state);
}

static void on_terminal_border_width_changed(GtkEditable *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (state->synchronizing) {
    return;
  }
  update_terminal_border_width_from_widget(state);
  update_action_sensitivity(state);
  notify_changed(state);
}

static void on_terminal_bell_sound_changed(GtkEditable *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (state->synchronizing) {
    return;
  }
  update_terminal_bell_sound_from_widget(state);
  update_action_sensitivity(state);
  notify_changed(state);
}

static void restore_terminal_bell_sound_dialog_parent(GtkWidget *dialog) {
  if (dialog == nullptr || !GTK_IS_WINDOW(dialog)) {
    return;
  }
  GtkWindow *parent =
      gtk_window_get_transient_for(GTK_WINDOW(dialog));
  if (parent != nullptr &&
      !gtk_widget_in_destruction(GTK_WIDGET(parent))) {
    gtk_widget_set_sensitive(GTK_WIDGET(parent), TRUE);
  }
}

static void on_terminal_bell_sound_dialog_destroy(GtkWidget *dialog,
                                                  gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (state == nullptr || state->terminal_bell_sound_dialog != dialog) {
    return;
  }
  state->terminal_bell_sound_dialog = nullptr;
  restore_terminal_bell_sound_dialog_parent(dialog);
}

static void on_terminal_bell_sound_dialog_response(GtkDialog *dialog,
                                                   gint response,
                                                   gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (state == nullptr ||
      state->terminal_bell_sound_dialog != GTK_WIDGET(dialog)) {
    return;
  }

  if (response == GTK_RESPONSE_ACCEPT) {
    gchar *filename =
        gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
    if (filename != nullptr) {
      gchar *utf8_filename =
          g_filename_to_utf8(filename, -1, nullptr, nullptr, nullptr);
      if (utf8_filename != nullptr) {
        gtk_entry_set_text(GTK_ENTRY(state->terminal_bell_sound_entry),
                           utf8_filename);
        g_free(utf8_filename);
      }
      g_free(filename);
    }
  }

  state->terminal_bell_sound_dialog = nullptr;
  restore_terminal_bell_sound_dialog_parent(GTK_WIDGET(dialog));
  gtk_widget_destroy(GTK_WIDGET(dialog));
}

static void add_terminal_bell_sound_filter(GtkFileChooser *chooser) {
  GtkFileFilter *filter = gtk_file_filter_new();
  const std::string name = setting_label(terminal_bell_sound_setting_key());
  gtk_file_filter_set_name(filter, name.c_str());
  for (const char *pattern : {"*.oga", "*.ogg", "*.wav", "*.OGA",
                              "*.OGG", "*.WAV"}) {
    gtk_file_filter_add_pattern(filter, pattern);
  }
  gtk_file_filter_add_mime_type(filter, "audio/ogg");
  gtk_file_filter_add_mime_type(filter, "audio/wav");
  gtk_file_filter_add_mime_type(filter, "audio/x-wav");
  gtk_file_chooser_add_filter(chooser, filter);
}

static void select_current_terminal_bell_sound(GtkFileChooser *chooser,
                                               GtkWidget *entry) {
  const char *text = gtk_entry_get_text(GTK_ENTRY(entry));
  const std::string value = text == nullptr ? "" : text;
  std::string reason;
  if (value == terminal_text_default ||
      !terminal_bell_sound_is_valid(value, &reason)) {
    return;
  }

  gchar *filename =
      g_filename_from_utf8(value.c_str(), -1, nullptr, nullptr, nullptr);
  if (filename != nullptr) {
    gtk_file_chooser_set_filename(chooser, filename);
    g_free(filename);
  }
}

static void on_terminal_bell_sound_browse_clicked(GtkButton *,
                                                  gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (state == nullptr || state->root == nullptr) {
    return;
  }
  if (state->terminal_bell_sound_dialog != nullptr) {
    gtk_window_present(GTK_WINDOW(state->terminal_bell_sound_dialog));
    return;
  }

  GtkWidget *toplevel = gtk_widget_get_toplevel(state->root);
  GtkWindow *parent =
      GTK_IS_WINDOW(toplevel) ? GTK_WINDOW(toplevel) : nullptr;
  GtkWidget *dialog = gtk_file_chooser_dialog_new(
      settings_ui_text(SettingsUiText::select_file), parent,
      GTK_FILE_CHOOSER_ACTION_OPEN,
      settings_ui_text(SettingsUiText::cancel), GTK_RESPONSE_CANCEL,
      settings_ui_text(SettingsUiText::open), GTK_RESPONSE_ACCEPT, nullptr);
  assign_accessible_id(
      dialog, widget_id(state, "terminal_bell_sound_dialog").c_str());
  gtk_window_set_modal(GTK_WINDOW(dialog), FALSE);
  gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE);
  gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);

  GtkWidget *cancel_button = gtk_dialog_get_widget_for_response(
      GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
  GtkWidget *open_button = gtk_dialog_get_widget_for_response(
      GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
  assign_accessible_id(
      cancel_button,
      widget_id(state, "terminal_bell_sound_cancel_button").c_str());
  assign_accessible_id(
      open_button,
      widget_id(state, "terminal_bell_sound_open_button").c_str());

  GtkFileChooser *chooser = GTK_FILE_CHOOSER(dialog);
  gtk_file_chooser_set_local_only(chooser, TRUE);
  gtk_file_chooser_set_select_multiple(chooser, FALSE);
  add_terminal_bell_sound_filter(chooser);
  select_current_terminal_bell_sound(
      chooser, state->terminal_bell_sound_entry);

  state->terminal_bell_sound_dialog = dialog;
  g_signal_connect(dialog, "response",
                   G_CALLBACK(on_terminal_bell_sound_dialog_response), state);
  g_signal_connect(dialog, "destroy",
                   G_CALLBACK(on_terminal_bell_sound_dialog_destroy), state);
  if (parent != nullptr) {
    gtk_widget_set_sensitive(GTK_WIDGET(parent), FALSE);
  }
  gtk_widget_show_all(dialog);
  gtk_window_present(GTK_WINDOW(dialog));
}

static void on_terminal_font_primary_mode_changed(GtkComboBox *,
                                                  gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (state->synchronizing) {
    return;
  }
  update_terminal_font_mode_from_widget(
      state, state->terminal_font_primary_mode_combo,
      state->terminal_font_primary_button,
      terminal_font_primary_family_setting_key());
  notify_changed(state);
}

static void on_terminal_font_fallback_mode_changed(GtkComboBox *,
                                                   gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (state->synchronizing) {
    return;
  }
  update_terminal_font_mode_from_widget(
      state, state->terminal_font_fallback_mode_combo,
      state->terminal_font_fallback_button,
      terminal_font_fallback_family_setting_key());
  notify_changed(state);
}

static void on_terminal_font_primary_set(GtkFontButton *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (state->synchronizing) {
    return;
  }
  update_terminal_font_family_from_widget(
      state, state->terminal_font_primary_mode_combo,
      state->terminal_font_primary_button,
      terminal_font_primary_family_setting_key());
  notify_changed(state);
}

static void on_terminal_font_fallback_set(GtkFontButton *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (state->synchronizing) {
    return;
  }
  update_terminal_font_family_from_widget(
      state, state->terminal_font_fallback_mode_combo,
      state->terminal_font_fallback_button,
      terminal_font_fallback_family_setting_key());
  notify_changed(state);
}

static void on_general_exterior_background_mode_changed(
    GtkComboBox *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (state->synchronizing) {
    return;
  }
  update_general_color_mode_from_widget(
      state, GeneralColorField::exterior_background);
  notify_changed(state);
}

static void on_general_background_mode_changed(GtkComboBox *,
                                               gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (state->synchronizing) {
    return;
  }
  update_general_color_mode_from_widget(
      state, GeneralColorField::background);
  sync_terminal_type_entries(state);
  notify_changed(state);
}

static void on_general_exterior_background_color_set(GtkColorButton *,
                                                     gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (state->synchronizing) {
    return;
  }
  update_general_color_from_picker(
      state, GeneralColorField::exterior_background);
  notify_changed(state);
}

static void on_general_background_color_set(GtkColorButton *,
                                            gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (state->synchronizing) {
    return;
  }
  update_general_color_from_picker(state, GeneralColorField::background);
  sync_terminal_type_entries(state);
  notify_changed(state);
}

static void on_terminal_encoding_changed(GtkEditable *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (state->synchronizing) {
    return;
  }
  update_terminal_encoding_from_widget(state);
  update_action_sensitivity(state);
  notify_changed(state);
}

static void on_terminal_backspace_code_changed(GtkComboBox *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (state->synchronizing) {
    return;
  }
  update_terminal_backspace_code_from_widget(state);
  notify_changed(state);
}

static void on_terminal_cursor_key_mode_changed(GtkComboBox *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (state->synchronizing) {
    return;
  }
  update_terminal_cursor_key_mode_from_widget(state);
  notify_changed(state);
}

static void on_terminal_return_code_changed(GtkComboBox *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (state->synchronizing) {
    return;
  }
  update_terminal_return_code_from_widget(state);
  notify_changed(state);
}

enum class TerminalKeyBindingField {
  zoom_in,
  zoom_out,
  send_break,
};

static KeyBindingInputWidgetState *terminal_key_binding_input(
    SettingsWidgetState *state, TerminalKeyBindingField field) {
  switch (field) {
  case TerminalKeyBindingField::zoom_in:
    return state->terminal_zoom_in_key_input;
  case TerminalKeyBindingField::zoom_out:
    return state->terminal_zoom_out_key_input;
  case TerminalKeyBindingField::send_break:
    return state->terminal_send_break_key_input;
  }
  return nullptr;
}

static GtkWidget *terminal_key_binding_reset_button(
    SettingsWidgetState *state, TerminalKeyBindingField field) {
  switch (field) {
  case TerminalKeyBindingField::zoom_in:
    return state->terminal_zoom_in_key_reset_button;
  case TerminalKeyBindingField::zoom_out:
    return state->terminal_zoom_out_key_reset_button;
  case TerminalKeyBindingField::send_break:
    return state->terminal_send_break_key_reset_button;
  }
  return nullptr;
}

static SettingKey terminal_key_binding_setting_key(
    TerminalKeyBindingField field) {
  switch (field) {
  case TerminalKeyBindingField::zoom_in:
    return terminal_zoom_in_key_setting_key();
  case TerminalKeyBindingField::zoom_out:
    return terminal_zoom_out_key_setting_key();
  case TerminalKeyBindingField::send_break:
    return terminal_send_break_key_setting_key();
  }
  return {};
}

static std::string terminal_key_binding_effective_text(
    const SettingsWidgetState *state, TerminalKeyBindingField field) {
  switch (field) {
  case TerminalKeyBindingField::zoom_in:
    return terminal_zoom_in_key(state->draft_store);
  case TerminalKeyBindingField::zoom_out:
    return terminal_zoom_out_key(state->draft_store);
  case TerminalKeyBindingField::send_break:
    return terminal_send_break_key(state->draft_store);
  }
  return {};
}

static void update_terminal_key_binding_from_widget(
    SettingsWidgetState *state, TerminalKeyBindingField field) {
  switch (field) {
  case TerminalKeyBindingField::zoom_in:
    update_terminal_zoom_in_key_from_widget(state);
    return;
  case TerminalKeyBindingField::zoom_out:
    update_terminal_zoom_out_key_from_widget(state);
    return;
  case TerminalKeyBindingField::send_break:
    update_terminal_send_break_key_from_widget(state);
    return;
  }
}

static void on_terminal_key_binding_changed(
    SettingsWidgetState *state, TerminalKeyBindingField field) {
  if (state->synchronizing) {
    return;
  }
  KeyBindingInputWidgetState *input =
      terminal_key_binding_input(state, field);
  if (!key_binding_input_widget_is_valid(input)) {
    update_terminal_key_binding_validation(state);
    notify_changed(state);
    return;
  }
  update_terminal_key_binding_from_widget(state, field);
  const SettingKey key = terminal_key_binding_setting_key(field);
  const std::string effective =
      terminal_key_binding_effective_text(state, field);
  set_key_binding_input_widget_empty_clear_enabled(input, false);
  gtk_entry_set_placeholder_text(
      GTK_ENTRY(key_binding_input_widget_root(input)),
      effective.empty()
          ? settings_ui_text(SettingsUiText::disabled)
          : settings_ui_text(SettingsUiText::press_key_combination));
  GtkWidget *reset_button = terminal_key_binding_reset_button(state, field);
  sync_key_binding_reset_button(state, reset_button, key);
  update_terminal_key_binding_validation(state);
  notify_changed(state);
}

static void reset_terminal_key_binding(SettingsWidgetState *state,
                                       TerminalKeyBindingField field) {
  const SettingKey key = terminal_key_binding_setting_key(field);
  KeyBindingInputWidgetState *input =
      terminal_key_binding_input(state, field);
  GtkWidget *button = terminal_key_binding_reset_button(state, field);
  clear_explicit_setting_value(&state->draft_store, key);
  const std::string effective =
      terminal_key_binding_effective_text(state, field);
  const bool previous_synchronizing = state->synchronizing;
  state->synchronizing = true;
  sync_key_binding_widget(state, input, key, effective);
  sync_key_binding_reset_button(state, button, key);
  state->synchronizing = previous_synchronizing;
  update_terminal_key_binding_validation(state);
  notify_changed(state);
}

static void on_terminal_zoom_in_key_reset_clicked(GtkButton *,
                                                  gpointer data) {
  reset_terminal_key_binding(
      static_cast<SettingsWidgetState *>(data),
      TerminalKeyBindingField::zoom_in);
}

static void on_terminal_zoom_out_key_reset_clicked(GtkButton *,
                                                   gpointer data) {
  reset_terminal_key_binding(
      static_cast<SettingsWidgetState *>(data),
      TerminalKeyBindingField::zoom_out);
}

static void on_terminal_send_break_key_reset_clicked(GtkButton *,
                                                     gpointer data) {
  reset_terminal_key_binding(
      static_cast<SettingsWidgetState *>(data),
      TerminalKeyBindingField::send_break);
}

static void on_local_command_line_changed(GtkEditable *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (state->synchronizing) {
    return;
  }
  update_local_command_line_from_widget(state);
  update_action_sensitivity(state);
  notify_changed(state);
}

static void on_telnet_address_changed(GtkEditable *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (state->synchronizing) {
    return;
  }
  update_telnet_address_from_widget(state);
  notify_changed(state);
}

static void on_telnet_port_changed(GtkEditable *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (state->synchronizing) {
    return;
  }
  update_telnet_port_from_widget(state);
  update_action_sensitivity(state);
  notify_changed(state);
}

static void on_telnet_terminal_type_changed(GtkEditable *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (state->synchronizing) {
    return;
  }
  update_telnet_terminal_type_from_widget(state);
  notify_changed(state);
}

static gboolean on_telnet_terminal_type_focus_out(GtkWidget *,
                                                  GdkEventFocus *,
                                                  gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  const char *text =
      gtk_entry_get_text(GTK_ENTRY(state->telnet_terminal_type_entry));
  if (text != nullptr && !ascii_blank(text)) {
    return FALSE;
  }

  const bool previous_synchronizing = state->synchronizing;
  state->synchronizing = true;
  const TelnetConnectionSettings settings =
      telnet_connection_settings(state->draft_store);
  sync_inheritable_entry(state->telnet_terminal_type_entry,
                         state->draft_store,
                         telnet_terminal_type_setting_key(),
                         settings.terminal_type);
  state->synchronizing = previous_synchronizing;
  return FALSE;
}

static void on_ssh_address_changed(GtkEditable *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (state->synchronizing) {
    return;
  }
  update_ssh_address_from_widget(state);
  notify_changed(state);
}

static void on_ssh_port_changed(GtkEditable *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (state->synchronizing) {
    return;
  }
  update_ssh_port_from_widget(state);
  update_action_sensitivity(state);
  notify_changed(state);
}

static void on_ssh_username_changed(GtkEditable *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (state->synchronizing) {
    return;
  }
  update_ssh_username_from_widget(state);
  notify_changed(state);
}

static void on_ssh_identity_file_changed(GtkEditable *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (state->synchronizing) {
    return;
  }
  update_ssh_identity_file_from_widget(state);
  notify_changed(state);
}

static void on_ssh_terminal_type_changed(GtkEditable *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (state->synchronizing) {
    return;
  }
  update_ssh_terminal_type_from_widget(state);
  notify_changed(state);
}

static gboolean on_ssh_terminal_type_focus_out(GtkWidget *, GdkEventFocus *,
                                               gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  const char *text =
      gtk_entry_get_text(GTK_ENTRY(state->ssh_terminal_type_entry));
  if (text != nullptr && !ascii_blank(text)) {
    return FALSE;
  }

  const bool previous_synchronizing = state->synchronizing;
  state->synchronizing = true;
  const SshConnectionSettings settings =
      ssh_connection_settings(state->draft_store);
  sync_inheritable_entry(state->ssh_terminal_type_entry,
                         state->draft_store,
                         ssh_terminal_type_setting_key(),
                         settings.terminal_type);
  state->synchronizing = previous_synchronizing;
  return FALSE;
}

static void on_sftp_local_directory_changed(GtkEditable *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (state->synchronizing) {
    return;
  }
  update_sftp_local_directory_from_widget(state);
  notify_changed(state);
}

static void on_sftp_remote_directory_changed(GtkEditable *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (state->synchronizing) {
    return;
  }
  update_sftp_remote_directory_from_widget(state);
  notify_changed(state);
}

static void on_ftp_address_changed(GtkEditable *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (state->synchronizing) {
    return;
  }
  update_ftp_address_from_widget(state);
  notify_changed(state);
}

static void on_ftp_port_changed(GtkEditable *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (state->synchronizing) {
    return;
  }
  update_ftp_port_from_widget(state);
  update_action_sensitivity(state);
  notify_changed(state);
}

static void on_ftp_username_changed(GtkEditable *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (state->synchronizing) {
    return;
  }
  update_ftp_username_from_widget(state);
  notify_changed(state);
}

static void on_ftp_data_connection_mode_changed(GtkComboBox *,
                                                gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (state->synchronizing) {
    return;
  }
  update_ftp_data_connection_mode_from_widget(state);
  notify_changed(state);
}

static void on_ftp_local_directory_changed(GtkEditable *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (state->synchronizing) {
    return;
  }
  update_ftp_local_directory_from_widget(state);
  notify_changed(state);
}

static void on_ftp_remote_directory_changed(GtkEditable *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (state->synchronizing) {
    return;
  }
  update_ftp_remote_directory_from_widget(state);
  notify_changed(state);
}

static void on_serial_device_changed(GtkComboBox *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (state->synchronizing) {
    return;
  }
  update_serial_device_from_widget(state);
  notify_changed(state);
}

static void on_serial_device_match_mode_changed(GtkComboBox *,
                                                gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (state->synchronizing) {
    return;
  }
  update_serial_device_match_mode_from_widget(state);
  notify_changed(state);
}

static void on_serial_device_popup_shown_changed(GObject *, GParamSpec *,
                                                 gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (state->serial_device_refresh_pending &&
      !serial_device_combo_popup_is_shown(state)) {
    refresh_serial_device_widgets(state);
  }
}

static void on_serial_baudrate_changed(GtkEditable *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (state->synchronizing) {
    return;
  }
  update_serial_baudrate_from_widget(state);
  update_action_sensitivity(state);
  notify_changed(state);
}

static void on_serial_bits_changed(GtkComboBox *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (state->synchronizing) {
    return;
  }
  update_serial_bits_from_widget(state);
  notify_changed(state);
}

static void on_serial_parity_changed(GtkComboBox *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (state->synchronizing) {
    return;
  }
  update_serial_parity_from_widget(state);
  notify_changed(state);
}

static void on_serial_stop_bit_changed(GtkComboBox *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (state->synchronizing) {
    return;
  }
  update_serial_stop_bit_from_widget(state);
  notify_changed(state);
}

static void on_serial_flow_control_changed(GtkComboBox *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (state->synchronizing) {
    return;
  }
  update_serial_flow_control_from_widget(state);
  notify_changed(state);
}

static void on_serial_carrier_detect_changed(GtkComboBox *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (state->synchronizing) {
    return;
  }
  update_serial_carrier_detect_from_widget(state);
  notify_changed(state);
}

static void on_transfer_base_path_changed(GtkEditable *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (state->synchronizing) {
    return;
  }
  update_transfer_base_path_from_widget(state);
  notify_changed(state);
}

static void on_transfer_text_send_rate_changed(GtkEditable *,
                                               gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (state->synchronizing) {
    return;
  }
  update_transfer_text_send_rate_from_widget(state);
  update_action_sensitivity(state);
  notify_changed(state);
}

static void on_transfer_zmodem_autostart_changed(GtkComboBox *,
                                                 gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (state->synchronizing) {
    return;
  }
  update_transfer_zmodem_autostart_from_widget(state);
  notify_changed(state);
}

static void on_transfer_text_send_follow_return_code_changed(
    GtkComboBox *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (state->synchronizing) {
    return;
  }
  update_transfer_text_send_follow_return_code_from_widget(state);
  notify_changed(state);
}

static void on_log_enabled_changed(GtkComboBox *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (state->synchronizing) {
    return;
  }
  update_log_enabled_from_widget(state);
  notify_changed(state);
}

static void on_log_base_directory_changed(GtkEditable *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (state->synchronizing) {
    return;
  }
  update_log_base_directory_from_widget(state);
  notify_changed(state);
}

static void on_log_file_name_format_changed(GtkEditable *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (state->synchronizing) {
    return;
  }
  update_log_file_name_format_from_widget(state);
  update_action_sensitivity(state);
  notify_changed(state);
}

static void on_log_mode_changed(GtkComboBox *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (state->synchronizing) {
    return;
  }
  update_log_mode_from_widget(state);
  notify_changed(state);
}

static void on_apply_clicked(GtkButton *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  state->applied_store = state->draft_store;
  if (state->callbacks.apply) {
    state->callbacks.apply(state->applied_store);
  }
}

static void on_save_clicked(GtkButton *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (state->callbacks.save && state->callbacks.save(state->draft_store)) {
    state->applied_store = state->draft_store;
  }
}

static void on_cancel_clicked(GtkButton *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  state->draft_store = state->applied_store;
  if (state->callbacks.cancel) {
    state->callbacks.cancel();
  }
}

static GtkWidget *create_general_page(SettingsWidgetState *state) {
  const std::string page_id = widget_id(state, "general_page");
  GtkWidget *page = create_page_grid(page_id.c_str());
  int row = 0;
  if (state->mode == SettingsWidgetMode::connection) {
    state->general_name_entry =
        create_entry(widget_id(state, "general_name_entry"));
    g_signal_connect(state->general_name_entry, "changed",
                     G_CALLBACK(on_general_name_changed), state);
    g_signal_connect(state->general_name_entry, "focus-out-event",
                     G_CALLBACK(on_general_name_focus_out), state);
    attach_row(page, row++, general_name_setting_key(),
               state->general_name_entry);
  }

  const std::string type_id = widget_id(state, "general_type_combo");
  state->general_type_combo = create_combo_box(type_id.c_str());
  gtk_widget_set_sensitive(state->general_type_combo,
                           state->is_runtime ? FALSE : TRUE);
  g_signal_connect(state->general_type_combo, "changed",
                   G_CALLBACK(on_general_type_changed), state);
  attach_row(page, row++, general_type_setting_key(),
             state->general_type_combo);

  if (state->mode == SettingsWidgetMode::connection &&
      !state->is_runtime) {
    const std::string hotkey_id =
        widget_id(state, "general_open_connection_entry");
    state->general_open_connection_input =
        create_key_binding_input_widget({
            .text = "",
            .accessible_id = hotkey_id,
            .changed = [state]() {
              on_connection_hotkey_changed(state);
            },
        });
    GtkWidget *hotkey_row =
        gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(
        GTK_BOX(hotkey_row),
        key_binding_input_widget_root(
            state->general_open_connection_input),
        TRUE, TRUE, 0);
    state->general_open_connection_reset_button =
        gtk_button_new_with_label(
            settings_ui_text(SettingsUiText::reset));
    const std::string reset_id =
        widget_id(state, "general_open_connection_reset_button");
    assign_accessible_id(state->general_open_connection_reset_button,
                         reset_id.c_str());
    g_signal_connect(
        state->general_open_connection_reset_button, "clicked",
        G_CALLBACK(on_connection_hotkey_reset_clicked), state);
    gtk_box_pack_start(
        GTK_BOX(hotkey_row),
        state->general_open_connection_reset_button, FALSE, FALSE, 0);
    attach_row(page, row++, general_open_connection_hotkey_setting_key(),
               hotkey_row);
  }

  GtkWidget *exterior_color_row =
      gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  const std::string exterior_mode_id =
      widget_id(state, "general_exterior_background_mode_combo");
  state->general_exterior_background_mode_combo =
      create_combo_box(exterior_mode_id.c_str());
  g_signal_connect(
      state->general_exterior_background_mode_combo, "changed",
      G_CALLBACK(on_general_exterior_background_mode_changed), state);
  gtk_box_pack_start(
      GTK_BOX(exterior_color_row),
      state->general_exterior_background_mode_combo, TRUE, TRUE, 0);
  state->general_exterior_background_button = gtk_color_button_new();
  const std::string exterior_button_id =
      widget_id(state, "general_exterior_background_button");
  assign_accessible_id(state->general_exterior_background_button,
                       exterior_button_id.c_str());
  gtk_color_chooser_set_use_alpha(
      GTK_COLOR_CHOOSER(state->general_exterior_background_button), FALSE);
  set_color_button_rgb(state->general_exterior_background_button,
                       std::nullopt);
  g_signal_connect(
      state->general_exterior_background_button, "color-set",
      G_CALLBACK(on_general_exterior_background_color_set), state);
  gtk_box_pack_start(
      GTK_BOX(exterior_color_row),
      state->general_exterior_background_button, FALSE, FALSE, 0);
  attach_row(page, row++, general_exterior_background_setting_key(),
             exterior_color_row);

  GtkWidget *background_color_row =
      gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  const std::string background_mode_id =
      widget_id(state, "general_background_mode_combo");
  state->general_background_mode_combo =
      create_combo_box(background_mode_id.c_str());
  g_signal_connect(state->general_background_mode_combo, "changed",
                   G_CALLBACK(on_general_background_mode_changed), state);
  gtk_box_pack_start(GTK_BOX(background_color_row),
                     state->general_background_mode_combo, TRUE, TRUE, 0);
  state->general_background_button = gtk_color_button_new();
  const std::string background_button_id =
      widget_id(state, "general_background_button");
  assign_accessible_id(state->general_background_button,
                       background_button_id.c_str());
  gtk_color_chooser_set_use_alpha(
      GTK_COLOR_CHOOSER(state->general_background_button), FALSE);
  set_color_button_rgb(state->general_background_button, std::nullopt);
  g_signal_connect(state->general_background_button, "color-set",
                   G_CALLBACK(on_general_background_color_set), state);
  gtk_box_pack_start(GTK_BOX(background_color_row),
                     state->general_background_button, FALSE, FALSE, 0);
  attach_row(page, row, general_background_setting_key(),
             background_color_row);

  return page;
}

static GtkWidget *create_terminal_page(SettingsWidgetState *state) {
  const std::string page_id = widget_id(state, "terminal_page");
  const std::string contents_id =
      widget_id(state, "terminal_page_contents");
  GtkWidget *page = create_page_grid(contents_id.c_str());
  gtk_grid_set_row_spacing(GTK_GRID(page), 6);
  GtkWidget *scroller = gtk_scrolled_window_new(nullptr, nullptr);
  assign_accessible_id(scroller, page_id.c_str());
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
                                 GTK_POLICY_NEVER,
                                 GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_overlay_scrolling(
      GTK_SCROLLED_WINDOW(scroller), FALSE);
  gtk_scrolled_window_set_propagate_natural_height(
      GTK_SCROLLED_WINDOW(scroller), FALSE);
  assign_accessible_id(
      gtk_scrolled_window_get_vscrollbar(GTK_SCROLLED_WINDOW(scroller)),
      widget_id(state, "terminal_page_scrollbar").c_str());
  // Keep the grid margins inside a painted surface instead of exposing the
  // implicit GtkViewport window around the direct scroll child.
  GtkWidget *page_surface = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_container_add(GTK_CONTAINER(page_surface), page);
  gtk_container_add(GTK_CONTAINER(scroller), page_surface);

  const std::string encoding_combo_id =
      widget_id(state, "terminal_encoding_combo");
  const std::string encoding_entry_id =
      widget_id(state, "terminal_encoding_entry");
  state->terminal_encoding_combo = create_editable_combo_box(
      encoding_combo_id.c_str(), encoding_entry_id.c_str(),
      &state->terminal_encoding_entry);
  const std::string backspace_id =
      widget_id(state, "terminal_backspace_code_combo");
  state->terminal_backspace_code_combo =
      create_combo_box(backspace_id.c_str());
  const std::string cursor_id =
      widget_id(state, "terminal_cursor_key_mode_combo");
  state->terminal_cursor_key_mode_combo =
      create_combo_box(cursor_id.c_str());
  const std::string return_id =
      widget_id(state, "terminal_return_code_combo");
  state->terminal_return_code_combo = create_combo_box(return_id.c_str());
  if (state->terminal_encoding_entry != nullptr) {
    g_signal_connect(state->terminal_encoding_entry, "changed",
                     G_CALLBACK(on_terminal_encoding_changed), state);
  }
  g_signal_connect(state->terminal_backspace_code_combo, "changed",
                   G_CALLBACK(on_terminal_backspace_code_changed), state);
  g_signal_connect(state->terminal_cursor_key_mode_combo, "changed",
                   G_CALLBACK(on_terminal_cursor_key_mode_changed), state);
  g_signal_connect(state->terminal_return_code_combo, "changed",
                   G_CALLBACK(on_terminal_return_code_changed), state);
  attach_row(page, 0, terminal_encoding_setting_key(),
             state->terminal_encoding_combo);
  attach_row(page, 1, terminal_backspace_code_setting_key(),
             state->terminal_backspace_code_combo);
  attach_row(page, 2, terminal_cursor_key_mode_setting_key(),
             state->terminal_cursor_key_mode_combo);
  attach_row(page, 3, terminal_return_code_setting_key(),
             state->terminal_return_code_combo);

  state->terminal_width_entry =
      create_entry(widget_id(state, "terminal_width_entry"));
  g_signal_connect(state->terminal_width_entry, "changed",
                   G_CALLBACK(on_terminal_width_changed), state);
  attach_row(page, 4, terminal_width_setting_key(),
             state->terminal_width_entry);

  state->terminal_height_entry =
      create_entry(widget_id(state, "terminal_height_entry"));
  g_signal_connect(state->terminal_height_entry, "changed",
                   G_CALLBACK(on_terminal_height_changed), state);
  attach_row(page, 5, terminal_height_setting_key(),
             state->terminal_height_entry);

  state->terminal_scrollback_lines_entry =
      create_entry(widget_id(state, "terminal_scrollback_lines_entry"));
  g_signal_connect(state->terminal_scrollback_lines_entry, "changed",
                   G_CALLBACK(on_terminal_scrollback_lines_changed), state);
  attach_row(page, 6, terminal_scrollback_lines_setting_key(),
             state->terminal_scrollback_lines_entry);

  state->terminal_zoom_entry =
      create_entry(widget_id(state, "terminal_zoom_entry"));
  g_signal_connect(state->terminal_zoom_entry, "changed",
                   G_CALLBACK(on_terminal_zoom_changed), state);
  attach_row(page, 7, terminal_zoom_setting_key(),
             state->terminal_zoom_entry);

  const std::string auto_close_id =
      widget_id(state, "terminal_auto_close_combo");
  state->terminal_auto_close_combo = create_combo_box(auto_close_id.c_str());
  g_signal_connect(state->terminal_auto_close_combo, "changed",
                   G_CALLBACK(on_terminal_auto_close_changed), state);
  attach_row(page, 8, terminal_auto_close_setting_key(),
             state->terminal_auto_close_combo);

  const std::string show_border_id =
      widget_id(state, "terminal_show_border_combo");
  state->terminal_show_border_combo =
      create_combo_box(show_border_id.c_str());
  g_signal_connect(state->terminal_show_border_combo, "changed",
                   G_CALLBACK(on_terminal_show_border_changed), state);
  attach_row(page, 9, terminal_show_border_setting_key(),
             state->terminal_show_border_combo);

  state->terminal_border_width_entry =
      create_entry(widget_id(state, "terminal_border_width_entry"));
  g_signal_connect(state->terminal_border_width_entry, "changed",
                   G_CALLBACK(on_terminal_border_width_changed), state);
  attach_row(page, 10, terminal_border_width_setting_key(),
             state->terminal_border_width_entry);

  state->terminal_bell_sound_entry =
      create_entry(widget_id(state, "terminal_bell_sound_entry"));
  g_signal_connect(state->terminal_bell_sound_entry, "changed",
                   G_CALLBACK(on_terminal_bell_sound_changed), state);
  GtkWidget *bell_sound_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_pack_start(GTK_BOX(bell_sound_row),
                     state->terminal_bell_sound_entry, TRUE, TRUE, 0);
  GtkWidget *bell_sound_browse =
      gtk_button_new_with_label(settings_ui_text(SettingsUiText::select_file));
  assign_accessible_id(
      bell_sound_browse,
      widget_id(state, "terminal_bell_sound_browse_button").c_str());
  g_signal_connect(bell_sound_browse, "clicked",
                   G_CALLBACK(on_terminal_bell_sound_browse_clicked), state);
  gtk_box_pack_start(GTK_BOX(bell_sound_row), bell_sound_browse, FALSE, FALSE,
                     0);
  attach_row(page, 11, terminal_bell_sound_setting_key(), bell_sound_row);

  const std::string zoom_in_id =
      widget_id(state, "terminal_zoom_in_key_entry");
  state->terminal_zoom_in_key_input = create_key_binding_input_widget({
      .text = "",
      .accessible_id = zoom_in_id,
      .changed = [state]() {
        on_terminal_key_binding_changed(state,
                                        TerminalKeyBindingField::zoom_in);
      },
  });
  GtkWidget *zoom_in_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_pack_start(
      GTK_BOX(zoom_in_row),
      key_binding_input_widget_root(state->terminal_zoom_in_key_input),
      TRUE, TRUE, 0);
  state->terminal_zoom_in_key_reset_button =
      gtk_button_new_with_label(settings_ui_text(SettingsUiText::reset));
  const std::string zoom_in_reset_id =
      widget_id(state, "terminal_zoom_in_key_reset_button");
  assign_accessible_id(state->terminal_zoom_in_key_reset_button,
                       zoom_in_reset_id.c_str());
  g_signal_connect(state->terminal_zoom_in_key_reset_button, "clicked",
                   G_CALLBACK(on_terminal_zoom_in_key_reset_clicked), state);
  gtk_box_pack_start(GTK_BOX(zoom_in_row),
                     state->terminal_zoom_in_key_reset_button, FALSE, FALSE,
                     0);
  attach_row(page, 12, terminal_zoom_in_key_setting_key(), zoom_in_row);

  const std::string zoom_out_id =
      widget_id(state, "terminal_zoom_out_key_entry");
  state->terminal_zoom_out_key_input = create_key_binding_input_widget({
      .text = "",
      .accessible_id = zoom_out_id,
      .changed = [state]() {
        on_terminal_key_binding_changed(state,
                                        TerminalKeyBindingField::zoom_out);
      },
  });
  GtkWidget *zoom_out_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_pack_start(
      GTK_BOX(zoom_out_row),
      key_binding_input_widget_root(state->terminal_zoom_out_key_input),
      TRUE, TRUE, 0);
  state->terminal_zoom_out_key_reset_button =
      gtk_button_new_with_label(settings_ui_text(SettingsUiText::reset));
  const std::string zoom_out_reset_id =
      widget_id(state, "terminal_zoom_out_key_reset_button");
  assign_accessible_id(state->terminal_zoom_out_key_reset_button,
                       zoom_out_reset_id.c_str());
  g_signal_connect(state->terminal_zoom_out_key_reset_button, "clicked",
                   G_CALLBACK(on_terminal_zoom_out_key_reset_clicked), state);
  gtk_box_pack_start(GTK_BOX(zoom_out_row),
                     state->terminal_zoom_out_key_reset_button, FALSE, FALSE,
                     0);
  attach_row(page, 13, terminal_zoom_out_key_setting_key(), zoom_out_row);

  const std::string send_break_id =
      widget_id(state, "terminal_send_break_key_entry");
  state->terminal_send_break_key_input = create_key_binding_input_widget({
      .text = "",
      .accessible_id = send_break_id,
      .changed = [state]() {
        on_terminal_key_binding_changed(
            state, TerminalKeyBindingField::send_break);
      },
  });
  GtkWidget *send_break_row =
      gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_pack_start(
      GTK_BOX(send_break_row),
      key_binding_input_widget_root(state->terminal_send_break_key_input),
      TRUE, TRUE, 0);
  state->terminal_send_break_key_reset_button =
      gtk_button_new_with_label(settings_ui_text(SettingsUiText::reset));
  const std::string send_break_reset_id =
      widget_id(state, "terminal_send_break_key_reset_button");
  assign_accessible_id(state->terminal_send_break_key_reset_button,
                       send_break_reset_id.c_str());
  g_signal_connect(state->terminal_send_break_key_reset_button, "clicked",
                   G_CALLBACK(on_terminal_send_break_key_reset_clicked),
                   state);
  gtk_box_pack_start(GTK_BOX(send_break_row),
                     state->terminal_send_break_key_reset_button, FALSE,
                     FALSE, 0);
  attach_row(page, 14, terminal_send_break_key_setting_key(),
             send_break_row);

  GtkWidget *primary_font_row =
      gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  state->terminal_font_primary_mode_combo = create_combo_box(
      widget_id(state, "terminal_font_primary_mode_combo").c_str());
  state->terminal_font_primary_button = create_font_family_button(
      widget_id(state, "terminal_font_primary_button"),
      settings_ui_text(SettingsUiText::select_primary_terminal_font));
  g_signal_connect(state->terminal_font_primary_mode_combo, "changed",
                   G_CALLBACK(on_terminal_font_primary_mode_changed),
                   state);
  g_signal_connect(state->terminal_font_primary_button, "font-set",
                   G_CALLBACK(on_terminal_font_primary_set), state);
  gtk_box_pack_start(GTK_BOX(primary_font_row),
                     state->terminal_font_primary_mode_combo, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(primary_font_row),
                     state->terminal_font_primary_button, TRUE, TRUE, 0);
  attach_row(page, 15, terminal_font_primary_family_setting_key(),
             primary_font_row);

  GtkWidget *fallback_font_row =
      gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  state->terminal_font_fallback_mode_combo = create_combo_box(
      widget_id(state, "terminal_font_fallback_mode_combo").c_str());
  state->terminal_font_fallback_button = create_font_family_button(
      widget_id(state, "terminal_font_fallback_button"),
      settings_ui_text(SettingsUiText::select_secondary_terminal_font));
  g_signal_connect(state->terminal_font_fallback_mode_combo, "changed",
                   G_CALLBACK(on_terminal_font_fallback_mode_changed),
                   state);
  g_signal_connect(state->terminal_font_fallback_button, "font-set",
                   G_CALLBACK(on_terminal_font_fallback_set), state);
  gtk_box_pack_start(GTK_BOX(fallback_font_row),
                     state->terminal_font_fallback_mode_combo, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(fallback_font_row),
                     state->terminal_font_fallback_button, TRUE, TRUE, 0);
  attach_row(page, 16, terminal_font_fallback_family_setting_key(),
             fallback_font_row);

  return scroller;
}

static GtkWidget *create_local_page(SettingsWidgetState *state) {
  const std::string page_id = widget_id(state, "local_page");
  GtkWidget *page = create_page_grid(page_id.c_str());

  state->local_command_line_entry =
      create_entry(widget_id(state, "local_command_line_entry"));
  gtk_widget_set_sensitive(state->local_command_line_entry,
                           state->is_runtime ? FALSE : TRUE);
  g_signal_connect(state->local_command_line_entry, "changed",
                   G_CALLBACK(on_local_command_line_changed), state);
  attach_row(page, 0, local_command_line_setting_key(),
             state->local_command_line_entry);

  return page;
}

static GtkWidget *create_telnet_page(SettingsWidgetState *state) {
  const std::string page_id = widget_id(state, "telnet_page");
  GtkWidget *page = create_page_grid(page_id.c_str());

  state->telnet_address_entry =
      create_entry(widget_id(state, "telnet_address_entry"));
  gtk_widget_set_sensitive(state->telnet_address_entry,
                           state->is_runtime ? FALSE : TRUE);
  g_signal_connect(state->telnet_address_entry, "changed",
                   G_CALLBACK(on_telnet_address_changed), state);
  attach_row(page, 0, telnet_address_setting_key(),
             state->telnet_address_entry);

  state->telnet_port_entry =
      create_entry(widget_id(state, "telnet_port_entry"));
  gtk_widget_set_sensitive(state->telnet_port_entry,
                           state->is_runtime ? FALSE : TRUE);
  g_signal_connect(state->telnet_port_entry, "changed",
                   G_CALLBACK(on_telnet_port_changed), state);
  attach_row(page, 1, telnet_port_setting_key(), state->telnet_port_entry);

  state->telnet_terminal_type_entry =
      create_entry(widget_id(state, "telnet_terminal_type_entry"));
  gtk_widget_set_sensitive(state->telnet_terminal_type_entry,
                           state->is_runtime ? FALSE : TRUE);
  g_signal_connect(state->telnet_terminal_type_entry, "changed",
                   G_CALLBACK(on_telnet_terminal_type_changed), state);
  g_signal_connect(state->telnet_terminal_type_entry, "focus-out-event",
                   G_CALLBACK(on_telnet_terminal_type_focus_out), state);
  attach_row(page, 2, telnet_terminal_type_setting_key(),
             state->telnet_terminal_type_entry);

  return page;
}

static GtkWidget *create_ssh_page(SettingsWidgetState *state) {
  const std::string page_id = widget_id(state, "ssh_page");
  GtkWidget *page = create_page_grid(page_id.c_str());
  const gboolean sensitive = state->is_runtime ? FALSE : TRUE;

  state->ssh_address_entry =
      create_entry(widget_id(state, "ssh_address_entry"));
  gtk_widget_set_sensitive(state->ssh_address_entry, sensitive);
  g_signal_connect(state->ssh_address_entry, "changed",
                   G_CALLBACK(on_ssh_address_changed), state);
  attach_row(page, 0, ssh_address_setting_key(), state->ssh_address_entry);

  state->ssh_port_entry =
      create_entry(widget_id(state, "ssh_port_entry"));
  gtk_widget_set_sensitive(state->ssh_port_entry, sensitive);
  g_signal_connect(state->ssh_port_entry, "changed",
                   G_CALLBACK(on_ssh_port_changed), state);
  attach_row(page, 1, ssh_port_setting_key(), state->ssh_port_entry);

  state->ssh_username_entry =
      create_entry(widget_id(state, "ssh_username_entry"));
  gtk_widget_set_sensitive(state->ssh_username_entry, sensitive);
  g_signal_connect(state->ssh_username_entry, "changed",
                   G_CALLBACK(on_ssh_username_changed), state);
  attach_row(page, 2, ssh_username_setting_key(),
             state->ssh_username_entry);

  state->ssh_identity_file_entry =
      create_entry(widget_id(state, "ssh_identity_file_entry"));
  gtk_widget_set_sensitive(state->ssh_identity_file_entry, sensitive);
  g_signal_connect(state->ssh_identity_file_entry, "changed",
                   G_CALLBACK(on_ssh_identity_file_changed), state);
  attach_row(page, 3, ssh_identity_file_setting_key(),
             state->ssh_identity_file_entry);

  state->ssh_terminal_type_entry =
      create_entry(widget_id(state, "ssh_terminal_type_entry"));
  gtk_widget_set_sensitive(state->ssh_terminal_type_entry, sensitive);
  g_signal_connect(state->ssh_terminal_type_entry, "changed",
                   G_CALLBACK(on_ssh_terminal_type_changed), state);
  g_signal_connect(state->ssh_terminal_type_entry, "focus-out-event",
                   G_CALLBACK(on_ssh_terminal_type_focus_out), state);
  state->ssh_terminal_type_label =
      attach_row(page, 4, ssh_terminal_type_setting_key(),
                 state->ssh_terminal_type_entry);
  gtk_widget_set_no_show_all(state->ssh_terminal_type_label, TRUE);
  gtk_widget_set_no_show_all(state->ssh_terminal_type_entry, TRUE);

  return page;
}

static GtkWidget *create_sftp_page(SettingsWidgetState *state) {
  const std::string page_id = widget_id(state, "sftp_page");
  GtkWidget *page = create_page_grid(page_id.c_str());

  state->sftp_local_directory_entry =
      create_entry(widget_id(state, "sftp_local_directory_entry"));
  g_signal_connect(state->sftp_local_directory_entry, "changed",
                   G_CALLBACK(on_sftp_local_directory_changed), state);
  attach_row(page, 0, sftp_local_directory_setting_key(),
             state->sftp_local_directory_entry);

  state->sftp_remote_directory_entry =
      create_entry(widget_id(state, "sftp_remote_directory_entry"));
  g_signal_connect(state->sftp_remote_directory_entry, "changed",
                   G_CALLBACK(on_sftp_remote_directory_changed), state);
  attach_row(page, 1, sftp_remote_directory_setting_key(),
             state->sftp_remote_directory_entry);

  return page;
}

static GtkWidget *create_ftp_page(SettingsWidgetState *state) {
  const std::string page_id = widget_id(state, "ftp_page");
  GtkWidget *page = create_page_grid(page_id.c_str());
  const gboolean endpoint_sensitive = state->is_runtime ? FALSE : TRUE;

  state->ftp_address_entry =
      create_entry(widget_id(state, "ftp_address_entry"));
  gtk_widget_set_sensitive(state->ftp_address_entry, endpoint_sensitive);
  g_signal_connect(state->ftp_address_entry, "changed",
                   G_CALLBACK(on_ftp_address_changed), state);
  attach_row(page, 0, ftp_address_setting_key(), state->ftp_address_entry);

  state->ftp_port_entry =
      create_entry(widget_id(state, "ftp_port_entry"));
  gtk_widget_set_sensitive(state->ftp_port_entry, endpoint_sensitive);
  g_signal_connect(state->ftp_port_entry, "changed",
                   G_CALLBACK(on_ftp_port_changed), state);
  attach_row(page, 1, ftp_port_setting_key(), state->ftp_port_entry);

  state->ftp_username_entry =
      create_entry(widget_id(state, "ftp_username_entry"));
  gtk_widget_set_sensitive(state->ftp_username_entry, endpoint_sensitive);
  g_signal_connect(state->ftp_username_entry, "changed",
                   G_CALLBACK(on_ftp_username_changed), state);
  attach_row(page, 2, ftp_username_setting_key(), state->ftp_username_entry);

  state->ftp_data_connection_mode_combo = create_combo_box(
      widget_id(state, "ftp_data_connection_mode_combo").c_str());
  gtk_widget_set_sensitive(state->ftp_data_connection_mode_combo,
                           endpoint_sensitive);
  g_signal_connect(state->ftp_data_connection_mode_combo, "changed",
                   G_CALLBACK(on_ftp_data_connection_mode_changed), state);
  attach_row(page, 3, ftp_data_connection_mode_setting_key(),
             state->ftp_data_connection_mode_combo);

  state->ftp_local_directory_entry =
      create_entry(widget_id(state, "ftp_local_directory_entry"));
  g_signal_connect(state->ftp_local_directory_entry, "changed",
                   G_CALLBACK(on_ftp_local_directory_changed), state);
  attach_row(page, 4, ftp_local_directory_setting_key(),
             state->ftp_local_directory_entry);

  state->ftp_remote_directory_entry =
      create_entry(widget_id(state, "ftp_remote_directory_entry"));
  g_signal_connect(state->ftp_remote_directory_entry, "changed",
                   G_CALLBACK(on_ftp_remote_directory_changed), state);
  attach_row(page, 5, ftp_remote_directory_setting_key(),
             state->ftp_remote_directory_entry);

  return page;
}

static GtkWidget *create_serial_page(SettingsWidgetState *state) {
  const std::string page_id = widget_id(state, "serial_page");
  GtkWidget *page = create_page_grid(page_id.c_str());
  const gboolean device_sensitive = state->is_runtime ? FALSE : TRUE;

  const std::string match_mode_id =
      widget_id(state, "serial_device_match_mode_combo");
  state->serial_device_match_mode_combo =
      create_combo_box(match_mode_id.c_str());
  gtk_widget_set_sensitive(state->serial_device_match_mode_combo,
                           device_sensitive);
  g_signal_connect(state->serial_device_match_mode_combo, "changed",
                   G_CALLBACK(on_serial_device_match_mode_changed), state);
  attach_row(page, 0, serial_device_match_mode_setting_key(),
             state->serial_device_match_mode_combo);

  const std::string device_id = widget_id(state, "serial_device_combo");
  state->serial_device_combo = create_combo_box(device_id.c_str());
  gtk_widget_set_sensitive(state->serial_device_combo, device_sensitive);
  g_signal_connect(state->serial_device_combo, "changed",
                   G_CALLBACK(on_serial_device_changed), state);
  g_signal_connect(state->serial_device_combo, "notify::popup-shown",
                   G_CALLBACK(on_serial_device_popup_shown_changed), state);
  attach_row(page, 1, serial_device_setting_key(),
             state->serial_device_combo);

  state->serial_stable_id_value = create_metadata_value_label(
      widget_id(state, "serial_stable_id_value"));
  attach_text_row(page, 2,
                  settings_ui_text(SettingsUiText::serial_stable_id),
                  state->serial_stable_id_value);

  state->serial_usb_serial_value = create_metadata_value_label(
      widget_id(state, "serial_usb_serial_value"));
  attach_text_row(page, 3,
                  settings_ui_text(SettingsUiText::serial_usb_serial),
                  state->serial_usb_serial_value);

  state->serial_current_node_value = create_metadata_value_label(
      widget_id(state, "serial_current_node_value"));
  attach_text_row(page, 4,
                  settings_ui_text(SettingsUiText::serial_current_node),
                  state->serial_current_node_value);

  state->serial_baudrate_entry =
      create_entry(widget_id(state, "serial_baudrate_entry"));
  g_signal_connect(state->serial_baudrate_entry, "changed",
                   G_CALLBACK(on_serial_baudrate_changed), state);
  attach_row(page, 5, serial_baudrate_setting_key(),
             state->serial_baudrate_entry);

  const std::string bits_id = widget_id(state, "serial_bits_combo");
  state->serial_bits_combo = create_combo_box(bits_id.c_str());
  g_signal_connect(state->serial_bits_combo, "changed",
                   G_CALLBACK(on_serial_bits_changed), state);
  attach_row(page, 6, serial_bits_setting_key(), state->serial_bits_combo);

  const std::string parity_id = widget_id(state, "serial_parity_combo");
  state->serial_parity_combo = create_combo_box(parity_id.c_str());
  g_signal_connect(state->serial_parity_combo, "changed",
                   G_CALLBACK(on_serial_parity_changed), state);
  attach_row(page, 7, serial_parity_setting_key(),
             state->serial_parity_combo);

  const std::string stop_bit_id =
      widget_id(state, "serial_stop_bit_combo");
  state->serial_stop_bit_combo = create_combo_box(stop_bit_id.c_str());
  g_signal_connect(state->serial_stop_bit_combo, "changed",
                   G_CALLBACK(on_serial_stop_bit_changed), state);
  attach_row(page, 8, serial_stop_bit_setting_key(),
             state->serial_stop_bit_combo);

  const std::string flow_control_id =
      widget_id(state, "serial_flow_control_combo");
  state->serial_flow_control_combo =
      create_combo_box(flow_control_id.c_str());
  g_signal_connect(state->serial_flow_control_combo, "changed",
                   G_CALLBACK(on_serial_flow_control_changed), state);
  attach_row(page, 9, serial_flow_control_setting_key(),
             state->serial_flow_control_combo);

  const std::string carrier_detect_id =
      widget_id(state, "serial_carrier_detect_combo");
  state->serial_carrier_detect_combo =
      create_combo_box(carrier_detect_id.c_str());
  g_signal_connect(state->serial_carrier_detect_combo, "changed",
                   G_CALLBACK(on_serial_carrier_detect_changed), state);
  attach_row(page, 10, serial_carrier_detect_setting_key(),
             state->serial_carrier_detect_combo);

  return page;
}

static GtkWidget *create_macro_page(SettingsWidgetState *state) {
  const std::string page_id = widget_id(state, "macro_page");
  GtkWidget *page = create_page_grid(page_id.c_str());
  GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_set_hexpand(paned, TRUE);
  gtk_widget_set_vexpand(paned, TRUE);
  gtk_grid_attach(GTK_GRID(page), paned, 0, 0, 2, 1);

  GtkWidget *rule_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_size_request(rule_panel, 190, -1);
  GtkWidget *rule_label =
      create_row_label(settings_ui_text(SettingsUiText::macro_rules));
  gtk_box_pack_start(GTK_BOX(rule_panel), rule_label, FALSE, FALSE, 0);
  GtkWidget *rule_scroll = gtk_scrolled_window_new(nullptr, nullptr);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(rule_scroll),
                                 GTK_POLICY_AUTOMATIC,
                                 GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(rule_scroll, TRUE);
  state->macro_list = gtk_list_box_new();
  assign_accessible_id(state->macro_list,
                       widget_id(state, "macro_list").c_str());
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(state->macro_list),
                                  GTK_SELECTION_SINGLE);
  g_signal_connect(state->macro_list, "row-selected",
                   G_CALLBACK(on_macro_list_row_selected), state);
  gtk_container_add(GTK_CONTAINER(rule_scroll), state->macro_list);
  gtk_box_pack_start(GTK_BOX(rule_panel), rule_scroll, TRUE, TRUE, 0);

  GtkWidget *rule_buttons = gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(rule_buttons), 6);
  GtkWidget *add =
      gtk_button_new_with_label(settings_ui_text(SettingsUiText::macro_add));
  assign_accessible_id(add, widget_id(state, "macro_add_button").c_str());
  g_signal_connect(add, "clicked", G_CALLBACK(on_macro_add_clicked), state);
  state->macro_remove_button = gtk_button_new_with_label(
      settings_ui_text(SettingsUiText::macro_remove));
  assign_accessible_id(state->macro_remove_button,
                       widget_id(state, "macro_remove_button").c_str());
  g_signal_connect(state->macro_remove_button, "clicked",
                   G_CALLBACK(on_macro_remove_clicked), state);
  state->macro_move_up_button = gtk_button_new_with_label(
      settings_ui_text(SettingsUiText::macro_move_up));
  assign_accessible_id(state->macro_move_up_button,
                       widget_id(state, "macro_move_up_button").c_str());
  g_signal_connect(state->macro_move_up_button, "clicked",
                   G_CALLBACK(on_macro_move_up_clicked), state);
  state->macro_move_down_button = gtk_button_new_with_label(
      settings_ui_text(SettingsUiText::macro_move_down));
  assign_accessible_id(state->macro_move_down_button,
                       widget_id(state, "macro_move_down_button").c_str());
  g_signal_connect(state->macro_move_down_button, "clicked",
                   G_CALLBACK(on_macro_move_down_clicked), state);
  gtk_grid_attach(GTK_GRID(rule_buttons), add, 0, 0, 2, 1);
  gtk_grid_attach(GTK_GRID(rule_buttons), state->macro_remove_button, 0, 1, 2,
                  1);
  gtk_grid_attach(GTK_GRID(rule_buttons), state->macro_move_up_button, 0, 2, 1,
                  1);
  gtk_grid_attach(GTK_GRID(rule_buttons), state->macro_move_down_button, 1, 2,
                  1, 1);
  gtk_box_pack_start(GTK_BOX(rule_panel), rule_buttons, FALSE, FALSE, 0);
  gtk_paned_pack1(GTK_PANED(paned), rule_panel, FALSE, FALSE);

  state->macro_editor = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(state->macro_editor), 8);
  gtk_grid_set_column_spacing(GTK_GRID(state->macro_editor), 12);
  gtk_widget_set_margin_start(state->macro_editor, 12);
  gtk_widget_set_hexpand(state->macro_editor, TRUE);
  gtk_widget_set_vexpand(state->macro_editor, TRUE);
  state->macro_id_entry =
      create_entry(widget_id(state, "macro_id_entry"));
  g_signal_connect(state->macro_id_entry, "changed",
                   G_CALLBACK(on_macro_id_changed), state);
  GtkWidget *id_label =
      create_row_label(settings_ui_text(SettingsUiText::macro_id));
  gtk_grid_attach(GTK_GRID(state->macro_editor), id_label, 0, 0, 1, 1);
  gtk_grid_attach(GTK_GRID(state->macro_editor), state->macro_id_entry, 1, 0,
                  1, 1);

  state->macro_regex_entry =
      create_entry(widget_id(state, "macro_regex_entry"));
  g_signal_connect(state->macro_regex_entry, "changed",
                   G_CALLBACK(on_macro_regex_changed), state);
  GtkWidget *regex_label =
      create_row_label(settings_ui_text(SettingsUiText::macro_regex));
  gtk_grid_attach(GTK_GRID(state->macro_editor), regex_label, 0, 1, 1, 1);
  gtk_grid_attach(GTK_GRID(state->macro_editor), state->macro_regex_entry, 1,
                  1, 1, 1);

  state->macro_action_combo = create_combo_box(
      widget_id(state, "macro_action_combo").c_str());
  append_combo_option(state->macro_action_combo, "send",
                      settings_ui_text(SettingsUiText::macro_send_action));
  append_combo_option(state->macro_action_combo, "command",
                      settings_ui_text(SettingsUiText::macro_command_action));
  g_signal_connect(state->macro_action_combo, "changed",
                   G_CALLBACK(on_macro_action_changed), state);
  GtkWidget *action_label =
      create_row_label(settings_ui_text(SettingsUiText::macro_action));
  gtk_grid_attach(GTK_GRID(state->macro_editor), action_label, 0, 2, 1, 1);
  gtk_grid_attach(GTK_GRID(state->macro_editor), state->macro_action_combo, 1,
                  2, 1, 1);

  state->macro_send_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  GtkWidget *send_label =
      create_row_label(settings_ui_text(SettingsUiText::macro_send));
  gtk_box_pack_start(GTK_BOX(state->macro_send_panel), send_label, FALSE,
                     FALSE, 0);
  GtkWidget *send_scroll = gtk_scrolled_window_new(nullptr, nullptr);
  gtk_widget_set_size_request(send_scroll, -1, 90);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(send_scroll),
                                 GTK_POLICY_AUTOMATIC,
                                 GTK_POLICY_AUTOMATIC);
  state->macro_send_view = gtk_text_view_new();
  assign_accessible_id(state->macro_send_view,
                       widget_id(state, "macro_send_text").c_str());
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(state->macro_send_view),
                              GTK_WRAP_WORD_CHAR);
  g_signal_connect(
      gtk_text_view_get_buffer(GTK_TEXT_VIEW(state->macro_send_view)),
      "changed", G_CALLBACK(on_macro_send_changed), state);
  gtk_container_add(GTK_CONTAINER(send_scroll), state->macro_send_view);
  gtk_box_pack_start(GTK_BOX(state->macro_send_panel), send_scroll, TRUE, TRUE,
                     0);
  gtk_grid_attach(GTK_GRID(state->macro_editor), state->macro_send_panel, 0, 3,
                  2, 1);

  state->macro_command_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  GtkWidget *command_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  GtkWidget *command_label =
      create_row_label(settings_ui_text(SettingsUiText::macro_command));
  state->macro_command_entry =
      create_entry(widget_id(state, "macro_command_entry"));
  gtk_widget_set_hexpand(state->macro_command_entry, TRUE);
  g_signal_connect(state->macro_command_entry, "changed",
                   G_CALLBACK(on_macro_command_changed), state);
  gtk_box_pack_start(GTK_BOX(command_row), command_label, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(command_row), state->macro_command_entry, TRUE,
                     TRUE, 0);
  gtk_box_pack_start(GTK_BOX(state->macro_command_panel), command_row, FALSE,
                     FALSE, 0);
  GtkWidget *arguments_label =
      create_row_label(settings_ui_text(SettingsUiText::macro_arguments));
  gtk_box_pack_start(GTK_BOX(state->macro_command_panel), arguments_label,
                     FALSE, FALSE, 0);
  state->macro_arguments_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  assign_accessible_id(state->macro_arguments_box,
                       widget_id(state, "macro_arguments_box").c_str());
  gtk_box_pack_start(GTK_BOX(state->macro_command_panel),
                     state->macro_arguments_box, FALSE, FALSE, 0);
  state->macro_argument_add_button = gtk_button_new_with_label(
      settings_ui_text(SettingsUiText::macro_add_argument));
  assign_accessible_id(
      state->macro_argument_add_button,
      widget_id(state, "macro_argument_add_button").c_str());
  g_signal_connect(state->macro_argument_add_button, "clicked",
                   G_CALLBACK(on_macro_argument_add_clicked), state);
  gtk_widget_set_halign(state->macro_argument_add_button, GTK_ALIGN_START);
  gtk_box_pack_start(GTK_BOX(state->macro_command_panel),
                     state->macro_argument_add_button, FALSE, FALSE, 0);
  gtk_grid_attach(GTK_GRID(state->macro_editor), state->macro_command_panel, 0,
                  3, 2, 1);
  gtk_paned_pack2(GTK_PANED(paned), state->macro_editor, TRUE, FALSE);
  gtk_widget_show_all(state->macro_send_panel);
  gtk_widget_show_all(state->macro_command_panel);
  gtk_widget_set_no_show_all(state->macro_send_panel, TRUE);
  gtk_widget_set_no_show_all(state->macro_command_panel, TRUE);
  gtk_widget_hide(state->macro_send_panel);
  gtk_widget_hide(state->macro_command_panel);
  return page;
}

static GtkWidget *create_transfer_page(SettingsWidgetState *state) {
  const std::string page_id = widget_id(state, "transfer_page");
  GtkWidget *page = create_page_grid(page_id.c_str());

  state->transfer_base_path_entry =
      create_entry(widget_id(state, "transfer_base_path_entry"));
  g_signal_connect(state->transfer_base_path_entry, "changed",
                   G_CALLBACK(on_transfer_base_path_changed), state);
  attach_row(page, 0, transfer_base_path_setting_key(),
             state->transfer_base_path_entry);

  state->transfer_text_send_rate_entry =
      create_entry(widget_id(state, "transfer_text_send_rate_entry"));
  g_signal_connect(state->transfer_text_send_rate_entry, "changed",
                   G_CALLBACK(on_transfer_text_send_rate_changed), state);
  attach_row(page, 1, transfer_text_send_bytes_per_second_setting_key(),
             state->transfer_text_send_rate_entry);

  const std::string follow_return_code_id =
      widget_id(state, "transfer_text_send_follow_return_code_combo");
  state->transfer_text_send_follow_return_code_combo =
      create_combo_box(follow_return_code_id.c_str());
  g_signal_connect(
      state->transfer_text_send_follow_return_code_combo, "changed",
      G_CALLBACK(on_transfer_text_send_follow_return_code_changed), state);
  attach_row(page, 2,
             transfer_text_send_follow_return_code_setting_key(),
             state->transfer_text_send_follow_return_code_combo);

  const std::string zmodem_id =
      widget_id(state, "transfer_zmodem_autostart_combo");
  state->transfer_zmodem_autostart_combo =
      create_combo_box(zmodem_id.c_str());
  g_signal_connect(state->transfer_zmodem_autostart_combo, "changed",
                   G_CALLBACK(on_transfer_zmodem_autostart_changed), state);
  attach_row(page, 3, transfer_zmodem_autostart_setting_key(),
             state->transfer_zmodem_autostart_combo);

  return page;
}

static GtkWidget *create_logging_page(SettingsWidgetState *state) {
  const std::string page_id = widget_id(state, "logging_page");
  GtkWidget *page = create_page_grid(page_id.c_str());

  const std::string enabled_id = widget_id(state, "log_enabled_combo");
  state->log_enabled_combo = create_combo_box(enabled_id.c_str());
  g_signal_connect(state->log_enabled_combo, "changed",
                   G_CALLBACK(on_log_enabled_changed), state);
  attach_row(page, 0, terminal_log_enabled_setting_key(),
             state->log_enabled_combo);

  state->log_base_directory_entry =
      create_entry(widget_id(state, "log_base_directory_entry"));
  g_signal_connect(state->log_base_directory_entry, "changed",
                   G_CALLBACK(on_log_base_directory_changed), state);
  attach_row(page, 1, terminal_log_base_directory_setting_key(),
             state->log_base_directory_entry);

  state->log_file_name_format_entry =
      create_entry(widget_id(state, "log_file_name_format_entry"));
  g_signal_connect(state->log_file_name_format_entry, "changed",
                   G_CALLBACK(on_log_file_name_format_changed), state);
  attach_row(page, 2, terminal_log_file_name_format_setting_key(),
             state->log_file_name_format_entry);

  const std::string mode_id = widget_id(state, "log_mode_combo");
  state->log_mode_combo = create_combo_box(mode_id.c_str());
  g_signal_connect(state->log_mode_combo, "changed",
                   G_CALLBACK(on_log_mode_changed), state);
  attach_row(page, 3, terminal_log_mode_setting_key(),
             state->log_mode_combo);

  return page;
}

static GtkWidget *create_button_box(SettingsWidgetState *state) {
  GtkWidget *panel =
      gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *button_box = gtk_button_box_new(GTK_ORIENTATION_HORIZONTAL);
  const std::string action_row_id = widget_id(state, "action_row");
  assign_accessible_id(button_box, action_row_id.c_str());
  gtk_button_box_set_layout(GTK_BUTTON_BOX(button_box), GTK_BUTTONBOX_END);
  gtk_box_set_spacing(GTK_BOX(button_box), 8);
  gtk_widget_set_margin_top(button_box, 12);
  gtk_widget_set_margin_bottom(button_box, 12);
  gtk_widget_set_margin_start(button_box, 12);
  gtk_widget_set_margin_end(button_box, 12);
  gtk_widget_set_valign(button_box, GTK_ALIGN_CENTER);

  state->apply_button =
      gtk_button_new_with_label(settings_ui_text(SettingsUiText::apply));
  const std::string apply_id = widget_id(state, "apply_button");
  assign_accessible_id(state->apply_button, apply_id.c_str());
  gtk_widget_set_valign(state->apply_button, GTK_ALIGN_CENTER);
  g_signal_connect(state->apply_button, "clicked",
                   G_CALLBACK(on_apply_clicked), state);
  gtk_container_add(GTK_CONTAINER(button_box), state->apply_button);

  if (state->callbacks.save) {
    state->save_button =
        gtk_button_new_with_label(settings_ui_text(SettingsUiText::save));
    const std::string save_id = widget_id(state, "save_button");
    assign_accessible_id(state->save_button, save_id.c_str());
    gtk_widget_set_valign(state->save_button, GTK_ALIGN_CENTER);
    g_signal_connect(state->save_button, "clicked",
                     G_CALLBACK(on_save_clicked), state);
    gtk_container_add(GTK_CONTAINER(button_box), state->save_button);
  }

  state->cancel_button =
      gtk_button_new_with_label(settings_ui_text(SettingsUiText::cancel));
  const std::string cancel_id = widget_id(state, "cancel_button");
  assign_accessible_id(state->cancel_button, cancel_id.c_str());
  gtk_widget_set_valign(state->cancel_button, GTK_ALIGN_CENTER);
  g_signal_connect(state->cancel_button, "clicked",
                   G_CALLBACK(on_cancel_clicked), state);
  gtk_container_add(GTK_CONTAINER(button_box), state->cancel_button);

  gtk_box_pack_start(
      GTK_BOX(panel), button_box, TRUE, TRUE, 0);
  return panel;
}

SettingsWidgetState *create_settings_widget(SettingsWidgetOptions options) {
  auto *state = new SettingsWidgetState();
  state->applied_store = options.store;
  state->draft_store = std::move(options.store);
  state->mode = options.mode;
  state->id_prefix =
      options.id_prefix.empty() ? std::string("settings")
                                : std::move(options.id_prefix);
  state->is_runtime = options.is_runtime;
  state->show_actions = options.show_actions;
  state->callbacks = std::move(options.callbacks);
  state->synchronizing = true;

  state->root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  const std::string root_id = widget_id(state, "widget_root");
  assign_accessible_id(state->root, root_id.c_str());

  state->notebook = gtk_notebook_new();
  const std::string notebook_id = widget_id(state, "notebook");
  assign_accessible_id(state->notebook, notebook_id.c_str());
  gtk_widget_set_vexpand(state->notebook, TRUE);
  gtk_widget_set_hexpand(state->notebook, TRUE);
  gtk_box_pack_start(GTK_BOX(state->root), state->notebook, TRUE, TRUE, 0);

  GtkWidget *general_page = create_general_page(state);
  const std::string general_tab_id = widget_id(state, "general_tab");
  GtkWidget *general_tab = create_tab_button(
      state, general_page, settings_ui_text(SettingsUiText::general_tab),
      general_tab_id.c_str());
  gtk_notebook_append_page(GTK_NOTEBOOK(state->notebook), general_page,
                           general_tab);
  gtk_widget_show(general_page);
  gtk_widget_show(general_tab);

  GtkWidget *local_page = create_local_page(state);
  const std::string local_tab_id = widget_id(state, "local_tab");
  GtkWidget *local_tab = create_tab_button(
      state, local_page, settings_ui_text(SettingsUiText::local_tab),
      local_tab_id.c_str());
  gtk_notebook_append_page(GTK_NOTEBOOK(state->notebook), local_page,
                           local_tab);
  gtk_widget_show_all(local_page);
  gtk_widget_show_all(local_tab);
  gtk_widget_set_no_show_all(local_page, TRUE);
  gtk_widget_set_no_show_all(local_tab, TRUE);
  state->connection_pages.push_back({
      .connection_types = {local_connection_type},
      .page = local_page,
      .tab_label = local_tab,
  });

  GtkWidget *telnet_page = create_telnet_page(state);
  const std::string telnet_tab_id = widget_id(state, "telnet_tab");
  GtkWidget *telnet_tab = create_tab_button(
      state, telnet_page, settings_ui_text(SettingsUiText::telnet_tab),
      telnet_tab_id.c_str());
  gtk_notebook_append_page(GTK_NOTEBOOK(state->notebook), telnet_page,
                           telnet_tab);
  gtk_widget_show_all(telnet_page);
  gtk_widget_show_all(telnet_tab);
  gtk_widget_set_no_show_all(telnet_page, TRUE);
  gtk_widget_set_no_show_all(telnet_tab, TRUE);
  state->connection_pages.push_back({
      .connection_types = {telnet_connection_type},
      .page = telnet_page,
      .tab_label = telnet_tab,
  });

  GtkWidget *serial_page = create_serial_page(state);
  const std::string serial_tab_id = widget_id(state, "serial_tab");
  GtkWidget *serial_tab = create_tab_button(
      state, serial_page, settings_ui_text(SettingsUiText::serial_tab),
      serial_tab_id.c_str());
  gtk_notebook_append_page(GTK_NOTEBOOK(state->notebook), serial_page,
                           serial_tab);
  gtk_widget_show_all(serial_page);
  gtk_widget_show_all(serial_tab);
  gtk_widget_set_no_show_all(serial_page, TRUE);
  gtk_widget_set_no_show_all(serial_tab, TRUE);
  state->connection_pages.push_back({
      .connection_types = {serial_connection_type},
      .page = serial_page,
      .tab_label = serial_tab,
  });

  GtkWidget *ssh_page = create_ssh_page(state);
  const std::string ssh_tab_id = widget_id(state, "ssh_tab");
  GtkWidget *ssh_tab = create_tab_button(
      state, ssh_page, settings_ui_text(SettingsUiText::ssh_tab),
      ssh_tab_id.c_str());
  gtk_notebook_append_page(GTK_NOTEBOOK(state->notebook), ssh_page, ssh_tab);
  gtk_widget_show_all(ssh_page);
  gtk_widget_show_all(ssh_tab);
  gtk_widget_set_no_show_all(ssh_page, TRUE);
  gtk_widget_set_no_show_all(ssh_tab, TRUE);
  state->connection_pages.push_back({
      .connection_types = {ssh_connection_type, sftp_connection_type},
      .page = ssh_page,
      .tab_label = ssh_tab,
  });

  GtkWidget *sftp_page = create_sftp_page(state);
  const std::string sftp_tab_id = widget_id(state, "sftp_tab");
  GtkWidget *sftp_tab = create_tab_button(
      state, sftp_page, settings_ui_text(SettingsUiText::sftp_tab),
      sftp_tab_id.c_str());
  gtk_notebook_append_page(GTK_NOTEBOOK(state->notebook), sftp_page, sftp_tab);
  gtk_widget_show_all(sftp_page);
  gtk_widget_show_all(sftp_tab);
  gtk_widget_set_no_show_all(sftp_page, TRUE);
  gtk_widget_set_no_show_all(sftp_tab, TRUE);
  state->connection_pages.push_back({
      .connection_types = {sftp_connection_type},
      .page = sftp_page,
      .tab_label = sftp_tab,
  });

  GtkWidget *ftp_page = create_ftp_page(state);
  const std::string ftp_tab_id = widget_id(state, "ftp_tab");
  GtkWidget *ftp_tab = create_tab_button(
      state, ftp_page, settings_ui_text(SettingsUiText::ftp_tab),
      ftp_tab_id.c_str());
  gtk_notebook_append_page(GTK_NOTEBOOK(state->notebook), ftp_page, ftp_tab);
  gtk_widget_show_all(ftp_page);
  gtk_widget_show_all(ftp_tab);
  gtk_widget_set_no_show_all(ftp_page, TRUE);
  gtk_widget_set_no_show_all(ftp_tab, TRUE);
  state->connection_pages.push_back({
      .connection_types = {ftp_connection_type},
      .page = ftp_page,
      .tab_label = ftp_tab,
  });

  GtkWidget *terminal_page = create_terminal_page(state);
  const std::string terminal_tab_id = widget_id(state, "terminal_tab");
  GtkWidget *terminal_tab = create_tab_button(
      state, terminal_page, settings_ui_text(SettingsUiText::terminal_tab),
      terminal_tab_id.c_str());
  gtk_notebook_append_page(GTK_NOTEBOOK(state->notebook), terminal_page,
                           terminal_tab);
  gtk_widget_show_all(terminal_page);
  gtk_widget_show_all(terminal_tab);
  gtk_widget_set_no_show_all(terminal_page, TRUE);
  gtk_widget_set_no_show_all(terminal_tab, TRUE);
  state->connection_pages.push_back({
      .connection_types = {local_connection_type, telnet_connection_type,
                           serial_connection_type, ssh_connection_type},
      .page = terminal_page,
      .tab_label = terminal_tab,
  });

  GtkWidget *transfer_page = create_transfer_page(state);
  const std::string transfer_tab_id = widget_id(state, "transfer_tab");
  GtkWidget *transfer_tab = create_tab_button(
      state, transfer_page, settings_ui_text(SettingsUiText::transfer_tab),
      transfer_tab_id.c_str());
  gtk_notebook_append_page(GTK_NOTEBOOK(state->notebook), transfer_page,
                           transfer_tab);
  gtk_widget_show_all(transfer_page);
  gtk_widget_show_all(transfer_tab);
  gtk_widget_set_no_show_all(transfer_page, TRUE);
  gtk_widget_set_no_show_all(transfer_tab, TRUE);
  state->connection_pages.push_back({
      .connection_types = {local_connection_type, telnet_connection_type,
                           serial_connection_type, ssh_connection_type},
      .page = transfer_page,
      .tab_label = transfer_tab,
  });

  GtkWidget *logging_page = create_logging_page(state);
  const std::string logging_tab_id = widget_id(state, "logging_tab");
  GtkWidget *logging_tab = create_tab_button(
      state, logging_page, settings_ui_text(SettingsUiText::logging_tab),
      logging_tab_id.c_str());
  gtk_notebook_append_page(GTK_NOTEBOOK(state->notebook), logging_page,
                           logging_tab);
  gtk_widget_show_all(logging_page);
  gtk_widget_show_all(logging_tab);
  gtk_widget_set_no_show_all(logging_page, TRUE);
  gtk_widget_set_no_show_all(logging_tab, TRUE);
  state->connection_pages.push_back({
      .connection_types = {local_connection_type, telnet_connection_type,
                           serial_connection_type, ssh_connection_type},
      .page = logging_page,
      .tab_label = logging_tab,
  });

  if (state->mode == SettingsWidgetMode::connection) {
    GtkWidget *macro_page = create_macro_page(state);
    const std::string macro_tab_id = widget_id(state, "macro_tab");
    GtkWidget *macro_tab = create_tab_button(
        state, macro_page, settings_ui_text(SettingsUiText::macro_tab),
        macro_tab_id.c_str());
    gtk_notebook_append_page(GTK_NOTEBOOK(state->notebook), macro_page,
                             macro_tab);
    gtk_widget_show_all(macro_page);
    gtk_widget_show_all(macro_tab);
    gtk_widget_set_no_show_all(macro_page, TRUE);
    gtk_widget_set_no_show_all(macro_tab, TRUE);
    state->connection_pages.push_back({
        .connection_types = {local_connection_type, telnet_connection_type,
                             serial_connection_type, ssh_connection_type},
        .page = macro_page,
        .tab_label = macro_tab,
    });
  }

  if (state->show_actions) {
    gtk_box_pack_start(GTK_BOX(state->root), create_button_box(state), FALSE,
                       FALSE, 0);
  }
  sync_widgets_from_draft(state);
  update_terminal_key_binding_validation(state);
  update_connection_pages(state);
  gtk_notebook_set_current_page(GTK_NOTEBOOK(state->notebook), 0);
  state->synchronizing = false;

  SerialDeviceEventMonitorOptions monitor_options;
  monitor_options.paths = host_serial_device_paths();
  state->serial_device_event_monitor =
      std::make_unique<SerialDeviceEventMonitor>(
          std::move(monitor_options),
          [state]() { refresh_serial_device_widgets(state); });
  state->serial_device_event_monitor->start();

  return state;
}

void update_settings_widget_store(SettingsWidgetState *state,
                                  SettingsStore store) {
  if (state == nullptr) {
    return;
  }

  state->applied_store = store;
  state->draft_store = std::move(store);
  state->synchronizing = true;
  sync_widgets_from_draft(state);
  update_terminal_key_binding_validation(state);
  state->synchronizing = false;
  notify_changed(state);
}

void settings_widget_rebase_fallbacks(
    SettingsWidgetState *state, const SettingsStore &fallbacks) {
  if (state == nullptr) {
    return;
  }

  const bool local_command_line_invalid =
      !state->local_command_line_valid;
  const bool width_invalid = !state->terminal_width_valid;
  const bool height_invalid = !state->terminal_height_valid;
  const bool scrollback_lines_invalid =
      !state->terminal_scrollback_lines_valid;
  const bool border_width_invalid = !state->terminal_border_width_valid;
  const bool zoom_invalid = !state->terminal_zoom_valid;
  const bool encoding_invalid = !state->terminal_encoding_valid;
  const bool bell_sound_invalid = !state->terminal_bell_sound_valid;
  const bool telnet_port_invalid = !state->telnet_port_valid;
  const bool ssh_port_invalid = !state->ssh_port_valid;
  const bool baudrate_invalid = !state->serial_baudrate_valid;
  const bool send_rate_invalid = !state->transfer_text_send_rate_valid;
  const bool log_format_invalid = !state->log_file_name_format_valid;
  const auto entry_text = [](GtkWidget *entry) {
    const char *text = gtk_entry_get_text(GTK_ENTRY(entry));
    return std::string(text == nullptr ? "" : text);
  };
  const std::string local_command_line_text =
      entry_text(state->local_command_line_entry);
  const std::string width_text = entry_text(state->terminal_width_entry);
  const std::string height_text = entry_text(state->terminal_height_entry);
  const std::string scrollback_lines_text =
      entry_text(state->terminal_scrollback_lines_entry);
  const std::string border_width_text =
      entry_text(state->terminal_border_width_entry);
  const std::string zoom_text = entry_text(state->terminal_zoom_entry);
  const std::string encoding_text =
      entry_text(state->terminal_encoding_entry);
  const std::string bell_sound_text =
      entry_text(state->terminal_bell_sound_entry);
  const std::string telnet_port_text =
      entry_text(state->telnet_port_entry);
  const std::string ssh_port_text = entry_text(state->ssh_port_entry);
  const std::string baudrate_text =
      entry_text(state->serial_baudrate_entry);
  const std::string send_rate_text =
      entry_text(state->transfer_text_send_rate_entry);
  const std::string log_format_text =
      entry_text(state->log_file_name_format_entry);

  rebase_settings_store_fallbacks(&state->applied_store, fallbacks);
  rebase_settings_store_fallbacks(&state->draft_store, fallbacks);
  state->synchronizing = true;
  sync_widgets_from_draft(state);

  const auto restore_invalid =
      [](GtkWidget *entry, const std::string &text, bool invalid,
         const auto &update) {
        if (!invalid) {
          return;
        }
        gtk_entry_set_text(GTK_ENTRY(entry), text.c_str());
        update();
      };
  restore_invalid(state->local_command_line_entry,
                  local_command_line_text,
                  local_command_line_invalid, [state]() {
                    update_local_command_line_from_widget(state);
                  });
  restore_invalid(state->terminal_width_entry, width_text, width_invalid,
                  [state]() { update_terminal_width_from_widget(state); });
  restore_invalid(state->terminal_height_entry, height_text, height_invalid,
                  [state]() { update_terminal_height_from_widget(state); });
  restore_invalid(
      state->terminal_scrollback_lines_entry, scrollback_lines_text,
      scrollback_lines_invalid,
      [state]() { update_terminal_scrollback_lines_from_widget(state); });
  restore_invalid(state->terminal_border_width_entry, border_width_text,
                  border_width_invalid, [state]() {
                    update_terminal_border_width_from_widget(state);
                  });
  restore_invalid(state->terminal_zoom_entry, zoom_text, zoom_invalid,
                  [state]() { update_terminal_zoom_from_widget(state); });
  if (encoding_invalid) {
    gtk_combo_box_set_active(GTK_COMBO_BOX(state->terminal_encoding_combo),
                             -1);
  }
  restore_invalid(state->terminal_encoding_entry, encoding_text,
                  encoding_invalid, [state]() {
                    update_terminal_encoding_from_widget(state);
                  });
  restore_invalid(state->terminal_bell_sound_entry, bell_sound_text,
                  bell_sound_invalid, [state]() {
                    update_terminal_bell_sound_from_widget(state);
                  });
  restore_invalid(state->telnet_port_entry, telnet_port_text,
                  telnet_port_invalid,
                  [state]() { update_telnet_port_from_widget(state); });
  restore_invalid(state->ssh_port_entry, ssh_port_text, ssh_port_invalid,
                  [state]() { update_ssh_port_from_widget(state); });
  restore_invalid(state->serial_baudrate_entry, baudrate_text,
                  baudrate_invalid,
                  [state]() { update_serial_baudrate_from_widget(state); });
  restore_invalid(state->transfer_text_send_rate_entry, send_rate_text,
                  send_rate_invalid, [state]() {
                    update_transfer_text_send_rate_from_widget(state);
                  });
  restore_invalid(state->log_file_name_format_entry, log_format_text,
                  log_format_invalid, [state]() {
                    update_log_file_name_format_from_widget(state);
                  });
  update_terminal_key_binding_validation(state);
  update_action_sensitivity(state);
  state->synchronizing = false;
  notify_changed(state);
}

static void update_default_connection_name(SettingsStore *store,
                                           const std::string &name) {
  const SettingKey key = general_name_setting_key();
  for (SettingEntry &entry : store->entries) {
    if (entry.definition.key.section != key.section ||
        entry.definition.key.name != key.name) {
      continue;
    }
    entry.definition.default_value = SettingValue{name};
    entry.fallback_value = SettingValue{name};
    if (!entry.loaded) {
      entry.value = SettingValue{name};
    }
    return;
  }
}

void settings_widget_set_default_connection_name(
    SettingsWidgetState *state, std::string default_connection_name) {
  if (state == nullptr || ascii_blank(default_connection_name)) {
    return;
  }

  update_default_connection_name(&state->applied_store,
                                 default_connection_name);
  update_default_connection_name(&state->draft_store,
                                 default_connection_name);
  if (state->general_name_entry == nullptr) {
    return;
  }

  const bool previous_synchronizing = state->synchronizing;
  state->synchronizing = true;
  gtk_entry_set_text(GTK_ENTRY(state->general_name_entry),
                     general_connection_name(state->draft_store).c_str());
  state->synchronizing = previous_synchronizing;
}

SettingsStore settings_widget_draft_store(const SettingsWidgetState *state) {
  return state == nullptr ? SettingsStore{} : state->draft_store;
}

bool settings_widget_is_dirty(const SettingsWidgetState *state) {
  return state != nullptr &&
         (settings_store_is_dirty(state->draft_store) ||
          !settings_inputs_valid(state));
}

bool settings_widget_is_valid(const SettingsWidgetState *state) {
  return state != nullptr && settings_inputs_valid(state);
}

GtkWidget *settings_widget_root(SettingsWidgetState *state) {
  return state == nullptr ? nullptr : state->root;
}

void destroy_settings_widget(SettingsWidgetState *state) {
  if (state == nullptr) {
    return;
  }

  state->serial_device_event_monitor.reset();
  if (state->terminal_bell_sound_dialog != nullptr) {
    gtk_widget_destroy(state->terminal_bell_sound_dialog);
  }
  destroy_key_binding_input_widget(state->terminal_zoom_in_key_input);
  destroy_key_binding_input_widget(state->terminal_zoom_out_key_input);
  destroy_key_binding_input_widget(state->terminal_send_break_key_input);
  destroy_key_binding_input_widget(
      state->general_open_connection_input);
  if (state->root != nullptr && gtk_widget_get_parent(state->root) == nullptr) {
    gtk_widget_destroy(state->root);
  }
  delete state;
}

} // namespace elder_terms
