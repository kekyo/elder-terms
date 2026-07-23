#include <elder-terms/settings-widget.h>

#include <algorithm>
#include <cctype>
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
static constexpr char zmodem_autostart_default[] = "default";
static constexpr char zmodem_autostart_enabled[] = "enabled";
static constexpr char zmodem_autostart_disabled[] = "disabled";
static constexpr char terminal_text_default[] = "default";
static constexpr char terminal_backspace_bs[] = "bs";
static constexpr char terminal_backspace_del[] = "del";
static constexpr char terminal_cursor_normal[] = "normal";
static constexpr char terminal_cursor_adm3[] = "adm3";
static constexpr char terminal_log_raw[] = "raw";
static constexpr char terminal_log_cooked[] = "cooked";

struct ConnectionSettingsPage {
  const char *connection_type = nullptr;
  GtkWidget *page = nullptr;
  GtkWidget *tab_label = nullptr;
};

struct SettingsWidgetState {
  SettingsStore applied_store;
  SettingsStore draft_store;
  bool is_runtime = false;
  bool show_actions = true;
  bool synchronizing = false;
  SettingsWidgetCallbacks callbacks;
  GtkWidget *root = nullptr;
  GtkWidget *notebook = nullptr;
  GtkWidget *general_name_entry = nullptr;
  GtkWidget *general_type_combo = nullptr;
  GtkWidget *terminal_width_spin = nullptr;
  GtkWidget *terminal_height_spin = nullptr;
  GtkWidget *terminal_zoom_spin = nullptr;
  GtkWidget *terminal_auto_close_check = nullptr;
  GtkWidget *terminal_encoding_combo = nullptr;
  GtkWidget *terminal_encoding_entry = nullptr;
  GtkWidget *terminal_backspace_code_combo = nullptr;
  GtkWidget *terminal_cursor_key_mode_combo = nullptr;
  bool terminal_encoding_valid = true;
  KeyBindingInputWidgetState *terminal_zoom_in_key_input = nullptr;
  KeyBindingInputWidgetState *terminal_zoom_out_key_input = nullptr;
  GtkWidget *telnet_address_entry = nullptr;
  GtkWidget *telnet_port_spin = nullptr;
  GtkWidget *telnet_terminal_type_entry = nullptr;
  GtkWidget *ssh_address_entry = nullptr;
  GtkWidget *ssh_port_spin = nullptr;
  GtkWidget *ssh_username_entry = nullptr;
  GtkWidget *ssh_identity_file_entry = nullptr;
  GtkWidget *ssh_terminal_type_entry = nullptr;
  GtkWidget *serial_device_entry = nullptr;
  GtkWidget *serial_baudrate_spin = nullptr;
  GtkWidget *serial_bits_combo = nullptr;
  GtkWidget *serial_parity_combo = nullptr;
  GtkWidget *serial_stop_bit_combo = nullptr;
  GtkWidget *serial_flow_control_combo = nullptr;
  GtkWidget *serial_carrier_detect_combo = nullptr;
  GtkWidget *transfer_base_path_entry = nullptr;
  GtkWidget *transfer_text_send_rate_spin = nullptr;
  GtkWidget *transfer_zmodem_autostart_combo = nullptr;
  GtkWidget *log_enabled_check = nullptr;
  GtkWidget *log_base_directory_entry = nullptr;
  GtkWidget *log_file_name_format_entry = nullptr;
  GtkWidget *log_mode_combo = nullptr;
  bool log_file_name_format_valid = true;
  GtkWidget *apply_button = nullptr;
  GtkWidget *save_button = nullptr;
  GtkWidget *cancel_button = nullptr;
  std::vector<ConnectionSettingsPage> connection_pages;
};

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

static void attach_row(GtkWidget *grid, int row, const char *label_text,
                       GtkWidget *control) {
  GtkWidget *label = create_row_label(label_text);
  gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);
  gtk_widget_set_hexpand(control, true);
  gtk_widget_set_halign(control, GTK_ALIGN_FILL);
  gtk_grid_attach(GTK_GRID(grid), control, 1, row, 1, 1);
}

static GtkWidget *create_spin_button(double minimum, double maximum,
                                     double step, guint digits,
                                     const char *id) {
  GtkWidget *spin = gtk_spin_button_new_with_range(minimum, maximum, step);
  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(spin), digits);
  gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(spin), TRUE);
  assign_accessible_id(spin, id);
  return spin;
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

static const char *zmodem_autostart_choice_id(const SettingsStore &store) {
  const bool configured = setting_boolean_value_or_default(
      store, transfer_zmodem_autostart_setting_key(), false);
  if (!setting_has_explicit_value(store,
                                  transfer_zmodem_autostart_setting_key()) &&
      !configured) {
    return zmodem_autostart_default;
  }
  return configured ? zmodem_autostart_enabled : zmodem_autostart_disabled;
}

static std::string terminal_backspace_default_label(
    const TerminalTextSettings &defaults) {
  return defaults.backspace_code == TerminalBackspaceCode::bs
             ? "Default (BS)"
             : "Default (DEL)";
}

static std::string terminal_cursor_default_label(
    const TerminalTextSettings &defaults) {
  return defaults.cursor_key_mode == TerminalCursorKeyMode::adm3
             ? "Default (ADM3)"
             : "Default (Normal)";
}

static void populate_terminal_encoding_combo(SettingsWidgetState *state,
                                             const TerminalTextSettings &defaults) {
  if (state->terminal_encoding_combo == nullptr) {
    return;
  }

  gtk_combo_box_text_remove_all(
      GTK_COMBO_BOX_TEXT(state->terminal_encoding_combo));
  const std::string default_label = "Default (" + defaults.encoding + ")";
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
    return;
  }

  const std::string configured = trim_ascii_whitespace(
      setting_string_value_or_default(state->draft_store,
                                      terminal_encoding_setting_key(),
                                      defaults.encoding));
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
    SettingsWidgetState *state, const TerminalTextSettings &defaults) {
  if (state->terminal_backspace_code_combo != nullptr) {
    gtk_combo_box_text_remove_all(
        GTK_COMBO_BOX_TEXT(state->terminal_backspace_code_combo));
    const std::string default_label =
        terminal_backspace_default_label(defaults);
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
                   terminal_backspace_code_to_string(defaults.backspace_code)) ==
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
    const std::string default_label = terminal_cursor_default_label(defaults);
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
                   terminal_cursor_key_mode_to_string(
                       defaults.cursor_key_mode)) == terminal_cursor_adm3
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
    const bool visible = active_type == page.connection_type;
    gtk_widget_set_visible(page.page, visible);
    gtk_widget_set_visible(page.tab_label, visible);
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
      active_combo_id(state->general_type_combo, local_connection_type);
  set_setting_value(&state->draft_store, general_type_setting_key(),
                    SettingValue{type});
  update_connection_pages(state);
  sync_terminal_text_widgets(state);
}

