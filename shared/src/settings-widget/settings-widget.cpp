#include <elder-terms/settings-widget.h>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <atk/atk.h>

#include <elder-terms/key-binding-input-widget.h>
#include <elder-terms/settings/general-settings.h>

namespace elder_terms {

static constexpr char local_connection_type[] = "local";
static constexpr char telnet_connection_type[] = "telnet";
static constexpr char serial_connection_type[] = "serial";
static constexpr char ssh_connection_type[] = "ssh";
static constexpr char sftp_connection_type[] = "sftp";
static constexpr char zmodem_autostart_enabled[] = "enabled";
static constexpr char zmodem_autostart_disabled[] = "disabled";
static constexpr char terminal_text_default[] = "default";
static constexpr char terminal_backspace_bs[] = "bs";
static constexpr char terminal_backspace_del[] = "del";
static constexpr char terminal_cursor_normal[] = "normal";
static constexpr char terminal_cursor_adm3[] = "adm3";
static constexpr char terminal_log_raw[] = "raw";
static constexpr char terminal_log_cooked[] = "cooked";
static constexpr char inherit_choice[] = "inherit";
static constexpr char boolean_enabled[] = "enabled";
static constexpr char boolean_disabled[] = "disabled";
static constexpr char startup_window[] = "window";
static constexpr char startup_tray[] = "tray";
static constexpr char startup_window_and_tray[] = "window_and_tray";

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
  GtkWidget *general_startup_mode_combo = nullptr;
  KeyBindingInputWidgetState *general_open_application_input = nullptr;
  GtkWidget *general_open_application_reset_button = nullptr;
  GtkWidget *terminal_width_entry = nullptr;
  GtkWidget *terminal_height_entry = nullptr;
  GtkWidget *terminal_zoom_entry = nullptr;
  GtkWidget *terminal_auto_close_combo = nullptr;
  GtkWidget *terminal_encoding_combo = nullptr;
  GtkWidget *terminal_encoding_entry = nullptr;
  GtkWidget *terminal_backspace_code_combo = nullptr;
  GtkWidget *terminal_cursor_key_mode_combo = nullptr;
  bool terminal_width_valid = true;
  bool terminal_height_valid = true;
  bool terminal_zoom_valid = true;
  bool terminal_encoding_valid = true;
  KeyBindingInputWidgetState *terminal_zoom_in_key_input = nullptr;
  KeyBindingInputWidgetState *terminal_zoom_out_key_input = nullptr;
  GtkWidget *terminal_zoom_in_key_reset_button = nullptr;
  GtkWidget *terminal_zoom_out_key_reset_button = nullptr;
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
  GtkWidget *serial_device_entry = nullptr;
  GtkWidget *serial_baudrate_entry = nullptr;
  GtkWidget *serial_bits_combo = nullptr;
  GtkWidget *serial_parity_combo = nullptr;
  GtkWidget *serial_stop_bit_combo = nullptr;
  GtkWidget *serial_flow_control_combo = nullptr;
  GtkWidget *serial_carrier_detect_combo = nullptr;
  bool serial_baudrate_valid = true;
  GtkWidget *transfer_base_path_entry = nullptr;
  GtkWidget *transfer_text_send_rate_entry = nullptr;
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

static GtkWidget *attach_row(GtkWidget *grid, int row,
                             const char *label_text, GtkWidget *control) {
  GtkWidget *label = create_row_label(label_text);
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

static GtkWidget *create_combo_box(const char *id) {
  GtkWidget *combo = gtk_combo_box_text_new();
  assign_accessible_id(combo, id);
  return combo;
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

static const char *source_name(SettingValueSource source) {
  return source == SettingValueSource::global ? "global" : "built-in";
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
  return std::get<bool>(value) ? "Enabled" : "Disabled";
}

static std::string inherited_label(const std::string &value,
                                   SettingValueSource source) {
  if (value.empty()) {
    return source == SettingValueSource::global ? "Global default"
                                                : "Built-in default";
  }
  return value + " (" + source_name(source) + ")";
}

static std::string setting_fallback_label(const SettingsStore &store,
                                          const SettingKey &key,
                                          const std::string &display_value) {
  return inherited_label(display_value, setting_fallback_source(store, key));
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
  gtk_entry_set_icon_tooltip_text(
      GTK_ENTRY(entry), GTK_ENTRY_ICON_SECONDARY,
      valid ? nullptr : reason.c_str());
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

static std::string terminal_backspace_label(const std::string &value) {
  return value == terminal_backspace_bs ? "BS" : "DEL";
}

static std::string terminal_cursor_label(const std::string &value) {
  return value == terminal_cursor_adm3 ? "ADM3" : "Normal";
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
        terminal_backspace_label(fallback));
    append_combo_option(state->terminal_backspace_code_combo,
                        terminal_text_default, default_label.c_str());
    append_combo_option(state->terminal_backspace_code_combo,
                        terminal_backspace_bs, "BS");
    append_combo_option(state->terminal_backspace_code_combo,
                        terminal_backspace_del, "DEL");
    const char *active = terminal_text_default;
    if (setting_has_explicit_value(
            state->draft_store, terminal_backspace_code_setting_key())) {
      active = setting_string_value_or_default(
                   state->draft_store, terminal_backspace_code_setting_key(),
                   fallback) ==
                       terminal_backspace_bs
                   ? terminal_backspace_bs
                   : terminal_backspace_del;
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
        terminal_cursor_label(fallback));
    append_combo_option(state->terminal_cursor_key_mode_combo,
                        terminal_text_default, default_label.c_str());
    append_combo_option(state->terminal_cursor_key_mode_combo,
                        terminal_cursor_normal, "Normal");
    append_combo_option(state->terminal_cursor_key_mode_combo,
                        terminal_cursor_adm3, "ADM3");
    const char *active = terminal_text_default;
    if (setting_has_explicit_value(
            state->draft_store, terminal_cursor_key_mode_setting_key())) {
      active = setting_string_value_or_default(
                   state->draft_store, terminal_cursor_key_mode_setting_key(),
                   fallback) == terminal_cursor_adm3
                   ? terminal_cursor_adm3
                   : terminal_cursor_normal;
    }
    gtk_combo_box_set_active_id(
        GTK_COMBO_BOX(state->terminal_cursor_key_mode_combo), active);
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
update_application_startup_mode_from_widget(SettingsWidgetState *state) {
  const std::string mode =
      active_combo_id(state->general_startup_mode_combo, inherit_choice);
  if (mode == inherit_choice) {
    clear_explicit_setting_value(
        &state->draft_store, application_startup_mode_setting_key());
    return;
  }
  set_explicit_setting_value(
      &state->draft_store, application_startup_mode_setting_key(),
      SettingValue{mode});
}

static void
update_application_hotkey_from_widget(SettingsWidgetState *state) {
  const std::string text =
      key_binding_input_widget_text(state->general_open_application_input);
  std::string reason;
  const bool valid = application_hotkey_text_is_valid(text, &reason);
  set_key_binding_input_widget_external_error(
      state->general_open_application_input, valid ? std::string() : reason);
  if (!valid) {
    return;
  }
  set_explicit_setting_value(
      &state->draft_store, application_open_hotkey_setting_key(),
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

static void set_terminal_encoding_validation(
    SettingsWidgetState *state, bool valid, const std::string &reason) {
  state->terminal_encoding_valid = valid;
  if (state->terminal_encoding_entry == nullptr) {
    return;
  }
  gtk_entry_set_icon_from_icon_name(
      GTK_ENTRY(state->terminal_encoding_entry), GTK_ENTRY_ICON_SECONDARY,
      valid ? nullptr : "dialog-error-symbolic");
  gtk_entry_set_icon_tooltip_text(
      GTK_ENTRY(state->terminal_encoding_entry), GTK_ENTRY_ICON_SECONDARY,
      valid ? nullptr : reason.c_str());
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

static bool terminal_key_binding_inputs_conflict(
    SettingsWidgetState *state) {
  const std::string zoom_in_text = setting_has_explicit_value(
                                       state->draft_store,
                                       terminal_zoom_in_key_setting_key())
                                       ? key_binding_input_widget_text(
                                             state->terminal_zoom_in_key_input)
                                       : terminal_zoom_in_key(
                                             state->draft_store);
  const std::string zoom_out_text = setting_has_explicit_value(
                                        state->draft_store,
                                        terminal_zoom_out_key_setting_key())
                                        ? key_binding_input_widget_text(
                                              state->terminal_zoom_out_key_input)
                                        : terminal_zoom_out_key(
                                              state->draft_store);
  const KeyBindingParseResult zoom_in = parse_key_binding(
      zoom_in_text);
  const KeyBindingParseResult zoom_out = parse_key_binding(
      zoom_out_text);
  return zoom_in.error.empty() && zoom_out.error.empty() &&
         zoom_in.binding.has_value() && zoom_out.binding.has_value() &&
         key_bindings_equal(*zoom_in.binding, *zoom_out.binding);
}

static bool terminal_key_binding_inputs_valid(
    const SettingsWidgetState *state) {
  return key_binding_input_widget_is_valid(
             state->terminal_zoom_in_key_input) &&
         key_binding_input_widget_is_valid(
             state->terminal_zoom_out_key_input);
}

static bool application_hotkey_input_valid(
    const SettingsWidgetState *state) {
  return state->general_open_application_input == nullptr ||
         key_binding_input_widget_is_valid(
             state->general_open_application_input);
}

static bool settings_inputs_valid(const SettingsWidgetState *state) {
  return state->terminal_width_valid && state->terminal_height_valid &&
         state->terminal_zoom_valid && state->terminal_encoding_valid &&
         state->telnet_port_valid && state->ssh_port_valid &&
         state->serial_baudrate_valid &&
         state->transfer_text_send_rate_valid &&
         terminal_key_binding_inputs_valid(state) &&
         application_hotkey_input_valid(state) &&
         state->log_file_name_format_valid;
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
      "Zoom in and zoom out must use different key bindings";
  const bool conflict = terminal_key_binding_inputs_conflict(state);
  set_key_binding_input_widget_external_error(
      state->terminal_zoom_in_key_input,
      conflict ? conflict_error : std::string());
  set_key_binding_input_widget_external_error(
      state->terminal_zoom_out_key_input,
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

static void update_serial_device_from_widget(SettingsWidgetState *state) {
  update_string_entry(state, state->serial_device_entry,
                      serial_device_setting_key());
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
  gtk_entry_set_icon_tooltip_text(
      GTK_ENTRY(state->log_file_name_format_entry), GTK_ENTRY_ICON_SECONDARY,
      valid ? nullptr : reason.c_str());
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

struct ComboOption {
  const char *id;
  const char *label;
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
    append_combo_option(combo, option.id, option.label);
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
      combo, store, key, fallback ? "Enabled" : "Disabled",
      {
          {.id = boolean_enabled, .label = "Enabled"},
          {.id = boolean_disabled, .label = "Disabled"},
      },
      effective_value ? boolean_enabled : boolean_disabled);
}

static std::string connection_type_label(const std::string &type) {
  if (type == telnet_connection_type) {
    return "TELNET";
  }
  if (type == serial_connection_type) {
    return "Serial";
  }
  if (type == ssh_connection_type) {
    return "SSH";
  }
  if (type == sftp_connection_type) {
    return "SFTP";
  }
  return "Local";
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
          {.id = local_connection_type, .label = "Local"},
          {.id = telnet_connection_type, .label = "TELNET"},
          {.id = serial_connection_type, .label = "Serial"},
          {.id = ssh_connection_type, .label = "SSH"},
          {.id = sftp_connection_type, .label = "SFTP"},
      },
      effective);
}

static std::string startup_mode_label(const std::string &mode) {
  if (mode == startup_tray) {
    return "System tray only";
  }
  if (mode == startup_window_and_tray) {
    return "System tray and main window";
  }
  return "Simple startup";
}

static void sync_application_startup_mode_combo(
    SettingsWidgetState *state) {
  const std::string fallback = std::get<std::string>(setting_fallback_value(
      state->draft_store, application_startup_mode_setting_key(),
      SettingValue{std::string(startup_window)}));
  const std::string effective =
      startup_mode_to_string(application_startup_mode(state->draft_store));
  populate_inheritable_combo(
      state->general_startup_mode_combo, state->draft_store,
      application_startup_mode_setting_key(), startup_mode_label(fallback),
      {
          {.id = startup_window, .label = "Simple startup"},
          {.id = startup_tray, .label = "System tray only"},
          {.id = startup_window_and_tray,
           .label = "System tray and main window"},
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
      fallback ? "Enabled" : "Disabled",
      {
          {.id = zmodem_autostart_enabled, .label = "Enabled"},
          {.id = zmodem_autostart_disabled, .label = "Disabled"},
      },
      effective ? zmodem_autostart_enabled : zmodem_autostart_disabled);
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
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Disabled");
    return;
  }
  if (explicit_value) {
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry),
                                   "Press a key combination");
    return;
  }
  const std::string placeholder =
      setting_fallback_label(state->draft_store, key, effective_value);
  gtk_entry_set_placeholder_text(GTK_ENTRY(entry), placeholder.c_str());
}

static void sync_key_binding_reset_button(
    SettingsWidgetState *state, GtkWidget *button, const SettingKey &key) {
  gtk_widget_set_sensitive(
      button, setting_has_explicit_value(state->draft_store, key));
  const char *tooltip =
      setting_fallback_source(state->draft_store, key) ==
              SettingValueSource::global
          ? "Use global default"
          : "Use built-in default";
  gtk_widget_set_tooltip_text(button, tooltip);
}

static void sync_widgets_from_draft(SettingsWidgetState *state) {
  const TerminalDisplaySettings display =
      terminal_display_settings(state->draft_store);
  const TelnetConnectionSettings telnet =
      telnet_connection_settings(state->draft_store);
  const SshConnectionSettings ssh =
      ssh_connection_settings(state->draft_store);
  const SftpConnectionSettings sftp =
      sftp_connection_settings(state->draft_store);
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
  if (state->general_startup_mode_combo != nullptr) {
    sync_application_startup_mode_combo(state);
  }
  if (state->general_open_application_input != nullptr) {
    sync_key_binding_widget(
        state, state->general_open_application_input,
        application_open_hotkey_setting_key(),
        application_open_hotkey_text(state->draft_store));
    sync_key_binding_reset_button(
        state, state->general_open_application_reset_button,
        application_open_hotkey_setting_key());
    set_key_binding_input_widget_external_error(
        state->general_open_application_input, {});
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
  if (state->terminal_zoom_entry != nullptr) {
    sync_inheritable_entry(state->terminal_zoom_entry, state->draft_store,
                           terminal_zoom_setting_key(),
                           format_double(display.zoom));
    state->terminal_zoom_valid = true;
    set_entry_validation(state->terminal_zoom_entry, true, {});
  }
  if (state->terminal_auto_close_combo != nullptr) {
    populate_boolean_combo(state->terminal_auto_close_combo,
                           state->draft_store,
                           terminal_auto_close_setting_key(),
                           terminal_auto_close(state->draft_store));
  }
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
  if (state->telnet_terminal_type_entry != nullptr) {
    sync_inheritable_entry(state->telnet_terminal_type_entry,
                           state->draft_store,
                           telnet_terminal_type_setting_key(),
                           telnet.terminal_type);
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
  if (state->ssh_terminal_type_entry != nullptr) {
    sync_inheritable_entry(state->ssh_terminal_type_entry,
                           state->draft_store,
                           ssh_terminal_type_setting_key(),
                           ssh.terminal_type);
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
  if (state->serial_device_entry != nullptr) {
    sync_inheritable_entry(state->serial_device_entry, state->draft_store,
                           serial_device_setting_key(), serial.device);
  }
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
    const std::string fallback = format_setting_value(setting_fallback_value(
        state->draft_store, serial_parity_setting_key(),
        SettingValue{std::string("n")}));
    populate_inheritable_combo(
        state->serial_parity_combo, state->draft_store,
        serial_parity_setting_key(), fallback,
        {
            {.id = "n", .label = "n"},
            {.id = "e", .label = "e"},
            {.id = "o", .label = "o"},
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
    const std::string fallback = format_setting_value(setting_fallback_value(
        state->draft_store, serial_flow_control_setting_key(),
        SettingValue{std::string("none")}));
    populate_inheritable_combo(
        state->serial_flow_control_combo, state->draft_store,
        serial_flow_control_setting_key(), fallback,
        {
            {.id = "none", .label = "none"},
            {.id = "xon", .label = "xon"},
            {.id = "hard", .label = "hard"},
        },
        flow_control);
  }
  if (state->serial_carrier_detect_combo != nullptr) {
    const std::string carrier_detect =
        serial_carrier_detect_to_string(serial.carrier_detect);
    const std::string fallback = format_setting_value(setting_fallback_value(
        state->draft_store, serial_carrier_detect_setting_key(),
        SettingValue{std::string("cd")}));
    populate_inheritable_combo(
        state->serial_carrier_detect_combo, state->draft_store,
        serial_carrier_detect_setting_key(), fallback,
        {
            {.id = "cd", .label = "cd"},
            {.id = "cts", .label = "cts"},
            {.id = "dsr", .label = "dsr"},
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
    const std::string fallback_value =
        std::get<std::string>(setting_fallback_value(
            state->draft_store, terminal_log_mode_setting_key(),
            SettingValue{std::string(terminal_log_raw)}));
    populate_inheritable_combo(
        state->log_mode_combo, state->draft_store,
        terminal_log_mode_setting_key(),
        fallback_value == terminal_log_cooked ? "Cooked" : "Raw",
        {
            {.id = terminal_log_raw, .label = "Raw"},
            {.id = terminal_log_cooked, .label = "Cooked"},
        },
        effective);
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

static void on_application_startup_mode_changed(GtkComboBox *,
                                                gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (state->synchronizing) {
    return;
  }
  update_application_startup_mode_from_widget(state);
  notify_changed(state);
}

static void on_application_hotkey_changed(SettingsWidgetState *state) {
  if (state->synchronizing) {
    return;
  }
  update_application_hotkey_from_widget(state);
  if (key_binding_input_widget_is_valid(
          state->general_open_application_input)) {
    const std::string effective =
        application_open_hotkey_text(state->draft_store);
    set_key_binding_input_widget_empty_clear_enabled(
        state->general_open_application_input, false);
    gtk_entry_set_placeholder_text(
        GTK_ENTRY(key_binding_input_widget_root(
            state->general_open_application_input)),
        effective.empty() ? "Disabled" : "Press a key combination");
    sync_key_binding_reset_button(
        state, state->general_open_application_reset_button,
        application_open_hotkey_setting_key());
  }
  update_action_sensitivity(state);
  notify_changed(state);
}

static void on_application_hotkey_reset_clicked(GtkButton *,
                                                gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  clear_explicit_setting_value(
      &state->draft_store, application_open_hotkey_setting_key());
  const bool previous_synchronizing = state->synchronizing;
  state->synchronizing = true;
  sync_key_binding_widget(
      state, state->general_open_application_input,
      application_open_hotkey_setting_key(),
      application_open_hotkey_text(state->draft_store));
  sync_key_binding_reset_button(
      state, state->general_open_application_reset_button,
      application_open_hotkey_setting_key());
  set_key_binding_input_widget_external_error(
      state->general_open_application_input, {});
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

enum class TerminalKeyBindingField {
  zoom_in,
  zoom_out,
};

static void on_terminal_key_binding_changed(
    SettingsWidgetState *state, TerminalKeyBindingField field) {
  if (state->synchronizing) {
    return;
  }
  KeyBindingInputWidgetState *input =
      field == TerminalKeyBindingField::zoom_in
          ? state->terminal_zoom_in_key_input
          : state->terminal_zoom_out_key_input;
  if (!key_binding_input_widget_is_valid(input)) {
    update_terminal_key_binding_validation(state);
    notify_changed(state);
    return;
  }
  if (field == TerminalKeyBindingField::zoom_in) {
    update_terminal_zoom_in_key_from_widget(state);
  } else {
    update_terminal_zoom_out_key_from_widget(state);
  }
  const SettingKey key =
      field == TerminalKeyBindingField::zoom_in
          ? terminal_zoom_in_key_setting_key()
          : terminal_zoom_out_key_setting_key();
  const std::string effective =
      field == TerminalKeyBindingField::zoom_in
          ? terminal_zoom_in_key(state->draft_store)
          : terminal_zoom_out_key(state->draft_store);
  set_key_binding_input_widget_empty_clear_enabled(input, false);
  gtk_entry_set_placeholder_text(
      GTK_ENTRY(key_binding_input_widget_root(input)),
      effective.empty() ? "Disabled" : "Press a key combination");
  GtkWidget *reset_button =
      field == TerminalKeyBindingField::zoom_in
          ? state->terminal_zoom_in_key_reset_button
          : state->terminal_zoom_out_key_reset_button;
  sync_key_binding_reset_button(state, reset_button, key);
  update_terminal_key_binding_validation(state);
  notify_changed(state);
}

static void reset_terminal_key_binding(SettingsWidgetState *state,
                                       TerminalKeyBindingField field) {
  const SettingKey key =
      field == TerminalKeyBindingField::zoom_in
          ? terminal_zoom_in_key_setting_key()
          : terminal_zoom_out_key_setting_key();
  KeyBindingInputWidgetState *input =
      field == TerminalKeyBindingField::zoom_in
          ? state->terminal_zoom_in_key_input
          : state->terminal_zoom_out_key_input;
  GtkWidget *button =
      field == TerminalKeyBindingField::zoom_in
          ? state->terminal_zoom_in_key_reset_button
          : state->terminal_zoom_out_key_reset_button;
  clear_explicit_setting_value(&state->draft_store, key);
  const std::string effective =
      field == TerminalKeyBindingField::zoom_in
          ? terminal_zoom_in_key(state->draft_store)
          : terminal_zoom_out_key(state->draft_store);
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

static void on_serial_device_changed(GtkEditable *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  if (state->synchronizing) {
    return;
  }
  update_serial_device_from_widget(state);
  notify_changed(state);
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
    attach_row(page, row++, "name", state->general_name_entry);
  }

  const std::string type_id = widget_id(state, "general_type_combo");
  state->general_type_combo = create_combo_box(type_id.c_str());
  gtk_widget_set_sensitive(state->general_type_combo,
                           state->is_runtime ? FALSE : TRUE);
  g_signal_connect(state->general_type_combo, "changed",
                   G_CALLBACK(on_general_type_changed), state);
  attach_row(page, row++, "type", state->general_type_combo);

  if (state->mode == SettingsWidgetMode::global_defaults) {
    const std::string startup_id =
        widget_id(state, "general_startup_mode_combo");
    state->general_startup_mode_combo =
        create_combo_box(startup_id.c_str());
    g_signal_connect(state->general_startup_mode_combo, "changed",
                     G_CALLBACK(on_application_startup_mode_changed), state);
    attach_row(page, row++, "startup_mode",
               state->general_startup_mode_combo);

    const std::string hotkey_id =
        widget_id(state, "general_open_application_entry");
    state->general_open_application_input =
        create_key_binding_input_widget({
            .text = "",
            .accessible_id = hotkey_id,
            .changed = [state]() {
              on_application_hotkey_changed(state);
            },
        });
    GtkWidget *hotkey_row =
        gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(
        GTK_BOX(hotkey_row),
        key_binding_input_widget_root(
            state->general_open_application_input),
        TRUE, TRUE, 0);
    state->general_open_application_reset_button =
        gtk_button_new_with_label("Reset");
    const std::string reset_id =
        widget_id(state, "general_open_application_reset_button");
    assign_accessible_id(state->general_open_application_reset_button,
                         reset_id.c_str());
    g_signal_connect(state->general_open_application_reset_button,
                     "clicked",
                     G_CALLBACK(on_application_hotkey_reset_clicked), state);
    gtk_box_pack_start(
        GTK_BOX(hotkey_row),
        state->general_open_application_reset_button, FALSE, FALSE, 0);
    attach_row(page, row, "open_application", hotkey_row);
  }
  return page;
}

static GtkWidget *create_terminal_page(SettingsWidgetState *state) {
  const std::string page_id = widget_id(state, "terminal_page");
  GtkWidget *page = create_page_grid(page_id.c_str());

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
  if (state->terminal_encoding_entry != nullptr) {
    g_signal_connect(state->terminal_encoding_entry, "changed",
                     G_CALLBACK(on_terminal_encoding_changed), state);
  }
  g_signal_connect(state->terminal_backspace_code_combo, "changed",
                   G_CALLBACK(on_terminal_backspace_code_changed), state);
  g_signal_connect(state->terminal_cursor_key_mode_combo, "changed",
                   G_CALLBACK(on_terminal_cursor_key_mode_changed), state);
  attach_row(page, 0, "encoding", state->terminal_encoding_combo);
  attach_row(page, 1, "backspace_code",
             state->terminal_backspace_code_combo);
  attach_row(page, 2, "cursor_key_mode",
             state->terminal_cursor_key_mode_combo);

  state->terminal_width_entry =
      create_entry(widget_id(state, "terminal_width_entry"));
  g_signal_connect(state->terminal_width_entry, "changed",
                   G_CALLBACK(on_terminal_width_changed), state);
  attach_row(page, 3, "width", state->terminal_width_entry);

  state->terminal_height_entry =
      create_entry(widget_id(state, "terminal_height_entry"));
  g_signal_connect(state->terminal_height_entry, "changed",
                   G_CALLBACK(on_terminal_height_changed), state);
  attach_row(page, 4, "height", state->terminal_height_entry);

  state->terminal_zoom_entry =
      create_entry(widget_id(state, "terminal_zoom_entry"));
  g_signal_connect(state->terminal_zoom_entry, "changed",
                   G_CALLBACK(on_terminal_zoom_changed), state);
  attach_row(page, 5, "zoom", state->terminal_zoom_entry);

  const std::string auto_close_id =
      widget_id(state, "terminal_auto_close_combo");
  state->terminal_auto_close_combo = create_combo_box(auto_close_id.c_str());
  g_signal_connect(state->terminal_auto_close_combo, "changed",
                   G_CALLBACK(on_terminal_auto_close_changed), state);
  attach_row(page, 6, "auto_close", state->terminal_auto_close_combo);

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
      gtk_button_new_with_label("Reset");
  const std::string zoom_in_reset_id =
      widget_id(state, "terminal_zoom_in_key_reset_button");
  assign_accessible_id(state->terminal_zoom_in_key_reset_button,
                       zoom_in_reset_id.c_str());
  g_signal_connect(state->terminal_zoom_in_key_reset_button, "clicked",
                   G_CALLBACK(on_terminal_zoom_in_key_reset_clicked), state);
  gtk_box_pack_start(GTK_BOX(zoom_in_row),
                     state->terminal_zoom_in_key_reset_button, FALSE, FALSE,
                     0);
  attach_row(page, 7, "zoom_in_key", zoom_in_row);

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
      gtk_button_new_with_label("Reset");
  const std::string zoom_out_reset_id =
      widget_id(state, "terminal_zoom_out_key_reset_button");
  assign_accessible_id(state->terminal_zoom_out_key_reset_button,
                       zoom_out_reset_id.c_str());
  g_signal_connect(state->terminal_zoom_out_key_reset_button, "clicked",
                   G_CALLBACK(on_terminal_zoom_out_key_reset_clicked), state);
  gtk_box_pack_start(GTK_BOX(zoom_out_row),
                     state->terminal_zoom_out_key_reset_button, FALSE, FALSE,
                     0);
  attach_row(page, 8, "zoom_out_key", zoom_out_row);

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
  attach_row(page, 0, "address", state->telnet_address_entry);

  state->telnet_port_entry =
      create_entry(widget_id(state, "telnet_port_entry"));
  gtk_widget_set_sensitive(state->telnet_port_entry,
                           state->is_runtime ? FALSE : TRUE);
  g_signal_connect(state->telnet_port_entry, "changed",
                   G_CALLBACK(on_telnet_port_changed), state);
  attach_row(page, 1, "port", state->telnet_port_entry);

  state->telnet_terminal_type_entry =
      create_entry(widget_id(state, "telnet_terminal_type_entry"));
  gtk_widget_set_sensitive(state->telnet_terminal_type_entry,
                           state->is_runtime ? FALSE : TRUE);
  g_signal_connect(state->telnet_terminal_type_entry, "changed",
                   G_CALLBACK(on_telnet_terminal_type_changed), state);
  g_signal_connect(state->telnet_terminal_type_entry, "focus-out-event",
                   G_CALLBACK(on_telnet_terminal_type_focus_out), state);
  attach_row(page, 2, "terminal_type",
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
  attach_row(page, 0, "address", state->ssh_address_entry);

  state->ssh_port_entry =
      create_entry(widget_id(state, "ssh_port_entry"));
  gtk_widget_set_sensitive(state->ssh_port_entry, sensitive);
  g_signal_connect(state->ssh_port_entry, "changed",
                   G_CALLBACK(on_ssh_port_changed), state);
  attach_row(page, 1, "port", state->ssh_port_entry);

  state->ssh_username_entry =
      create_entry(widget_id(state, "ssh_username_entry"));
  gtk_widget_set_sensitive(state->ssh_username_entry, sensitive);
  g_signal_connect(state->ssh_username_entry, "changed",
                   G_CALLBACK(on_ssh_username_changed), state);
  attach_row(page, 2, "username", state->ssh_username_entry);

  state->ssh_identity_file_entry =
      create_entry(widget_id(state, "ssh_identity_file_entry"));
  gtk_widget_set_sensitive(state->ssh_identity_file_entry, sensitive);
  g_signal_connect(state->ssh_identity_file_entry, "changed",
                   G_CALLBACK(on_ssh_identity_file_changed), state);
  attach_row(page, 3, "identity_file", state->ssh_identity_file_entry);

  state->ssh_terminal_type_entry =
      create_entry(widget_id(state, "ssh_terminal_type_entry"));
  gtk_widget_set_sensitive(state->ssh_terminal_type_entry, sensitive);
  g_signal_connect(state->ssh_terminal_type_entry, "changed",
                   G_CALLBACK(on_ssh_terminal_type_changed), state);
  g_signal_connect(state->ssh_terminal_type_entry, "focus-out-event",
                   G_CALLBACK(on_ssh_terminal_type_focus_out), state);
  state->ssh_terminal_type_label =
      attach_row(page, 4, "terminal_type", state->ssh_terminal_type_entry);
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
  attach_row(page, 0, "local_directory",
             state->sftp_local_directory_entry);

  state->sftp_remote_directory_entry =
      create_entry(widget_id(state, "sftp_remote_directory_entry"));
  g_signal_connect(state->sftp_remote_directory_entry, "changed",
                   G_CALLBACK(on_sftp_remote_directory_changed), state);
  attach_row(page, 1, "remote_directory",
             state->sftp_remote_directory_entry);

  return page;
}

static GtkWidget *create_serial_page(SettingsWidgetState *state) {
  const std::string page_id = widget_id(state, "serial_page");
  GtkWidget *page = create_page_grid(page_id.c_str());
  const gboolean device_sensitive = state->is_runtime ? FALSE : TRUE;

  state->serial_device_entry =
      create_entry(widget_id(state, "serial_device_entry"));
  gtk_widget_set_sensitive(state->serial_device_entry, device_sensitive);
  g_signal_connect(state->serial_device_entry, "changed",
                   G_CALLBACK(on_serial_device_changed), state);
  attach_row(page, 0, "device", state->serial_device_entry);

  state->serial_baudrate_entry =
      create_entry(widget_id(state, "serial_baudrate_entry"));
  g_signal_connect(state->serial_baudrate_entry, "changed",
                   G_CALLBACK(on_serial_baudrate_changed), state);
  attach_row(page, 1, "baudrate", state->serial_baudrate_entry);

  const std::string bits_id = widget_id(state, "serial_bits_combo");
  state->serial_bits_combo = create_combo_box(bits_id.c_str());
  g_signal_connect(state->serial_bits_combo, "changed",
                   G_CALLBACK(on_serial_bits_changed), state);
  attach_row(page, 2, "bits", state->serial_bits_combo);

  const std::string parity_id = widget_id(state, "serial_parity_combo");
  state->serial_parity_combo = create_combo_box(parity_id.c_str());
  g_signal_connect(state->serial_parity_combo, "changed",
                   G_CALLBACK(on_serial_parity_changed), state);
  attach_row(page, 3, "parity", state->serial_parity_combo);

  const std::string stop_bit_id =
      widget_id(state, "serial_stop_bit_combo");
  state->serial_stop_bit_combo = create_combo_box(stop_bit_id.c_str());
  g_signal_connect(state->serial_stop_bit_combo, "changed",
                   G_CALLBACK(on_serial_stop_bit_changed), state);
  attach_row(page, 4, "stop_bit", state->serial_stop_bit_combo);

  const std::string flow_control_id =
      widget_id(state, "serial_flow_control_combo");
  state->serial_flow_control_combo =
      create_combo_box(flow_control_id.c_str());
  g_signal_connect(state->serial_flow_control_combo, "changed",
                   G_CALLBACK(on_serial_flow_control_changed), state);
  attach_row(page, 5, "flow_control", state->serial_flow_control_combo);

  const std::string carrier_detect_id =
      widget_id(state, "serial_carrier_detect_combo");
  state->serial_carrier_detect_combo =
      create_combo_box(carrier_detect_id.c_str());
  g_signal_connect(state->serial_carrier_detect_combo, "changed",
                   G_CALLBACK(on_serial_carrier_detect_changed), state);
  attach_row(page, 6, "carrier_detect", state->serial_carrier_detect_combo);

  return page;
}

static GtkWidget *create_transfer_page(SettingsWidgetState *state) {
  const std::string page_id = widget_id(state, "transfer_page");
  GtkWidget *page = create_page_grid(page_id.c_str());

  state->transfer_base_path_entry =
      create_entry(widget_id(state, "transfer_base_path_entry"));
  g_signal_connect(state->transfer_base_path_entry, "changed",
                   G_CALLBACK(on_transfer_base_path_changed), state);
  attach_row(page, 0, "base_path", state->transfer_base_path_entry);

  state->transfer_text_send_rate_entry =
      create_entry(widget_id(state, "transfer_text_send_rate_entry"));
  g_signal_connect(state->transfer_text_send_rate_entry, "changed",
                   G_CALLBACK(on_transfer_text_send_rate_changed), state);
  attach_row(page, 1, "text_send_bytes_per_second",
             state->transfer_text_send_rate_entry);

  const std::string zmodem_id =
      widget_id(state, "transfer_zmodem_autostart_combo");
  state->transfer_zmodem_autostart_combo =
      create_combo_box(zmodem_id.c_str());
  g_signal_connect(state->transfer_zmodem_autostart_combo, "changed",
                   G_CALLBACK(on_transfer_zmodem_autostart_changed), state);
  attach_row(page, 2, "zmodem_autostart",
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
  attach_row(page, 0, "enabled", state->log_enabled_combo);

  state->log_base_directory_entry =
      create_entry(widget_id(state, "log_base_directory_entry"));
  g_signal_connect(state->log_base_directory_entry, "changed",
                   G_CALLBACK(on_log_base_directory_changed), state);
  attach_row(page, 1, "base_directory", state->log_base_directory_entry);

  state->log_file_name_format_entry =
      create_entry(widget_id(state, "log_file_name_format_entry"));
  g_signal_connect(state->log_file_name_format_entry, "changed",
                   G_CALLBACK(on_log_file_name_format_changed), state);
  attach_row(page, 2, "file_name_format",
             state->log_file_name_format_entry);

  const std::string mode_id = widget_id(state, "log_mode_combo");
  state->log_mode_combo = create_combo_box(mode_id.c_str());
  g_signal_connect(state->log_mode_combo, "changed",
                   G_CALLBACK(on_log_mode_changed), state);
  attach_row(page, 3, "mode", state->log_mode_combo);

  return page;
}

static GtkWidget *create_button_box(SettingsWidgetState *state) {
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

  state->apply_button = gtk_button_new_with_label("Apply");
  const std::string apply_id = widget_id(state, "apply_button");
  assign_accessible_id(state->apply_button, apply_id.c_str());
  gtk_widget_set_valign(state->apply_button, GTK_ALIGN_CENTER);
  g_signal_connect(state->apply_button, "clicked",
                   G_CALLBACK(on_apply_clicked), state);
  gtk_container_add(GTK_CONTAINER(button_box), state->apply_button);

  if (state->callbacks.save) {
    state->save_button = gtk_button_new_with_label("Save");
    const std::string save_id = widget_id(state, "save_button");
    assign_accessible_id(state->save_button, save_id.c_str());
    gtk_widget_set_valign(state->save_button, GTK_ALIGN_CENTER);
    g_signal_connect(state->save_button, "clicked",
                     G_CALLBACK(on_save_clicked), state);
    gtk_container_add(GTK_CONTAINER(button_box), state->save_button);
  }

  state->cancel_button = gtk_button_new_with_label("Cancel");
  const std::string cancel_id = widget_id(state, "cancel_button");
  assign_accessible_id(state->cancel_button, cancel_id.c_str());
  gtk_widget_set_valign(state->cancel_button, GTK_ALIGN_CENTER);
  g_signal_connect(state->cancel_button, "clicked",
                   G_CALLBACK(on_cancel_clicked), state);
  gtk_container_add(GTK_CONTAINER(button_box), state->cancel_button);

  return button_box;
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
  gtk_notebook_append_page(GTK_NOTEBOOK(state->notebook), general_page,
                           create_tab_button(state, general_page, "General",
                                             general_tab_id.c_str()));

  GtkWidget *telnet_page = create_telnet_page(state);
  const std::string telnet_tab_id = widget_id(state, "telnet_tab");
  GtkWidget *telnet_tab = create_tab_button(
      state, telnet_page, "TELNET", telnet_tab_id.c_str());
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
      state, serial_page, "Serial", serial_tab_id.c_str());
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
  GtkWidget *ssh_tab =
      create_tab_button(state, ssh_page, "SSH", ssh_tab_id.c_str());
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
  GtkWidget *sftp_tab =
      create_tab_button(state, sftp_page, "SFTP", sftp_tab_id.c_str());
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

  GtkWidget *terminal_page = create_terminal_page(state);
  const std::string terminal_tab_id = widget_id(state, "terminal_tab");
  GtkWidget *terminal_tab = create_tab_button(
      state, terminal_page, "Terminal", terminal_tab_id.c_str());
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
      state, transfer_page, "Transfer", transfer_tab_id.c_str());
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
      state, logging_page, "Logging", logging_tab_id.c_str());
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

  if (state->show_actions) {
    gtk_box_pack_start(GTK_BOX(state->root), create_button_box(state), FALSE,
                       FALSE, 0);
  }
  sync_widgets_from_draft(state);
  update_terminal_key_binding_validation(state);
  update_connection_pages(state);
  state->synchronizing = false;

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

  const bool width_invalid = !state->terminal_width_valid;
  const bool height_invalid = !state->terminal_height_valid;
  const bool zoom_invalid = !state->terminal_zoom_valid;
  const bool encoding_invalid = !state->terminal_encoding_valid;
  const bool telnet_port_invalid = !state->telnet_port_valid;
  const bool ssh_port_invalid = !state->ssh_port_valid;
  const bool baudrate_invalid = !state->serial_baudrate_valid;
  const bool send_rate_invalid = !state->transfer_text_send_rate_valid;
  const bool log_format_invalid = !state->log_file_name_format_valid;
  const auto entry_text = [](GtkWidget *entry) {
    const char *text = gtk_entry_get_text(GTK_ENTRY(entry));
    return std::string(text == nullptr ? "" : text);
  };
  const std::string width_text = entry_text(state->terminal_width_entry);
  const std::string height_text = entry_text(state->terminal_height_entry);
  const std::string zoom_text = entry_text(state->terminal_zoom_entry);
  const std::string encoding_text =
      entry_text(state->terminal_encoding_entry);
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
  restore_invalid(state->terminal_width_entry, width_text, width_invalid,
                  [state]() { update_terminal_width_from_widget(state); });
  restore_invalid(state->terminal_height_entry, height_text, height_invalid,
                  [state]() { update_terminal_height_from_widget(state); });
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

  destroy_key_binding_input_widget(state->terminal_zoom_in_key_input);
  destroy_key_binding_input_widget(state->terminal_zoom_out_key_input);
  destroy_key_binding_input_widget(
      state->general_open_application_input);
  if (state->root != nullptr && gtk_widget_get_parent(state->root) == nullptr) {
    gtk_widget_destroy(state->root);
  }
  delete state;
}

} // namespace elder_terms
