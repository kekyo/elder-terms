#include <elder-terms/settings-widget.h>

#include <algorithm>
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
static constexpr char zmodem_autostart_default[] = "default";
static constexpr char zmodem_autostart_enabled[] = "enabled";
static constexpr char zmodem_autostart_disabled[] = "disabled";

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
  GtkWidget *general_type_combo = nullptr;
  GtkWidget *terminal_width_spin = nullptr;
  GtkWidget *terminal_height_spin = nullptr;
  GtkWidget *terminal_zoom_spin = nullptr;
  GtkWidget *terminal_auto_close_check = nullptr;
  KeyBindingInputWidgetState *terminal_zoom_in_key_input = nullptr;
  KeyBindingInputWidgetState *terminal_zoom_out_key_input = nullptr;
  GtkWidget *telnet_address_entry = nullptr;
  GtkWidget *telnet_port_spin = nullptr;
  GtkWidget *serial_device_entry = nullptr;
  GtkWidget *serial_baudrate_spin = nullptr;
  GtkWidget *serial_bits_combo = nullptr;
  GtkWidget *serial_parity_combo = nullptr;
  GtkWidget *serial_stop_bit_combo = nullptr;
  GtkWidget *serial_flow_control_combo = nullptr;
  GtkWidget *serial_carrier_detect_combo = nullptr;
  GtkWidget *transfer_base_path_entry = nullptr;
  GtkWidget *transfer_zmodem_autostart_combo = nullptr;
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

static void notify_changed(SettingsWidgetState *state) {
  if (!state->synchronizing && state->callbacks.changed) {
    state->callbacks.changed();
  }
}

static void update_action_sensitivity(SettingsWidgetState *state) {
  const gboolean sensitive =
      terminal_key_binding_inputs_valid(state) ? TRUE : FALSE;
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

static void sync_widgets_from_draft(SettingsWidgetState *state) {
  const TerminalDisplaySettings display =
      terminal_display_settings(state->draft_store);
  const TelnetConnectionSettings telnet =
      telnet_connection_settings(state->draft_store);
  const SerialConnectionSettings serial =
      serial_connection_settings(state->draft_store);

  if (state->general_type_combo != nullptr) {
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(state->general_type_combo),
                                connection_type_value(state->draft_store)
                                    .c_str());
  }
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
  if (state->transfer_zmodem_autostart_combo != nullptr) {
    gtk_combo_box_set_active_id(
        GTK_COMBO_BOX(state->transfer_zmodem_autostart_combo),
        zmodem_autostart_choice_id(state->draft_store));
  }
  if (state->notebook != nullptr) {
    update_connection_pages(state);
  }
}

static void sync_draft_from_widgets(SettingsWidgetState *state) {
  update_general_type_from_widget(state);
  update_terminal_width_from_widget(state);
  update_terminal_height_from_widget(state);
  update_terminal_zoom_from_widget(state);
  update_terminal_auto_close_from_widget(state);
  update_terminal_zoom_in_key_from_widget(state);
  update_terminal_zoom_out_key_from_widget(state);
  update_telnet_address_from_widget(state);
  update_telnet_port_from_widget(state);
  update_serial_device_from_widget(state);
  update_serial_baudrate_from_widget(state);
  update_serial_bits_from_widget(state);
  update_serial_parity_from_widget(state);
  update_serial_stop_bit_from_widget(state);
  update_serial_flow_control_from_widget(state);
  update_serial_carrier_detect_from_widget(state);
  update_transfer_base_path_from_widget(state);
  update_transfer_zmodem_autostart_from_widget(state);
}