static void update_terminal_width_from_widget(SettingsWidgetState *state) {
  set_setting_value(
      &state->draft_store, terminal_width_setting_key(),
      SettingValue{static_cast<gint64>(gtk_spin_button_get_value_as_int(
          GTK_SPIN_BUTTON(state->terminal_width_spin)))});
}

static void update_terminal_height_from_widget(SettingsWidgetState *state) {
  set_setting_value(
      &state->draft_store, terminal_height_setting_key(),
      SettingValue{static_cast<gint64>(gtk_spin_button_get_value_as_int(
          GTK_SPIN_BUTTON(state->terminal_height_spin)))});
}

static void update_terminal_zoom_from_widget(SettingsWidgetState *state) {
  set_setting_value(&state->draft_store, terminal_zoom_setting_key(),
                    SettingValue{gtk_spin_button_get_value(
                        GTK_SPIN_BUTTON(state->terminal_zoom_spin))});
}

static void update_terminal_auto_close_from_widget(
    SettingsWidgetState *state) {
  set_setting_value(
      &state->draft_store, terminal_auto_close_setting_key(),
      SettingValue{gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(
                       state->terminal_auto_close_check)) != FALSE});
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
  const TerminalTextSettings defaults =
      default_terminal_text_settings(connection_kind_value(state->draft_store));
  const std::string default_label = "Default (" + defaults.encoding + ")";
  if (active_id != nullptr &&
      std::string(active_id) == terminal_text_default &&
      entered == default_label) {
    clear_explicit_setting_value(&state->draft_store,
                                 terminal_encoding_setting_key());
    set_terminal_encoding_validation(state, true, {});
    return;
  }

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
  const KeyBindingParseResult zoom_in = parse_key_binding(
      key_binding_input_widget_text(state->terminal_zoom_in_key_input));
  const KeyBindingParseResult zoom_out = parse_key_binding(
      key_binding_input_widget_text(state->terminal_zoom_out_key_input));
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

