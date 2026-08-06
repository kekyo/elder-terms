#pragma once

#include <string>

#include <elder-terms/settings/settings-store.h>

namespace elder_terms {

enum class SettingsUiText {
  general_tab,
  telnet_tab,
  serial_tab,
  ssh_tab,
  sftp_tab,
  terminal_tab,
  macro_tab,
  transfer_tab,
  logging_tab,
  apply,
  save,
  cancel,
  reset,
  select_primary_terminal_font,
  select_secondary_terminal_font,
  enabled,
  disabled,
  custom_font,
  no_color,
  custom_color,
  press_key_combination,
  clear_key_binding,
  use_global_default,
  use_built_in_default,
  macro_rules,
  macro_id,
  macro_regex,
  macro_action,
  macro_send,
  macro_command,
  macro_arguments,
  macro_add,
  macro_remove,
  macro_move_up,
  macro_move_down,
  macro_send_action,
  macro_command_action,
  macro_add_argument,
  macro_remove_argument,
  serial_no_device,
  serial_stable_id,
  serial_usb_serial,
  serial_current_node,
  unavailable,
};

const char *settings_ui_text(SettingsUiText text);

std::string setting_label(const SettingKey &key);

std::string setting_choice_label(const SettingKey &key,
                                 const std::string &value);

std::string inherited_setting_label(const std::string &value,
                                    SettingValueSource source);

std::string settings_validation_message(const std::string &reason);

} // namespace elder_terms
