// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include <elder-terms/settings/general-settings.h>

#include <algorithm>
#include <charconv>
#include <utility>

#include <glib.h>

namespace elder_terms {

static constexpr bool default_general_show_border = false;
static constexpr gint64 default_general_border_width = 4;
static constexpr gint64 minimum_general_border_width = 1;
static constexpr gint64 maximum_general_border_width = 1000;
static constexpr char general_show_border_key[] = "show_border";
static constexpr char general_border_width_key[] = "border_width";
static constexpr char general_section[] = "general";
static constexpr char general_auto_close_key[] = "auto_close";
static constexpr bool default_general_auto_close = true;
static constexpr char general_name_key[] = "name";
static constexpr char general_icon_key[] = "icon";
static constexpr char general_connection_id_key[] = "connection_id";
static constexpr char general_type_key[] = "type";
static constexpr char general_open_connection_key[] = "open_connection";
static constexpr char general_exterior_background_key[] =
    "exterior_background";
static constexpr char general_background_key[] = "background";
static constexpr char general_indicator_color_key[] = "indicator_color";
static constexpr char general_indicator_off_color_key[] = "indicator_off_color";
static constexpr char general_compact_mode_key[] = "compact_mode";
static constexpr bool default_general_compact_mode = false;
static constexpr char default_general_background[] = "none";
static constexpr char local_connection_type[] = "local";
static constexpr char telnet_connection_type[] = "telnet";
static constexpr char ssh_connection_type[] = "ssh";
static constexpr char serial_connection_type[] = "serial";
static constexpr char sftp_connection_type[] = "sftp";
static constexpr char ftp_connection_type[] = "ftp";
static constexpr char webdav_connection_type[] = "webdav";
static constexpr char smb_connection_type[] = "smb";

static bool validate_border_width(const SettingValue &value,
                                  std::string *reason) {
  const auto *integer = std::get_if<gint64>(&value);
  if (integer == nullptr || *integer < minimum_general_border_width ||
      *integer > maximum_general_border_width) {
    *reason = "must be an integer between 1 and 1000";
    return false;
  }
  return true;
}

SettingKey general_show_border_setting_key() {
  return make_setting_key(general_section, general_show_border_key);
}

SettingKey general_border_width_setting_key() {
  return make_setting_key(general_section, general_border_width_key);
}

bool general_show_border(const SettingsStore &store) {
  return setting_boolean_value_or_default(
      store, general_show_border_setting_key(), default_general_show_border);
}

gint general_border_width(const SettingsStore &store) {
  return static_cast<gint>(setting_integer_value_or_default(
      store, general_border_width_setting_key(),
      default_general_border_width));
}

static bool validate_connection_type(const SettingValue &value,
                                     std::string *reason) {
  const auto *text = std::get_if<std::string>(&value);
  if (text == nullptr ||
      (*text != local_connection_type && *text != telnet_connection_type &&
       *text != ssh_connection_type &&
       *text != serial_connection_type && *text != sftp_connection_type &&
       *text != ftp_connection_type && *text != webdav_connection_type &&
       *text != smb_connection_type)) {
    *reason = "must be local, telnet, ssh, serial, sftp, ftp, webdav, or smb";
    return false;
  }
  return true;
}

static bool validate_connection_hotkey(const SettingValue &value,
                                       std::string *reason) {
  const auto *text = std::get_if<std::string>(&value);
  if (text == nullptr) {
    *reason = "must be a string";
    return false;
  }
  return global_hotkey_text_is_valid(*text, reason);
}

std::optional<RgbColor> parse_rgb_color(const std::string &text) {
  if (text.size() != 7 || text.front() != '#') {
    return std::nullopt;
  }
  unsigned int packed = 0;
  const auto parsed = std::from_chars(text.data() + 1,
                                      text.data() + text.size(), packed, 16);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
    return std::nullopt;
  }
  return RgbColor{
      .red = static_cast<guint8>(packed >> 16),
      .green = static_cast<guint8>(packed >> 8),
      .blue = static_cast<guint8>(packed),
  };
}

static bool validate_indicator_color(const SettingValue &value,
                                      std::string *reason) {
  const auto *text = std::get_if<std::string>(&value);
  if (text != nullptr && (*text == "default" ||
                          parse_rgb_color(*text).has_value())) {
    return true;
  }
  *reason = "must be default or #RRGGBB";
  return false;
}