static bool settings_inputs_valid(const SettingsWidgetState *state) {
  return state->terminal_encoding_valid &&
         terminal_key_binding_inputs_valid(state) &&
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
  set_setting_value(
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
  set_setting_value(
      &state->draft_store, terminal_zoom_out_key_setting_key(),
      SettingValue{
          key_binding_input_widget_text(state->terminal_zoom_out_key_input)});
}

static void update_telnet_address_from_widget(SettingsWidgetState *state) {
  const char *text =
      gtk_entry_get_text(GTK_ENTRY(state->telnet_address_entry));
  set_setting_value(&state->draft_store, telnet_address_setting_key(),
                    SettingValue{std::string(text == nullptr ? "" : text)});
}

static void update_telnet_port_from_widget(SettingsWidgetState *state) {
  set_setting_value(
      &state->draft_store, telnet_port_setting_key(),
      SettingValue{static_cast<gint64>(gtk_spin_button_get_value_as_int(
          GTK_SPIN_BUTTON(state->telnet_port_spin)))});
}

static void update_telnet_terminal_type_from_widget(
    SettingsWidgetState *state) {
  const char *text =
      gtk_entry_get_text(GTK_ENTRY(state->telnet_terminal_type_entry));
  const std::string terminal_type = text == nullptr ? "" : text;
  if (ascii_blank(terminal_type)) {
    clear_explicit_setting_value(&state->draft_store,
                                 telnet_terminal_type_setting_key());
    return;
  }
  if (!setting_has_explicit_value(state->draft_store,
                                  telnet_terminal_type_setting_key()) &&
      terminal_type ==
          telnet_connection_settings(state->draft_store).terminal_type) {
    return;
  }
  set_explicit_setting_value(&state->draft_store,
                             telnet_terminal_type_setting_key(),
                             SettingValue{terminal_type});
}

static void update_ssh_address_from_widget(SettingsWidgetState *state) {
  const char *text = gtk_entry_get_text(GTK_ENTRY(state->ssh_address_entry));
  set_setting_value(&state->draft_store, ssh_address_setting_key(),
                    SettingValue{std::string(text == nullptr ? "" : text)});
}

static void update_ssh_port_from_widget(SettingsWidgetState *state) {
  set_setting_value(
      &state->draft_store, ssh_port_setting_key(),
      SettingValue{static_cast<gint64>(gtk_spin_button_get_value_as_int(
          GTK_SPIN_BUTTON(state->ssh_port_spin)))});
}

static void update_ssh_username_from_widget(SettingsWidgetState *state) {
  const char *text = gtk_entry_get_text(GTK_ENTRY(state->ssh_username_entry));
  set_setting_value(&state->draft_store, ssh_username_setting_key(),
                    SettingValue{std::string(text == nullptr ? "" : text)});
}

static void update_ssh_identity_file_from_widget(
    SettingsWidgetState *state) {
  const char *text =
      gtk_entry_get_text(GTK_ENTRY(state->ssh_identity_file_entry));
  set_setting_value(&state->draft_store, ssh_identity_file_setting_key(),
                    SettingValue{std::string(text == nullptr ? "" : text)});
}

static void update_ssh_terminal_type_from_widget(
    SettingsWidgetState *state) {
  const char *text =
      gtk_entry_get_text(GTK_ENTRY(state->ssh_terminal_type_entry));
  const std::string terminal_type = text == nullptr ? "" : text;
  if (ascii_blank(terminal_type)) {
    clear_explicit_setting_value(&state->draft_store,
                                 ssh_terminal_type_setting_key());
    return;
  }
  if (!setting_has_explicit_value(state->draft_store,
                                  ssh_terminal_type_setting_key()) &&
      terminal_type ==
          ssh_connection_settings(state->draft_store).terminal_type) {
    return;
  }
  set_explicit_setting_value(&state->draft_store,
                             ssh_terminal_type_setting_key(),
                             SettingValue{terminal_type});
}

static void update_serial_device_from_widget(SettingsWidgetState *state) {
  const char *text = gtk_entry_get_text(GTK_ENTRY(state->serial_device_entry));
  set_setting_value(&state->draft_store, serial_device_setting_key(),
                    SettingValue{std::string(text == nullptr ? "" : text)});
}

static void update_serial_baudrate_from_widget(SettingsWidgetState *state) {
  set_setting_value(
      &state->draft_store, serial_baudrate_setting_key(),
      SettingValue{static_cast<gint64>(gtk_spin_button_get_value_as_int(
          GTK_SPIN_BUTTON(state->serial_baudrate_spin)))});
}

static void update_serial_bits_from_widget(SettingsWidgetState *state) {
  const std::string bits = active_combo_id(state->serial_bits_combo, "8");
  set_setting_value(&state->draft_store, serial_bits_setting_key(),
                    SettingValue{static_cast<gint64>(std::stoll(bits))});
}

static void update_serial_parity_from_widget(SettingsWidgetState *state) {
  set_setting_value(
      &state->draft_store, serial_parity_setting_key(),
      SettingValue{active_combo_id(state->serial_parity_combo, "n")});
}

static void update_serial_stop_bit_from_widget(SettingsWidgetState *state) {
  const std::string stop_bit =
      active_combo_id(state->serial_stop_bit_combo, "1");
  set_setting_value(&state->draft_store, serial_stop_bit_setting_key(),
                    SettingValue{static_cast<gint64>(std::stoll(stop_bit))});
}

static void update_serial_flow_control_from_widget(
    SettingsWidgetState *state) {
  set_setting_value(
      &state->draft_store, serial_flow_control_setting_key(),
      SettingValue{
          active_combo_id(state->serial_flow_control_combo, "none")});
}

static void update_serial_carrier_detect_from_widget(
    SettingsWidgetState *state) {
  set_setting_value(
      &state->draft_store, serial_carrier_detect_setting_key(),
      SettingValue{active_combo_id(state->serial_carrier_detect_combo, "cd")});
}

static void update_transfer_base_path_from_widget(
    SettingsWidgetState *state) {
  const char *text =
      gtk_entry_get_text(GTK_ENTRY(state->transfer_base_path_entry));
  set_setting_value(&state->draft_store, transfer_base_path_setting_key(),
                    SettingValue{std::string(text == nullptr ? "" : text)});
}

static void update_transfer_text_send_rate_from_widget(
    SettingsWidgetState *state) {
  set_setting_value(
      &state->draft_store, transfer_text_send_bytes_per_second_setting_key(),
      SettingValue{static_cast<gint64>(gtk_spin_button_get_value_as_int(
          GTK_SPIN_BUTTON(state->transfer_text_send_rate_spin)))});
}

static void update_transfer_zmodem_autostart_from_widget(
    SettingsWidgetState *state) {
  const std::string choice = active_combo_id(
      state->transfer_zmodem_autostart_combo, zmodem_autostart_default);
  if (choice == zmodem_autostart_default) {
    clear_explicit_setting_value(&state->draft_store,
                                 transfer_zmodem_autostart_setting_key());
    return;
  }

  set_explicit_setting_value(
      &state->draft_store, transfer_zmodem_autostart_setting_key(),
      SettingValue{choice == zmodem_autostart_enabled});
}

static void update_log_enabled_from_widget(SettingsWidgetState *state) {
  set_setting_value(
      &state->draft_store, terminal_log_enabled_setting_key(),
      SettingValue{gtk_toggle_button_get_active(
                       GTK_TOGGLE_BUTTON(state->log_enabled_check)) != FALSE});
}

static void update_log_base_directory_from_widget(
    SettingsWidgetState *state) {
  const char *text =
      gtk_entry_get_text(GTK_ENTRY(state->log_base_directory_entry));
  set_setting_value(&state->draft_store,
                    terminal_log_base_directory_setting_key(),
                    SettingValue{std::string(text == nullptr ? "" : text)});
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
  std::string reason;
  if (!terminal_log_file_name_format_is_valid(format, &reason)) {
    set_log_file_name_format_validation(state, false, reason);
    return;
  }

  set_log_file_name_format_validation(state, true, {});
  set_setting_value(&state->draft_store,
                    terminal_log_file_name_format_setting_key(),
                    SettingValue{format});
}

static void update_log_mode_from_widget(SettingsWidgetState *state) {
  set_setting_value(
      &state->draft_store, terminal_log_mode_setting_key(),
      SettingValue{active_combo_id(state->log_mode_combo, terminal_log_raw)});
}

static void sync_widgets_from_draft(SettingsWidgetState *state) {
  const TerminalDisplaySettings display =
      terminal_display_settings(state->draft_store);
  const TelnetConnectionSettings telnet =
      telnet_connection_settings(state->draft_store);
  const SshConnectionSettings ssh =
      ssh_connection_settings(state->draft_store);
  const SerialConnectionSettings serial =
      serial_connection_settings(state->draft_store);
  const TerminalLogSettings log = terminal_log_settings(state->draft_store);

  if (state->general_name_entry != nullptr) {
    gtk_entry_set_text(GTK_ENTRY(state->general_name_entry),
                       general_connection_name(state->draft_store).c_str());
  }
  if (state->general_type_combo != nullptr) {
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(state->general_type_combo),
                                connection_type_value(state->draft_store)
                                    .c_str());
  }
  sync_terminal_text_widgets(state);
  if (state->terminal_width_spin != nullptr) {
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(state->terminal_width_spin),
                              static_cast<double>(display.width));
  }
  if (state->terminal_height_spin != nullptr) {
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(state->terminal_height_spin),
                              static_cast<double>(display.height));
  }
  if (state->terminal_zoom_spin != nullptr) {
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(state->terminal_zoom_spin),
                              display.zoom);
  }
  if (state->terminal_auto_close_check != nullptr) {
    gtk_toggle_button_set_active(
        GTK_TOGGLE_BUTTON(state->terminal_auto_close_check),
        terminal_auto_close(state->draft_store) ? TRUE : FALSE);
  }
  if (state->terminal_zoom_in_key_input != nullptr) {
    set_key_binding_input_widget_text(state->terminal_zoom_in_key_input,
                                      terminal_zoom_in_key(state->draft_store));
  }
  if (state->terminal_zoom_out_key_input != nullptr) {
    set_key_binding_input_widget_text(
        state->terminal_zoom_out_key_input,
        terminal_zoom_out_key(state->draft_store));
  }
  if (state->telnet_address_entry != nullptr) {
    gtk_entry_set_text(GTK_ENTRY(state->telnet_address_entry),
                       telnet.address.c_str());
  }
  if (state->telnet_port_spin != nullptr) {
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(state->telnet_port_spin),
                              static_cast<double>(telnet.port));
  }
  if (state->telnet_terminal_type_entry != nullptr) {
    gtk_entry_set_text(GTK_ENTRY(state->telnet_terminal_type_entry),
                       telnet.terminal_type.c_str());
  }
  if (state->ssh_address_entry != nullptr) {
    gtk_entry_set_text(GTK_ENTRY(state->ssh_address_entry),
                       ssh.address.c_str());
  }
  if (state->ssh_port_spin != nullptr) {
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(state->ssh_port_spin),
                              static_cast<double>(ssh.port));
  }
  if (state->ssh_username_entry != nullptr) {
    gtk_entry_set_text(GTK_ENTRY(state->ssh_username_entry),
                       ssh.username.c_str());
  }
  if (state->ssh_identity_file_entry != nullptr) {
    gtk_entry_set_text(GTK_ENTRY(state->ssh_identity_file_entry),
                       ssh.identity_file.c_str());
  }
  if (state->ssh_terminal_type_entry != nullptr) {
    gtk_entry_set_text(GTK_ENTRY(state->ssh_terminal_type_entry),
                       ssh.terminal_type.c_str());
  }
  if (state->serial_device_entry != nullptr) {
    gtk_entry_set_text(GTK_ENTRY(state->serial_device_entry),
                       serial.device.c_str());
  }
  if (state->serial_baudrate_spin != nullptr) {
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(state->serial_baudrate_spin),
                              static_cast<double>(serial.baudrate));
  }
  if (state->serial_bits_combo != nullptr) {
    const std::string bits = std::to_string(serial.bits);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(state->serial_bits_combo),
                                bits.c_str());
  }
  if (state->serial_parity_combo != nullptr) {
    const std::string parity = serial_parity_to_string(serial.parity);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(state->serial_parity_combo),
                                parity.c_str());
  }
  if (state->serial_stop_bit_combo != nullptr) {
    const std::string stop_bit = std::to_string(serial.stop_bit);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(state->serial_stop_bit_combo),
                                stop_bit.c_str());
  }
  if (state->serial_flow_control_combo != nullptr) {
    const std::string flow_control =
        serial_flow_control_to_string(serial.flow_control);
    gtk_combo_box_set_active_id(
        GTK_COMBO_BOX(state->serial_flow_control_combo), flow_control.c_str());
  }
  if (state->serial_carrier_detect_combo != nullptr) {
    const std::string carrier_detect =
        serial_carrier_detect_to_string(serial.carrier_detect);
    gtk_combo_box_set_active_id(
        GTK_COMBO_BOX(state->serial_carrier_detect_combo),
        carrier_detect.c_str());
  }
  if (state->transfer_base_path_entry != nullptr) {
    gtk_entry_set_text(GTK_ENTRY(state->transfer_base_path_entry),
                       transfer_base_path(state->draft_store).c_str());
  }
  if (state->transfer_text_send_rate_spin != nullptr) {
    gtk_spin_button_set_value(
        GTK_SPIN_BUTTON(state->transfer_text_send_rate_spin),
        static_cast<double>(
            transfer_text_send_bytes_per_second(state->draft_store)));
  }
  if (state->transfer_zmodem_autostart_combo != nullptr) {
    gtk_combo_box_set_active_id(
        GTK_COMBO_BOX(state->transfer_zmodem_autostart_combo),
        zmodem_autostart_choice_id(state->draft_store));
  }
  if (state->log_enabled_check != nullptr) {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->log_enabled_check),
                                 log.enabled ? TRUE : FALSE);
  }
  if (state->log_base_directory_entry != nullptr) {
    gtk_entry_set_text(GTK_ENTRY(state->log_base_directory_entry),
                       log.base_directory.c_str());
  }
  if (state->log_file_name_format_entry != nullptr) {
    gtk_entry_set_text(GTK_ENTRY(state->log_file_name_format_entry),
                       log.file_name_format.c_str());
    set_log_file_name_format_validation(state, true, {});
  }
  if (state->log_mode_combo != nullptr) {
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(state->log_mode_combo),
                                terminal_log_mode_to_string(log.mode));
  }
  if (state->notebook != nullptr) {
    update_connection_pages(state);
  }
}