static void on_general_type_changed(GtkComboBox *, gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  update_general_type_from_widget(state);
  notify_changed(state);
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

static void on_transfer_zmodem_autostart_changed(GtkComboBox *,
                                                 gpointer data) {
  auto *state = static_cast<SettingsWidgetState *>(data);
  update_transfer_zmodem_autostart_from_widget(state);
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
  state->general_type_combo =
      create_combo_box("settings_general_type_combo");
  append_combo_option(state->general_type_combo, local_connection_type,
                      "Local");
  append_combo_option(state->general_type_combo, telnet_connection_type,
                      "TELNET");
  append_combo_option(state->general_type_combo, serial_connection_type,
                      "Serial");
  gtk_combo_box_set_active_id(GTK_COMBO_BOX(state->general_type_combo),
                              connection_type_value(state->draft_store).c_str());
  gtk_widget_set_sensitive(state->general_type_combo,
                           state->is_runtime ? FALSE : TRUE);
  g_signal_connect(state->general_type_combo, "changed",
                   G_CALLBACK(on_general_type_changed), state);
  attach_row(page, 0, "type", state->general_type_combo);
  return page;
}

static GtkWidget *create_terminal_page(SettingsWidgetState *state) {
  GtkWidget *page = create_page_grid("settings_terminal_page");
  const TerminalDisplaySettings display =
      terminal_display_settings(state->draft_store);

  state->terminal_width_spin =
      create_spin_button(1.0, 1000.0, 1.0, 0, "settings_terminal_width_spin");
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(state->terminal_width_spin),
                            static_cast<double>(display.width));
  g_signal_connect(state->terminal_width_spin, "value-changed",
                   G_CALLBACK(on_terminal_width_changed), state);
  attach_row(page, 0, "width", state->terminal_width_spin);

  state->terminal_height_spin =
      create_spin_button(1.0, 1000.0, 1.0, 0, "settings_terminal_height_spin");
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(state->terminal_height_spin),
                            static_cast<double>(display.height));
  g_signal_connect(state->terminal_height_spin, "value-changed",
                   G_CALLBACK(on_terminal_height_changed), state);
  attach_row(page, 1, "height", state->terminal_height_spin);

  state->terminal_zoom_spin =
      create_spin_button(0.1, 5.0, 0.1, 2, "settings_terminal_zoom_spin");
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(state->terminal_zoom_spin),
                            display.zoom);
  g_signal_connect(state->terminal_zoom_spin, "value-changed",
                   G_CALLBACK(on_terminal_zoom_changed), state);
  attach_row(page, 2, "zoom", state->terminal_zoom_spin);

  state->terminal_auto_close_check =
      gtk_check_button_new_with_label("enabled");
  assign_accessible_id(state->terminal_auto_close_check,
                       "settings_terminal_auto_close_check");
  gtk_toggle_button_set_active(
      GTK_TOGGLE_BUTTON(state->terminal_auto_close_check),
      terminal_auto_close(state->draft_store) ? TRUE : FALSE);
  g_signal_connect(state->terminal_auto_close_check, "toggled",
                   G_CALLBACK(on_terminal_auto_close_toggled), state);
  attach_row(page, 3, "auto_close", state->terminal_auto_close_check);

  state->terminal_zoom_in_key_input = create_key_binding_input_widget({
      .text = terminal_zoom_in_key(state->draft_store),
      .accessible_id = "settings_terminal_zoom_in_key_entry",
      .changed = [state]() { on_terminal_key_binding_changed(state); },
  });
  attach_row(page, 4, "zoom_in_key",
             key_binding_input_widget_root(
                 state->terminal_zoom_in_key_input));

  state->terminal_zoom_out_key_input = create_key_binding_input_widget({
      .text = terminal_zoom_out_key(state->draft_store),
      .accessible_id = "settings_terminal_zoom_out_key_entry",
      .changed = [state]() { on_terminal_key_binding_changed(state); },
  });
  attach_row(page, 5, "zoom_out_key",
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
  attach_row(page, 1, "zmodem_autostart",
             state->transfer_zmodem_autostart_combo);

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

  GtkWidget *terminal_page = create_terminal_page(state);
  gtk_notebook_append_page(GTK_NOTEBOOK(state->notebook), terminal_page,
                           create_tab_button(state, terminal_page, "Terminal",
                                             "settings_terminal_tab"));

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

  GtkWidget *transfer_page = create_transfer_page(state);
  gtk_notebook_append_page(GTK_NOTEBOOK(state->notebook), transfer_page,
                           create_tab_button(state, transfer_page, "Transfer",
                                             "settings_transfer_tab"));

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

SettingsStore settings_widget_draft_store(const SettingsWidgetState *state) {
  return state == nullptr ? SettingsStore{} : state->draft_store;
}

bool settings_widget_is_dirty(const SettingsWidgetState *state) {
  return state != nullptr && settings_store_is_dirty(state->draft_store);
}

bool settings_widget_is_valid(const SettingsWidgetState *state) {
  return state != nullptr && terminal_key_binding_inputs_valid(state);
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
