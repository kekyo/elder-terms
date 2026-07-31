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
  transfer_tab,
  logging_tab,
  apply,
  save,
  cancel,
  reset,
  enabled,
  disabled,
  no_color,
  custom_color,
  press_key_combination,
  clear_key_binding,
  use_global_default,
  use_built_in_default,
};

const char *settings_ui_text(SettingsUiText text);

std::string setting_label(const SettingKey &key);

std::string setting_choice_label(const SettingKey &key,
                                 const std::string &value);

std::string inherited_setting_label(const std::string &value,
                                    SettingValueSource source);

std::string settings_validation_message(const std::string &reason);

} // namespace elder_terms