static void sync_draft_from_widgets(SettingsWidgetState *state) {
  update_general_name_from_widget(state);
  update_general_type_from_widget(state);
  update_terminal_encoding_from_widget(state);
  update_terminal_backspace_code_from_widget(state);
  update_terminal_cursor_key_mode_from_widget(state);
  update_terminal_width_from_widget(state);
  update_terminal_height_from_widget(state);
  update_terminal_zoom_from_widget(state);
  update_terminal_auto_close_from_widget(state);
  update_terminal_zoom_in_key_from_widget(state);
  update_terminal_zoom_out_key_from_widget(state);
  update_telnet_address_from_widget(state);
  update_telnet_port_from_widget(state);
  update_telnet_terminal_type_from_widget(state);
  update_ssh_address_from_widget(state);
  update_ssh_port_from_widget(state);
  update_ssh_username_from_widget(state);
  update_ssh_identity_file_from_widget(state);
  update_ssh_terminal_type_from_widget(state);
  update_serial_device_from_widget(state);
  update_serial_baudrate_from_widget(state);
  update_serial_bits_from_widget(state);
  update_serial_parity_from_widget(state);
  update_serial_stop_bit_from_widget(state);
  update_serial_flow_control_from_widget(state);
  update_serial_carrier_detect_from_widget(state);
  update_transfer_base_path_from_widget(state);
  update_transfer_text_send_rate_from_widget(state);
  update_transfer_zmodem_autostart_from_widget(state);
  update_log_enabled_from_widget(state);
  update_log_base_directory_from_widget(state);
  update_log_file_name_format_from_widget(state);
  update_log_mode_from_widget(state);
}