static bool is_ascii_hex_digit(char character) {
  return (character >= '0' && character <= '9') ||
         (character >= 'a' && character <= 'f') ||
         (character >= 'A' && character <= 'F');
}

static bool validate_general_color(const SettingValue &value,
                                   std::string *reason) {
  const auto *text = std::get_if<std::string>(&value);
  if (text == nullptr) {
    *reason = "must be a string";
    return false;
  }
  if (*text == default_general_background) {
    return true;
  }
  if (text->size() != 7 || text->front() != '#' ||
      !std::all_of(text->begin() + 1, text->end(), is_ascii_hex_digit)) {
    *reason = "must be none or an RGB color in #RRGGBB format";
    return false;
  }
  return true;
}

static guint8 hex_nibble(char character) {
  if (character >= '0' && character <= '9') {
    return static_cast<guint8>(character - '0');
  }
  if (character >= 'a' && character <= 'f') {
    return static_cast<guint8>(character - 'a' + 10);
  }
  return static_cast<guint8>(character - 'A' + 10);
}

static guint8 parse_hex_byte(char high, char low) {
  return static_cast<guint8>((hex_nibble(high) << 4) | hex_nibble(low));
}

static std::optional<RgbColor> parse_general_color(const std::string &text) {
  if (text == default_general_background) {
    return std::nullopt;
  }
  return RgbColor{
      .red = parse_hex_byte(text[1], text[2]),
      .green = parse_hex_byte(text[3], text[4]),
      .blue = parse_hex_byte(text[5], text[6]),
  };
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

SettingKey general_auto_close_setting_key() {
  return make_setting_key(general_section, general_auto_close_key);
}

bool general_auto_close(const SettingsStore &store) {
  return setting_boolean_value_or_default(
      store, general_auto_close_setting_key(), default_general_auto_close);
}

SettingKey general_name_setting_key() {
  return make_setting_key(general_section, general_name_key);
}

SettingKey general_icon_setting_key() {
  return make_setting_key(general_section, general_icon_key);
}

SettingKey general_connection_id_setting_key() {
  return make_setting_key(general_section, general_connection_id_key);
}

std::string general_connection_id(const SettingsStore &store) {
  const auto id = setting_string_value_or_default(
      store, general_connection_id_setting_key(), "");
  return g_uuid_string_is_valid(id.c_str()) ? id : std::string();
}

static bool validate_connection_id(const SettingValue &value,
                                   std::string *reason) {
  const auto *id = std::get_if<std::string>(&value);
  if (id != nullptr && (id->empty() || g_uuid_string_is_valid(id->c_str())))
    return true;
  *reason = "must be a UUID";
  return false;
}

std::string general_connection_icon(const SettingsStore &store) {
  return setting_string_value_or_default(store, general_icon_setting_key(),
                                         "default");
}

SettingKey general_type_setting_key() {
  return make_setting_key(general_section, general_type_key);
}

SettingKey general_open_connection_hotkey_setting_key() {
  return make_setting_key(general_section, general_open_connection_key);
}

SettingKey general_exterior_background_setting_key() {
  return make_setting_key(general_section,
                          general_exterior_background_key);
}

SettingKey general_background_setting_key() {
  return make_setting_key(general_section, general_background_key);
}

SettingKey general_indicator_color_setting_key() {
  return make_setting_key(general_section, general_indicator_color_key);
}

std::optional<RgbColor> general_indicator_color(const SettingsStore &store) {
  return parse_rgb_color(setting_string_value_or_default(
      store, general_indicator_color_setting_key(), "default"));
}

SettingKey general_indicator_off_color_setting_key() {
  return make_setting_key(general_section, general_indicator_off_color_key);
}

std::optional<RgbColor> general_indicator_off_color(const SettingsStore &store) {
  return parse_rgb_color(setting_string_value_or_default(
      store, general_indicator_off_color_setting_key(), "default"));
}

SettingKey general_compact_mode_setting_key() {
  return make_setting_key(general_section, general_compact_mode_key);
}

bool general_compact_mode(const SettingsStore &store) {
  return setting_boolean_value_or_default(
      store, general_compact_mode_setting_key(), default_general_compact_mode);
}

std::vector<SettingDefinition>
general_setting_definitions(std::string default_connection_name) {
  return {
      {
          .key = general_auto_close_setting_key(),
          .default_value = SettingValue{default_general_auto_close},
          .validate = nullptr,
      },
      {
          .key = general_name_setting_key(),
          .default_value =
              SettingValue{std::move(default_connection_name)},
          .validate = nullptr,
      },
      {
          .key = general_icon_setting_key(),
          .default_value = SettingValue{std::string("default")},
          .validate = nullptr,
      },
      {
          .key = general_connection_id_setting_key(),
          .default_value = SettingValue{std::string()},
          .validate = validate_connection_id,
      },
      {
          .key = general_type_setting_key(),
          .default_value = SettingValue{std::string(local_connection_type)},
          .validate = validate_connection_type,
      },
      {
          .key = general_open_connection_hotkey_setting_key(),
          .default_value = SettingValue{std::string()},
          .validate = validate_connection_hotkey,
      },
      {
          .key = general_exterior_background_setting_key(),
          .default_value =
              SettingValue{std::string(default_general_background)},
          .validate = validate_general_color,
      },
      {
          .key = general_background_setting_key(),
          .default_value =
              SettingValue{std::string(default_general_background)},
          .validate = validate_general_color,
      },
      {
          .key = general_indicator_color_setting_key(),
          .default_value = SettingValue{std::string("default")},
          .validate = validate_indicator_color,
      },
      {
          .key = general_indicator_off_color_setting_key(),
          .default_value = SettingValue{std::string("default")},
          .validate = validate_indicator_color,
      },
      {
          .key = general_show_border_setting_key(),
          .default_value = SettingValue{default_general_show_border},
          .validate = nullptr,
      },
      {
          .key = general_border_width_setting_key(),
          .default_value = SettingValue{default_general_border_width},
          .validate = validate_border_width,
      },
      {
          .key = general_compact_mode_setting_key(),
          .default_value = SettingValue{default_general_compact_mode},
          .validate = nullptr,
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

ConnectionKind general_connection_kind(const SettingsStore &store) {
  const std::string configured = setting_string_value_or_default(
      store, general_type_setting_key(), local_connection_type);
  if (configured == telnet_connection_type) {
    return ConnectionKind::telnet;
  }
  if (configured == ssh_connection_type) {
    return ConnectionKind::ssh;
  }
  if (configured == serial_connection_type) {
    return ConnectionKind::serial;
  }
  if (configured == sftp_connection_type) {
    return ConnectionKind::sftp;
  }
  if (configured == ftp_connection_type) {
    return ConnectionKind::ftp;
  }
  if (configured == webdav_connection_type) {
    return ConnectionKind::webdav;
  }
  if (configured == smb_connection_type) {
    return ConnectionKind::smb;
  }
  return ConnectionKind::local_shell;
}

std::string
general_open_connection_hotkey_text(const SettingsStore &store) {
  return setting_string_value_or_default(
      store, general_open_connection_hotkey_setting_key(), {});
}

std::optional<KeyBinding>
general_open_connection_hotkey(const SettingsStore &store) {
  return parse_key_binding(
             general_open_connection_hotkey_text(store))
      .binding;
}

GeneralColorSettings general_color_settings(const SettingsStore &store) {
  return {
      .exterior_background = parse_general_color(
          setting_string_value_or_default(
              store, general_exterior_background_setting_key(),
              default_general_background)),
      .background = parse_general_color(setting_string_value_or_default(
          store, general_background_setting_key(),
          default_general_background)),
  };
}

bool general_settings_select_telnet_connection(const SettingsStore &store) {
  return general_connection_kind(store) == ConnectionKind::telnet;
}

bool general_settings_select_serial_connection(const SettingsStore &store) {
  return general_connection_kind(store) == ConnectionKind::serial;
}

bool general_settings_select_ssh_connection(const SettingsStore &store) {
  return general_connection_kind(store) == ConnectionKind::ssh;
}

bool general_settings_select_sftp_connection(const SettingsStore &store) {
  return general_connection_kind(store) == ConnectionKind::sftp;
}

bool general_settings_select_ftp_connection(const SettingsStore &store) {
  return general_connection_kind(store) == ConnectionKind::ftp;
}

} // namespace elder_terms
