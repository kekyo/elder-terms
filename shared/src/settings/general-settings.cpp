#include <elder-terms/settings/general-settings.h>

#include <algorithm>
#include <utility>

namespace elder_terms {

static constexpr char general_section[] = "general";
static constexpr char general_name_key[] = "name";
static constexpr char general_type_key[] = "type";
static constexpr char local_connection_type[] = "local";
static constexpr char telnet_connection_type[] = "telnet";
static constexpr char ssh_connection_type[] = "ssh";
static constexpr char serial_connection_type[] = "serial";

static bool validate_connection_type(const SettingValue &value,
                                     std::string *reason) {
  const auto *text = std::get_if<std::string>(&value);
  if (text == nullptr ||
      (*text != local_connection_type && *text != telnet_connection_type &&
       *text != ssh_connection_type &&
       *text != serial_connection_type)) {
    *reason = "must be local, telnet, ssh, or serial";
    return false;
  }
  return true;
}

static bool ascii_blank(const std::string &value) {
  return std::all_of(value.begin(), value.end(), [](unsigned char character) {
    return g_ascii_isspace(character) != FALSE;
  });
}

static const SettingEntry *connection_name_entry(const SettingsStore &store) {
  const SettingKey key = general_name_setting_key();
  const auto iterator =
      std::find_if(store.entries.begin(), store.entries.end(),
                   [&key](const SettingEntry &entry) {
                     return entry.definition.key.section == key.section &&
                            entry.definition.key.name == key.name;
                   });
  return iterator == store.entries.end() ? nullptr : &*iterator;
}

SettingKey general_name_setting_key() {
  return make_setting_key(general_section, general_name_key);
}

SettingKey general_type_setting_key() {
  return make_setting_key(general_section, general_type_key);
}

std::vector<SettingDefinition>
general_setting_definitions(std::string default_connection_name) {
  return {
      {
          .key = general_name_setting_key(),
          .default_value =
              SettingValue{std::move(default_connection_name)},
          .validate = nullptr,
          .save_when_loaded = true,
      },
      {
          .key = general_type_setting_key(),
          .default_value = SettingValue{std::string(local_connection_type)},
          .validate = validate_connection_type,
      },
  };
}

std::string general_connection_name(const SettingsStore &store) {
  const SettingEntry *entry = connection_name_entry(store);
  if (entry == nullptr) {
    return "elder-terms";
  }

  const auto *configured = std::get_if<std::string>(&entry->value);
  if (configured != nullptr && !ascii_blank(*configured)) {
    return *configured;
  }

  const auto *fallback =
      std::get_if<std::string>(&entry->definition.default_value);
  return fallback == nullptr || ascii_blank(*fallback) ? "elder-terms"
                                                       : *fallback;
}

bool general_settings_select_telnet_connection(const SettingsStore &store) {
  return setting_string_value_or_default(store, general_type_setting_key(),
                                         local_connection_type) ==
         telnet_connection_type;
}

bool general_settings_select_serial_connection(const SettingsStore &store) {
  return setting_string_value_or_default(store, general_type_setting_key(),
                                         local_connection_type) ==
         serial_connection_type;
}

bool general_settings_select_ssh_connection(const SettingsStore &store) {
  return setting_string_value_or_default(store, general_type_setting_key(),
                                         local_connection_type) ==
         ssh_connection_type;
}

} // namespace elder_terms