static void on_general_type_changed(GtkComboBox *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  update_general_type_from_widget(state);
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

static void on_terminal_width_changed(GtkSpinButton *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  update_terminal_width_from_widget(state);
  notify_changed(state);
}

static void on_terminal_height_changed(GtkSpinButton *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  update_terminal_height_from_widget(state);
  notify_changed(state);
}

static void on_terminal_zoom_changed(GtkSpinButton *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  update_terminal_zoom_from_widget(state);
  notify_changed(state);
}

static void on_terminal_auto_close_toggled(GtkToggleButton *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
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

static void on_terminal_key_binding_changed(SettingsWidgetState *state) {
  update_terminal_key_binding_validation(state);
  if (!terminal_key_binding_inputs_valid(state)) {
    notify_changed(state);
    return;
  }
  update_terminal_zoom_in_key_from_widget(state);
  update_terminal_zoom_out_key_from_widget(state);
  notify_changed(state);
}

static void on_telnet_address_changed(GtkEditable *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  update_telnet_address_from_widget(state);
  notify_changed(state);
}

static void on_telnet_port_changed(GtkSpinButton *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  update_telnet_port_from_widget(state);
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
  gtk_entry_set_text(GTK_ENTRY(state->telnet_terminal_type_entry),
                     settings.terminal_type.c_str());
  state->synchronizing = previous_synchronizing;
  return FALSE;
}

static void on_ssh_address_changed(GtkEditable *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  update_ssh_address_from_widget(state);
  notify_changed(state);
}

static void on_ssh_port_changed(GtkSpinButton *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  update_ssh_port_from_widget(state);
  notify_changed(state);
}

static void on_ssh_username_changed(GtkEditable *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  update_ssh_username_from_widget(state);
  notify_changed(state);
}

static void on_ssh_identity_file_changed(GtkEditable *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
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
  gtk_entry_set_text(GTK_ENTRY(state->ssh_terminal_type_entry),
                     settings.terminal_type.c_str());
  state->synchronizing = previous_synchronizing;
  return FALSE;
}

static void on_serial_device_changed(GtkEditable *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  update_serial_device_from_widget(state);
  notify_changed(state);
}

static void on_serial_baudrate_changed(GtkSpinButton *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  update_serial_baudrate_from_widget(state);
  notify_changed(state);
}

static void on_serial_bits_changed(GtkComboBox *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  update_serial_bits_from_widget(state);
  notify_changed(state);
}

static void on_serial_parity_changed(GtkComboBox *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  update_serial_parity_from_widget(state);
  notify_changed(state);
}

static void on_serial_stop_bit_changed(GtkComboBox *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  update_serial_stop_bit_from_widget(state);
  notify_changed(state);
}

static void on_serial_flow_control_changed(GtkComboBox *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  update_serial_flow_control_from_widget(state);
  notify_changed(state);
}

static void on_serial_carrier_detect_changed(GtkComboBox *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  update_serial_carrier_detect_from_widget(state);
  notify_changed(state);
}

static void on_transfer_base_path_changed(GtkEditable *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  update_transfer_base_path_from_widget(state);
  notify_changed(state);
}

static void on_transfer_text_send_rate_changed(GtkSpinButton *,
                                               gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  update_transfer_text_send_rate_from_widget(state);
  notify_changed(state);
}

static void on_transfer_zmodem_autostart_changed(GtkComboBox *,
                                                 gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  update_transfer_zmodem_autostart_from_widget(state);
  notify_changed(state);
}

static void on_log_enabled_toggled(GtkToggleButton *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  update_log_enabled_from_widget(state);
  notify_changed(state);
}

static void on_log_base_directory_changed(GtkEditable *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
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
  update_log_mode_from_widget(state);
  notify_changed(state);
}

static void on_apply_clicked(GtkButton *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  sync_draft_from_widgets(state);
  state->applied_store = state->draft_store;
  if (state->callbacks.apply) {
    state->callbacks.apply(state->applied_store);
  }
}

static void on_save_clicked(GtkButton *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  sync_draft_from_widgets(state);
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
  GtkWidget *page = create_page_grid("settings_general_page");
  state->general_name_entry = gtk_entry_new();
  assign_accessible_id(state->general_name_entry,
                       "settings_general_name_entry");
  gtk_entry_set_text(GTK_ENTRY(state->general_name_entry),
                     general_connection_name(state->draft_store).c_str());
  g_signal_connect(state->general_name_entry, "changed",
                   G_CALLBACK(on_general_name_changed), state);
  g_signal_connect(state->general_name_entry, "focus-out-event",
                   G_CALLBACK(on_general_name_focus_out), state);
  attach_row(page, 0, "name", state->general_name_entry);

  state->general_type_combo =
      create_combo_box("settings_general_type_combo");
  append_combo_option(state->general_type_combo, local_connection_type,
                      "Local");
  append_combo_option(state->general_type_combo, telnet_connection_type,
                      "TELNET");
  append_combo_option(state->general_type_combo, serial_connection_type,
                      "Serial");
  append_combo_option(state->general_type_combo, ssh_connection_type, "SSH");
  gtk_combo_box_set_active_id(GTK_COMBO_BOX(state->general_type_combo),
                              connection_type_value(state->draft_store).c_str());
  gtk_widget_set_sensitive(state->general_type_combo,
                           state->is_runtime ? FALSE : TRUE);
  g_signal_connect(state->general_type_combo, "changed",
                   G_CALLBACK(on_general_type_changed), state);
  attach_row(page, 1, "type", state->general_type_combo);
  return page;
}

static GtkWidget *create_terminal_page(SettingsWidgetState *state) {
  GtkWidget *page = create_page_grid("settings_terminal_page");
  const TerminalDisplaySettings display =
      terminal_display_settings(state->draft_store);

  state->terminal_encoding_combo = create_editable_combo_box(
      "settings_terminal_encoding_combo", "settings_terminal_encoding_entry",
      &state->terminal_encoding_entry);
  state->terminal_backspace_code_combo =
      create_combo_box("settings_terminal_backspace_code_combo");
  state->terminal_cursor_key_mode_combo =
      create_combo_box("settings_terminal_cursor_key_mode_combo");
  sync_terminal_text_widgets(state);
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

  state->terminal_width_spin =
      create_spin_button(1.0, 1000.0, 1.0, 0, "settings_terminal_width_spin");
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(state->terminal_width_spin),
                            static_cast<double>(display.width));
  g_signal_connect(state->terminal_width_spin, "value-changed",
                   G_CALLBACK(on_terminal_width_changed), state);
  attach_row(page, 3, "width", state->terminal_width_spin);

  state->terminal_height_spin =
      create_spin_button(1.0, 1000.0, 1.0, 0, "settings_terminal_height_spin");
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(state->terminal_height_spin),
                            static_cast<double>(display.height));
  g_signal_connect(state->terminal_height_spin, "value-changed",
                   G_CALLBACK(on_terminal_height_changed), state);
  attach_row(page, 4, "height", state->terminal_height_spin);

  state->terminal_zoom_spin =
      create_spin_button(0.1, 5.0, 0.1, 2, "settings_terminal_zoom_spin");
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(state->terminal_zoom_spin),
                            display.zoom);
  g_signal_connect(state->terminal_zoom_spin, "value-changed",
                   G_CALLBACK(on_terminal_zoom_changed), state);
  attach_row(page, 5, "zoom", state->terminal_zoom_spin);

  state->terminal_auto_close_check =
      gtk_check_button_new_with_label("enabled");
  assign_accessible_id(state->terminal_auto_close_check,
                       "settings_terminal_auto_close_check");
  gtk_toggle_button_set_active(
      GTK_TOGGLE_BUTTON(state->terminal_auto_close_check),
      terminal_auto_close(state->draft_store) ? TRUE : FALSE);
  g_signal_connect(state->terminal_auto_close_check, "toggled",
                   G_CALLBACK(on_terminal_auto_close_toggled), state);
  attach_row(page, 6, "auto_close", state->terminal_auto_close_check);

  state->terminal_zoom_in_key_input = create_key_binding_input_widget({
      .text = terminal_zoom_in_key(state->draft_store),
      .accessible_id = "settings_terminal_zoom_in_key_entry",
      .changed = [state]() { on_terminal_key_binding_changed(state); },
  });
  attach_row(page, 7, "zoom_in_key",
             key_binding_input_widget_root(
                 state->terminal_zoom_in_key_input));

  state->terminal_zoom_out_key_input = create_key_binding_input_widget({
      .text = terminal_zoom_out_key(state->draft_store),
      .accessible_id = "settings_terminal_zoom_out_key_entry",
      .changed = [state]() { on_terminal_key_binding_changed(state); },
  });
  attach_row(page, 8, "zoom_out_key",
             key_binding_input_widget_root(
                 state->terminal_zoom_out_key_input));

  return page;
}

static GtkWidget *create_telnet_page(SettingsWidgetState *state) {
  GtkWidget *page = create_page_grid("settings_telnet_page");
  const TelnetConnectionSettings settings =
      telnet_connection_settings(state->draft_store);

  state->telnet_address_entry = gtk_entry_new();
  assign_accessible_id(state->telnet_address_entry,
                       "settings_telnet_address_entry");
  gtk_entry_set_text(GTK_ENTRY(state->telnet_address_entry),
                     settings.address.c_str());
  gtk_widget_set_sensitive(state->telnet_address_entry,
                           state->is_runtime ? FALSE : TRUE);
  g_signal_connect(state->telnet_address_entry, "changed",
                   G_CALLBACK(on_telnet_address_changed), state);
  attach_row(page, 0, "address", state->telnet_address_entry);

  state->telnet_port_spin =
      create_spin_button(1.0, 65535.0, 1.0, 0, "settings_telnet_port_spin");
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(state->telnet_port_spin),
                            static_cast<double>(settings.port));
  gtk_widget_set_sensitive(state->telnet_port_spin,
                           state->is_runtime ? FALSE : TRUE);
  g_signal_connect(state->telnet_port_spin, "value-changed",
                   G_CALLBACK(on_telnet_port_changed), state);
  attach_row(page, 1, "port", state->telnet_port_spin);

  state->telnet_terminal_type_entry = gtk_entry_new();
  assign_accessible_id(state->telnet_terminal_type_entry,
                       "settings_telnet_terminal_type_entry");
  gtk_entry_set_text(GTK_ENTRY(state->telnet_terminal_type_entry),
                     settings.terminal_type.c_str());
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
  GtkWidget *page = create_page_grid("settings_ssh_page");
  const SshConnectionSettings settings =
      ssh_connection_settings(state->draft_store);
  const gboolean sensitive = state->is_runtime ? FALSE : TRUE;

  state->ssh_address_entry = gtk_entry_new();
  assign_accessible_id(state->ssh_address_entry,
                       "settings_ssh_address_entry");
  gtk_entry_set_text(GTK_ENTRY(state->ssh_address_entry),
                     settings.address.c_str());
  gtk_widget_set_sensitive(state->ssh_address_entry, sensitive);
  g_signal_connect(state->ssh_address_entry, "changed",
                   G_CALLBACK(on_ssh_address_changed), state);
  attach_row(page, 0, "address", state->ssh_address_entry);

  state->ssh_port_spin =
      create_spin_button(1.0, 65535.0, 1.0, 0, "settings_ssh_port_spin");
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(state->ssh_port_spin),
                            static_cast<double>(settings.port));
  gtk_widget_set_sensitive(state->ssh_port_spin, sensitive);
  g_signal_connect(state->ssh_port_spin, "value-changed",
                   G_CALLBACK(on_ssh_port_changed), state);
  attach_row(page, 1, "port", state->ssh_port_spin);

  state->ssh_username_entry = gtk_entry_new();
  assign_accessible_id(state->ssh_username_entry,
                       "settings_ssh_username_entry");
  gtk_entry_set_text(GTK_ENTRY(state->ssh_username_entry),
                     settings.username.c_str());
  gtk_widget_set_sensitive(state->ssh_username_entry, sensitive);
  g_signal_connect(state->ssh_username_entry, "changed",
                   G_CALLBACK(on_ssh_username_changed), state);
  attach_row(page, 2, "username", state->ssh_username_entry);

  state->ssh_identity_file_entry = gtk_entry_new();
  assign_accessible_id(state->ssh_identity_file_entry,
                       "settings_ssh_identity_file_entry");
  gtk_entry_set_text(GTK_ENTRY(state->ssh_identity_file_entry),
                     settings.identity_file.c_str());
  gtk_widget_set_sensitive(state->ssh_identity_file_entry, sensitive);
  g_signal_connect(state->ssh_identity_file_entry, "changed",
                   G_CALLBACK(on_ssh_identity_file_changed), state);
  attach_row(page, 3, "identity_file", state->ssh_identity_file_entry);

  state->ssh_terminal_type_entry = gtk_entry_new();
  assign_accessible_id(state->ssh_terminal_type_entry,
                       "settings_ssh_terminal_type_entry");
  gtk_entry_set_text(GTK_ENTRY(state->ssh_terminal_type_entry),
                     settings.terminal_type.c_str());
  gtk_widget_set_sensitive(state->ssh_terminal_type_entry, sensitive);
  g_signal_connect(state->ssh_terminal_type_entry, "changed",
                   G_CALLBACK(on_ssh_terminal_type_changed), state);
  g_signal_connect(state->ssh_terminal_type_entry, "focus-out-event",
                   G_CALLBACK(on_ssh_terminal_type_focus_out), state);
  attach_row(page, 4, "terminal_type", state->ssh_terminal_type_entry);

  return page;
}

static GtkWidget *create_serial_page(SettingsWidgetState *state) {
  GtkWidget *page = create_page_grid("settings_serial_page");
  const SerialConnectionSettings settings =
      serial_connection_settings(state->draft_store);
  const gboolean device_sensitive = state->is_runtime ? FALSE : TRUE;

  state->serial_device_entry = gtk_entry_new();
  assign_accessible_id(state->serial_device_entry,
                       "settings_serial_device_entry");
  gtk_entry_set_text(GTK_ENTRY(state->serial_device_entry),
                     settings.device.c_str());
  gtk_widget_set_sensitive(state->serial_device_entry, device_sensitive);
  g_signal_connect(state->serial_device_entry, "changed",
                   G_CALLBACK(on_serial_device_changed), state);
  attach_row(page, 0, "device", state->serial_device_entry);

  state->serial_baudrate_spin =
      create_spin_button(150.0, 8000000.0, 1.0, 0,
                         "settings_serial_baudrate_spin");
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(state->serial_baudrate_spin),
                            static_cast<double>(settings.baudrate));
  g_signal_connect(state->serial_baudrate_spin, "value-changed",
                   G_CALLBACK(on_serial_baudrate_changed), state);
  attach_row(page, 1, "baudrate", state->serial_baudrate_spin);

  state->serial_bits_combo = create_combo_box("settings_serial_bits_combo");
  append_combo_option(state->serial_bits_combo, "5", "5");
  append_combo_option(state->serial_bits_combo, "6", "6");
  append_combo_option(state->serial_bits_combo, "7", "7");
  append_combo_option(state->serial_bits_combo, "8", "8");
  const std::string bits = std::to_string(settings.bits);
  gtk_combo_box_set_active_id(GTK_COMBO_BOX(state->serial_bits_combo),
                              bits.c_str());
  g_signal_connect(state->serial_bits_combo, "changed",
                   G_CALLBACK(on_serial_bits_changed), state);
  attach_row(page, 2, "bits", state->serial_bits_combo);

  state->serial_parity_combo =
      create_combo_box("settings_serial_parity_combo");
  append_combo_option(state->serial_parity_combo, "n", "n");
  append_combo_option(state->serial_parity_combo, "e", "e");
  append_combo_option(state->serial_parity_combo, "o", "o");
  const std::string parity = serial_parity_to_string(settings.parity);
  gtk_combo_box_set_active_id(GTK_COMBO_BOX(state->serial_parity_combo),
                              parity.c_str());
  g_signal_connect(state->serial_parity_combo, "changed",
                   G_CALLBACK(on_serial_parity_changed), state);
  attach_row(page, 3, "parity", state->serial_parity_combo);

  state->serial_stop_bit_combo =
      create_combo_box("settings_serial_stop_bit_combo");
  append_combo_option(state->serial_stop_bit_combo, "1", "1");
  append_combo_option(state->serial_stop_bit_combo, "2", "2");
  const std::string stop_bit = std::to_string(settings.stop_bit);
  gtk_combo_box_set_active_id(GTK_COMBO_BOX(state->serial_stop_bit_combo),
                              stop_bit.c_str());
  g_signal_connect(state->serial_stop_bit_combo, "changed",
                   G_CALLBACK(on_serial_stop_bit_changed), state);
  attach_row(page, 4, "stop_bit", state->serial_stop_bit_combo);

  state->serial_flow_control_combo =
      create_combo_box("settings_serial_flow_control_combo");
  append_combo_option(state->serial_flow_control_combo, "none", "none");
  append_combo_option(state->serial_flow_control_combo, "xon", "xon");
  append_combo_option(state->serial_flow_control_combo, "hard", "hard");
  const std::string flow_control =
      serial_flow_control_to_string(settings.flow_control);
  gtk_combo_box_set_active_id(
      GTK_COMBO_BOX(state->serial_flow_control_combo), flow_control.c_str());
  g_signal_connect(state->serial_flow_control_combo, "changed",
                   G_CALLBACK(on_serial_flow_control_changed), state);
  attach_row(page, 5, "flow_control", state->serial_flow_control_combo);

  state->serial_carrier_detect_combo =
      create_combo_box("settings_serial_carrier_detect_combo");
  append_combo_option(state->serial_carrier_detect_combo, "cd", "cd");
  append_combo_option(state->serial_carrier_detect_combo, "cts", "cts");
  append_combo_option(state->serial_carrier_detect_combo, "dsr", "dsr");
  const std::string carrier_detect =
      serial_carrier_detect_to_string(settings.carrier_detect);
  gtk_combo_box_set_active_id(
      GTK_COMBO_BOX(state->serial_carrier_detect_combo),
      carrier_detect.c_str());
  g_signal_connect(state->serial_carrier_detect_combo, "changed",
                   G_CALLBACK(on_serial_carrier_detect_changed), state);
  attach_row(page, 6, "carrier_detect", state->serial_carrier_detect_combo);

  return page;
}

static GtkWidget *create_transfer_page(SettingsWidgetState *state) {
  GtkWidget *page = create_page_grid("settings_transfer_page");

  state->transfer_base_path_entry = gtk_entry_new();
  assign_accessible_id(state->transfer_base_path_entry,
                       "settings_transfer_base_path_entry");
  gtk_entry_set_text(GTK_ENTRY(state->transfer_base_path_entry),
                     transfer_base_path(state->draft_store).c_str());
  g_signal_connect(state->transfer_base_path_entry, "changed",
                   G_CALLBACK(on_transfer_base_path_changed), state);
  attach_row(page, 0, "base_path", state->transfer_base_path_entry);

  state->transfer_text_send_rate_spin =
      create_spin_button(1.0, 8000000.0, 1.0, 0,
                         "settings_transfer_text_send_rate_spin");
  gtk_spin_button_set_value(
      GTK_SPIN_BUTTON(state->transfer_text_send_rate_spin),
      static_cast<double>(
          transfer_text_send_bytes_per_second(state->draft_store)));
  g_signal_connect(state->transfer_text_send_rate_spin, "value-changed",
                   G_CALLBACK(on_transfer_text_send_rate_changed), state);
  attach_row(page, 1, "text_send_bytes_per_second",
             state->transfer_text_send_rate_spin);

  state->transfer_zmodem_autostart_combo =
      create_combo_box("settings_transfer_zmodem_autostart_combo");
  append_combo_option(state->transfer_zmodem_autostart_combo,
                      zmodem_autostart_default, "Default");
  append_combo_option(state->transfer_zmodem_autostart_combo,
                      zmodem_autostart_enabled, "Enabled");
  append_combo_option(state->transfer_zmodem_autostart_combo,
                      zmodem_autostart_disabled, "Disabled");
  gtk_combo_box_set_active_id(
      GTK_COMBO_BOX(state->transfer_zmodem_autostart_combo),
      zmodem_autostart_choice_id(state->draft_store));
  g_signal_connect(state->transfer_zmodem_autostart_combo, "changed",
                   G_CALLBACK(on_transfer_zmodem_autostart_changed), state);
  attach_row(page, 2, "zmodem_autostart",
             state->transfer_zmodem_autostart_combo);

  return page;
}

static GtkWidget *create_logging_page(SettingsWidgetState *state) {
  GtkWidget *page = create_page_grid("settings_logging_page");
  const TerminalLogSettings settings =
      terminal_log_settings(state->draft_store);

  state->log_enabled_check = gtk_check_button_new_with_label("enabled");
  assign_accessible_id(state->log_enabled_check,
                       "settings_log_enabled_check");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->log_enabled_check),
                               settings.enabled ? TRUE : FALSE);
  g_signal_connect(state->log_enabled_check, "toggled",
                   G_CALLBACK(on_log_enabled_toggled), state);
  attach_row(page, 0, "enabled", state->log_enabled_check);

  state->log_base_directory_entry = gtk_entry_new();
  assign_accessible_id(state->log_base_directory_entry,
                       "settings_log_base_directory_entry");
  gtk_entry_set_text(GTK_ENTRY(state->log_base_directory_entry),
                     settings.base_directory.c_str());
  g_signal_connect(state->log_base_directory_entry, "changed",
                   G_CALLBACK(on_log_base_directory_changed), state);
  attach_row(page, 1, "base_directory", state->log_base_directory_entry);

  state->log_file_name_format_entry = gtk_entry_new();
  assign_accessible_id(state->log_file_name_format_entry,
                       "settings_log_file_name_format_entry");
  gtk_entry_set_text(GTK_ENTRY(state->log_file_name_format_entry),
                     settings.file_name_format.c_str());
  g_signal_connect(state->log_file_name_format_entry, "changed",
                   G_CALLBACK(on_log_file_name_format_changed), state);
  attach_row(page, 2, "file_name_format",
             state->log_file_name_format_entry);

  state->log_mode_combo = create_combo_box("settings_log_mode_combo");
  append_combo_option(state->log_mode_combo, terminal_log_raw, "Raw");
  append_combo_option(state->log_mode_combo, terminal_log_cooked, "Cooked");
  gtk_combo_box_set_active_id(GTK_COMBO_BOX(state->log_mode_combo),
                              terminal_log_mode_to_string(settings.mode));
  g_signal_connect(state->log_mode_combo, "changed",
                   G_CALLBACK(on_log_mode_changed), state);
  attach_row(page, 3, "mode", state->log_mode_combo);

  return page;
}

static GtkWidget *create_button_box(SettingsWidgetState *state) {
  GtkWidget *button_box = gtk_button_box_new(GTK_ORIENTATION_HORIZONTAL);
  assign_accessible_id(button_box, "settings_action_row");
  gtk_button_box_set_layout(GTK_BUTTON_BOX(button_box), GTK_BUTTONBOX_END);
  gtk_box_set_spacing(GTK_BOX(button_box), 8);
  gtk_widget_set_margin_top(button_box, 12);
  gtk_widget_set_margin_bottom(button_box, 12);
  gtk_widget_set_margin_start(button_box, 12);
  gtk_widget_set_margin_end(button_box, 12);
  gtk_widget_set_valign(button_box, GTK_ALIGN_CENTER);

  state->apply_button = gtk_button_new_with_label("Apply");
  assign_accessible_id(state->apply_button, "settings_apply_button");
  gtk_widget_set_valign(state->apply_button, GTK_ALIGN_CENTER);
  g_signal_connect(state->apply_button, "clicked",
                   G_CALLBACK(on_apply_clicked), state);
  gtk_container_add(GTK_CONTAINER(button_box), state->apply_button);

  if (state->callbacks.save) {
    state->save_button = gtk_button_new_with_label("Save");
    assign_accessible_id(state->save_button, "settings_save_button");
    gtk_widget_set_valign(state->save_button, GTK_ALIGN_CENTER);
    g_signal_connect(state->save_button, "clicked",
                     G_CALLBACK(on_save_clicked), state);
    gtk_container_add(GTK_CONTAINER(button_box), state->save_button);
  }

  state->cancel_button = gtk_button_new_with_label("Cancel");
  assign_accessible_id(state->cancel_button, "settings_cancel_button");
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
  state->is_runtime = options.is_runtime;
  state->show_actions = options.show_actions;
  state->callbacks = std::move(options.callbacks);

  state->root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  assign_accessible_id(state->root, "settings_widget_root");

  state->notebook = gtk_notebook_new();
  assign_accessible_id(state->notebook, "settings_notebook");
  gtk_widget_set_vexpand(state->notebook, TRUE);
  gtk_widget_set_hexpand(state->notebook, TRUE);
  gtk_box_pack_start(GTK_BOX(state->root), state->notebook, TRUE, TRUE, 0);

  GtkWidget *general_page = create_general_page(state);
  gtk_notebook_append_page(GTK_NOTEBOOK(state->notebook), general_page,
                           create_tab_button(state, general_page, "General",
                                             "settings_general_tab"));

  GtkWidget *telnet_page = create_telnet_page(state);
  GtkWidget *telnet_tab =
      create_tab_button(state, telnet_page, "TELNET", "settings_telnet_tab");
  gtk_notebook_append_page(GTK_NOTEBOOK(state->notebook), telnet_page,
                           telnet_tab);
  gtk_widget_show_all(telnet_page);
  gtk_widget_show_all(telnet_tab);
  gtk_widget_set_no_show_all(telnet_page, TRUE);
  gtk_widget_set_no_show_all(telnet_tab, TRUE);
  state->connection_pages.push_back({
      .connection_type = telnet_connection_type,
      .page = telnet_page,
      .tab_label = telnet_tab,
  });

  GtkWidget *serial_page = create_serial_page(state);
  GtkWidget *serial_tab =
      create_tab_button(state, serial_page, "Serial", "settings_serial_tab");
  gtk_notebook_append_page(GTK_NOTEBOOK(state->notebook), serial_page,
                           serial_tab);
  gtk_widget_show_all(serial_page);
  gtk_widget_show_all(serial_tab);
  gtk_widget_set_no_show_all(serial_page, TRUE);
  gtk_widget_set_no_show_all(serial_tab, TRUE);
  state->connection_pages.push_back({
      .connection_type = serial_connection_type,
      .page = serial_page,
      .tab_label = serial_tab,
  });

  GtkWidget *ssh_page = create_ssh_page(state);
  GtkWidget *ssh_tab =
      create_tab_button(state, ssh_page, "SSH", "settings_ssh_tab");
  gtk_notebook_append_page(GTK_NOTEBOOK(state->notebook), ssh_page, ssh_tab);
  gtk_widget_show_all(ssh_page);
  gtk_widget_show_all(ssh_tab);
  gtk_widget_set_no_show_all(ssh_page, TRUE);
  gtk_widget_set_no_show_all(ssh_tab, TRUE);
  state->connection_pages.push_back({
      .connection_type = ssh_connection_type,
      .page = ssh_page,
      .tab_label = ssh_tab,
  });

  GtkWidget *terminal_page = create_terminal_page(state);
  gtk_notebook_append_page(GTK_NOTEBOOK(state->notebook), terminal_page,
                           create_tab_button(state, terminal_page, "Terminal",
                                             "settings_terminal_tab"));

  GtkWidget *transfer_page = create_transfer_page(state);
  gtk_notebook_append_page(GTK_NOTEBOOK(state->notebook), transfer_page,
                           create_tab_button(state, transfer_page, "Transfer",
                                             "settings_transfer_tab"));

  GtkWidget *logging_page = create_logging_page(state);
  gtk_notebook_append_page(GTK_NOTEBOOK(state->notebook), logging_page,
                           create_tab_button(state, logging_page, "Logging",
                                             "settings_logging_tab"));

  if (state->show_actions) {
    gtk_box_pack_start(GTK_BOX(state->root), create_button_box(state), FALSE,
                       FALSE, 0);
  }
  update_terminal_key_binding_validation(state);
  update_connection_pages(state);

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

static void update_default_connection_name(SettingsStore *store,
                                           const std::string &name) {
  const SettingKey key = general_name_setting_key();
  for (SettingEntry &entry : store->entries) {
    if (entry.definition.key.section != key.section ||
        entry.definition.key.name != key.name) {
      continue;
    }
    entry.definition.default_value = SettingValue{name};
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
  return state != nullptr && settings_store_is_dirty(state->draft_store);
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
  if (state->root != nullptr && gtk_widget_get_parent(state->root) == nullptr) {
    gtk_widget_destroy(state->root);
  }
  delete state;
}

} // namespace elder_terms
