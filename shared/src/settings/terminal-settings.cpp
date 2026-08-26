#include <elder-terms/settings/terminal-settings.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <string>

#include <iconv.h>

namespace elder_terms {

static constexpr glong default_terminal_width = 80;
static constexpr glong default_terminal_height = 24;
static constexpr glong default_terminal_scrollback_lines = 10000;
static constexpr gint64 minimum_terminal_scrollback_lines = 1000;
static constexpr gint64 maximum_terminal_scrollback_lines = 100000;
static constexpr bool default_terminal_auto_close = true;
static constexpr bool default_terminal_show_border = false;
static constexpr gint64 default_terminal_border_width = 4;
static constexpr gint64 minimum_terminal_border_width = 1;
static constexpr gint64 maximum_terminal_border_width = 1000;
static constexpr char terminal_section[] = "terminal";
static constexpr char terminal_width_key[] = "width";
static constexpr char terminal_height_key[] = "height";
static constexpr char terminal_scrollback_lines_key[] = "scrollback_lines";
static constexpr char terminal_zoom_key[] = "zoom";
static constexpr char terminal_font_primary_family_key[] =
    "font_primary_family";
static constexpr char terminal_font_fallback_family_key[] =
    "font_fallback_family";
static constexpr char terminal_font_default_value[] = "default";
static constexpr char default_terminal_font_primary_family[] =
    "Noto Sans Mono";
static constexpr char default_terminal_font_fallback_family[] = "Monospace";
static constexpr char terminal_auto_close_key[] = "auto_close";
static constexpr char terminal_show_border_key[] = "show_border";
static constexpr char terminal_border_width_key[] = "border_width";
static constexpr char terminal_bell_sound_key[] = "bell_sound";
static constexpr char terminal_zoom_in_key_name[] = "zoom_in_key";
static constexpr char terminal_zoom_out_key_name[] = "zoom_out_key";
static constexpr char terminal_send_break_key_name[] = "send_break_key";
static constexpr char terminal_encoding_key[] = "encoding";
static constexpr char terminal_backspace_code_key[] = "backspace_code";
static constexpr char terminal_cursor_key_mode_key[] = "cursor_key_mode";
static constexpr char terminal_return_code_key[] = "return_code";
static constexpr char default_terminal_zoom_in_key[] = "ctrl+equal";
static constexpr char default_terminal_zoom_out_key[] = "ctrl+minus";
static constexpr char default_terminal_send_break_key[] = "";
static constexpr char default_terminal_encoding[] = "UTF-8";
static constexpr char default_terminal_backspace_code[] = "auto";
static constexpr char default_terminal_cursor_key_mode[] = "normal";
static constexpr char default_terminal_return_code[] = "auto";
static constexpr char default_terminal_bell_sound[] = "default";
static constexpr char terminal_bell_sound_validation_reason[] =
    "must be default or an existing absolute .oga, .ogg, or .wav file";

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

static bool iconv_conversion_is_available(const char *to, const char *from) {
  iconv_t converter = iconv_open(to, from);
  if (converter == reinterpret_cast<iconv_t>(-1)) {
    return false;
  }
  iconv_close(converter);
  return true;
}

static bool validate_positive_integer(const SettingValue &value,
                                      std::string *reason) {
  const auto *integer = std::get_if<gint64>(&value);
  if (integer == nullptr || *integer <= 0) {
    *reason = "must be a positive integer";
    return false;
  }
  return true;
}

static bool validate_zoom(const SettingValue &value, std::string *reason) {
  const auto *number = std::get_if<gdouble>(&value);
  if (number == nullptr || *number <= 0.0 || !std::isfinite(*number)) {
    *reason = "must be a positive finite number";
    return false;
  }
  return true;
}

static bool validate_scrollback_lines(const SettingValue &value,
                                      std::string *reason) {
  const auto *integer = std::get_if<gint64>(&value);
  if (integer == nullptr || *integer < minimum_terminal_scrollback_lines ||
      *integer > maximum_terminal_scrollback_lines) {
    *reason = "must be an integer between 1000 and 100000";
    return false;
  }
  return true;
}

static bool validate_border_width(const SettingValue &value,
                                  std::string *reason) {
  const auto *integer = std::get_if<gint64>(&value);
  if (integer == nullptr || *integer < minimum_terminal_border_width ||
      *integer > maximum_terminal_border_width) {
    *reason = "must be an integer between 1 and 1000";
    return false;
  }
  return true;
}

static bool validate_font_family(const SettingValue &value,
                                 std::string *reason) {
  const auto *text = std::get_if<std::string>(&value);
  if (text == nullptr || trim_ascii_whitespace(*text).empty()) {
    *reason = "must be a non-empty font family name";
    return false;
  }
  return true;
}

static std::optional<std::string>
terminal_font_family_value(const std::string &value,
                           const char *default_family) {
  const std::string normalized = trim_ascii_whitespace(value);
  if (normalized.empty() || normalized == terminal_font_default_value) {
    return std::string(default_family);
  }
  return normalized;
}

static bool validate_key_binding(const SettingValue &value,
                                 std::string *reason) {
  const auto *text = std::get_if<std::string>(&value);
  if (text == nullptr) {
    *reason = "must be a string";
    return false;
  }
  const KeyBindingParseResult parsed = parse_key_binding(*text);
  if (!parsed.error.empty()) {
    *reason = parsed.error;
    return false;
  }
  return true;
}

static bool validate_terminal_encoding(const SettingValue &value,
                                       std::string *reason) {
  const auto *text = std::get_if<std::string>(&value);
  if (text == nullptr) {
    *reason = "must be a string";
    return false;
  }
  return terminal_encoding_name_is_valid(*text, reason);
}

static bool validate_terminal_backspace_code(const SettingValue &value,
                                             std::string *reason) {
  const auto *text = std::get_if<std::string>(&value);
  if (text == nullptr ||
      (*text != "auto" && *text != "bs" && *text != "del")) {
    *reason = "must be auto, bs, or del";
    return false;
  }
  return true;
}

static bool validate_terminal_cursor_key_mode(const SettingValue &value,
                                              std::string *reason) {
  const auto *text = std::get_if<std::string>(&value);
  if (text == nullptr || (*text != "normal" && *text != "trs80")) {
    *reason = "must be normal or trs80";
    return false;
  }
  return true;
}

static bool validate_terminal_return_code(const SettingValue &value,
                                          std::string *reason) {
  const auto *text = std::get_if<std::string>(&value);
  if (text == nullptr ||
      (*text != "auto" && *text != "cr" && *text != "lf" &&
       *text != "crlf")) {
    *reason = "must be auto, cr, lf, or crlf";
    return false;
  }
  return true;
}

static bool validate_terminal_bell_sound(const SettingValue &value,
                                         std::string *reason) {
  const auto *text = std::get_if<std::string>(&value);
  if (text == nullptr) {
    *reason = terminal_bell_sound_validation_reason;
    return false;
  }
  return terminal_bell_sound_is_valid(*text, reason);
}

static SettingKey terminal_key(const char *name) {
  return make_setting_key(terminal_section, name);
}

TerminalDisplaySettings default_terminal_display_settings(gdouble default_zoom) {
  return {
      .width = default_terminal_width,
      .height = default_terminal_height,
      .scrollback_lines = default_terminal_scrollback_lines,
      .zoom = default_zoom,
  };
}

SettingKey terminal_width_setting_key() {
  return terminal_key(terminal_width_key);
}

SettingKey terminal_height_setting_key() {
  return terminal_key(terminal_height_key);
}

SettingKey terminal_scrollback_lines_setting_key() {
  return terminal_key(terminal_scrollback_lines_key);
}

SettingKey terminal_zoom_setting_key() {
  return terminal_key(terminal_zoom_key);
}

SettingKey terminal_font_primary_family_setting_key() {
  return terminal_key(terminal_font_primary_family_key);
}

SettingKey terminal_font_fallback_family_setting_key() {
  return terminal_key(terminal_font_fallback_family_key);
}

SettingKey terminal_auto_close_setting_key() {
  return terminal_key(terminal_auto_close_key);
}

SettingKey terminal_show_border_setting_key() {
  return terminal_key(terminal_show_border_key);
}

SettingKey terminal_border_width_setting_key() {
  return terminal_key(terminal_border_width_key);
}

SettingKey terminal_bell_sound_setting_key() {
  return terminal_key(terminal_bell_sound_key);
}

SettingKey terminal_zoom_in_key_setting_key() {
  return terminal_key(terminal_zoom_in_key_name);
}

SettingKey terminal_zoom_out_key_setting_key() {
  return terminal_key(terminal_zoom_out_key_name);
}

SettingKey terminal_send_break_key_setting_key() {
  return terminal_key(terminal_send_break_key_name);
}

SettingKey terminal_encoding_setting_key() {
  return terminal_key(terminal_encoding_key);
}

SettingKey terminal_backspace_code_setting_key() {
  return terminal_key(terminal_backspace_code_key);
}

SettingKey terminal_cursor_key_mode_setting_key() {
  return terminal_key(terminal_cursor_key_mode_key);
}

SettingKey terminal_return_code_setting_key() {
  return terminal_key(terminal_return_code_key);
}

bool terminal_encoding_name_is_valid(const std::string &encoding,
                                     std::string *reason) {
  std::string ignored_reason;
  std::string *failure_reason = reason == nullptr ? &ignored_reason : reason;
  const std::string normalized = trim_ascii_whitespace(encoding);
  if (normalized.empty()) {
    *failure_reason = "must not be empty";
    return false;
  }
  if (!iconv_conversion_is_available("UTF-8", normalized.c_str()) ||
      !iconv_conversion_is_available(normalized.c_str(), "UTF-8")) {
    *failure_reason = "is not supported by iconv in both directions";
    return false;
  }
  failure_reason->clear();
  return true;
}

std::vector<std::string> terminal_encoding_choices() {
  static const std::vector<std::string> choices = []() {
    static constexpr std::array<const char *, 14> candidates = {
        "UTF-8",      "ASCII",        "ISO-8859-1", "ISO-8859-15",
        "WINDOWS-1252", "WINDOWS-1251", "KOI8-R",     "CP437",
        "SHIFT-JIS",  "CP932",        "EUC-JP",      "GB18030",
        "BIG5",       "EUC-KR",
    };
    std::vector<std::string> supported;
    for (const char *candidate : candidates) {
      std::string reason;
      if (terminal_encoding_name_is_valid(candidate, &reason)) {
        supported.emplace_back(candidate);
      }
    }
    return supported;
  }();
  return choices;
}

bool terminal_bell_sound_is_valid(const std::string &value,
                                  std::string *reason) {
  std::string ignored_reason;
  std::string *failure_reason = reason == nullptr ? &ignored_reason : reason;
  if (value.find('\0') != std::string::npos) {
    *failure_reason = "contains a NUL byte";
    return false;
  }
  if (value == default_terminal_bell_sound) {
    failure_reason->clear();
    return true;
  }

  const std::filesystem::path path(value);
  std::string extension = path.extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  std::error_code error;
  if (!path.is_absolute() ||
      (extension != ".oga" && extension != ".ogg" && extension != ".wav") ||
      !std::filesystem::is_regular_file(path, error) || error) {
    *failure_reason = terminal_bell_sound_validation_reason;
    return false;
  }
  failure_reason->clear();
  return true;
}

std::vector<SettingDefinition>
terminal_setting_definitions(TerminalDisplaySettings terminal_defaults) {
  return {
      {
          .key = terminal_width_setting_key(),
          .default_value =
              SettingValue{static_cast<gint64>(terminal_defaults.width)},
          .validate = validate_positive_integer,
      },
      {
          .key = terminal_height_setting_key(),
          .default_value =
              SettingValue{static_cast<gint64>(terminal_defaults.height)},
          .validate = validate_positive_integer,
      },
      {
          .key = terminal_scrollback_lines_setting_key(),
          .default_value =
              SettingValue{static_cast<gint64>(
                  terminal_defaults.scrollback_lines)},
          .validate = validate_scrollback_lines,
      },
      {
          .key = terminal_zoom_setting_key(),
          .default_value = SettingValue{terminal_defaults.zoom},
          .validate = validate_zoom,
      },
      {
          .key = terminal_font_primary_family_setting_key(),
          .default_value = SettingValue{std::string{}},
          .validate = validate_font_family,
      },
      {
          .key = terminal_font_fallback_family_setting_key(),
          .default_value = SettingValue{std::string{}},
          .validate = validate_font_family,
      },
      {
          .key = terminal_auto_close_setting_key(),
          .default_value = SettingValue{default_terminal_auto_close},
          .validate = nullptr,
      },
      {
          .key = terminal_show_border_setting_key(),
          .default_value = SettingValue{default_terminal_show_border},
          .validate = nullptr,
      },
      {
          .key = terminal_border_width_setting_key(),
          .default_value = SettingValue{default_terminal_border_width},
          .validate = validate_border_width,
      },
      {
          .key = terminal_bell_sound_setting_key(),
          .default_value =
              SettingValue{std::string(default_terminal_bell_sound)},
          .validate = validate_terminal_bell_sound,
      },
      {
          .key = terminal_zoom_in_key_setting_key(),
          .default_value = SettingValue{std::string(default_terminal_zoom_in_key)},
          .validate = validate_key_binding,
      },
      {
          .key = terminal_zoom_out_key_setting_key(),
          .default_value =
              SettingValue{std::string(default_terminal_zoom_out_key)},
          .validate = validate_key_binding,
      },
      {
          .key = terminal_send_break_key_setting_key(),
          .default_value =
              SettingValue{std::string(default_terminal_send_break_key)},
          .validate = validate_key_binding,
      },
      {
          .key = terminal_encoding_setting_key(),
          .default_value =
              SettingValue{std::string(default_terminal_encoding)},
          .validate = validate_terminal_encoding,
      },
      {
          .key = terminal_backspace_code_setting_key(),
          .default_value =
              SettingValue{std::string(default_terminal_backspace_code)},
          .validate = validate_terminal_backspace_code,
      },
      {
          .key = terminal_cursor_key_mode_setting_key(),
          .default_value =
              SettingValue{std::string(default_terminal_cursor_key_mode)},
          .validate = validate_terminal_cursor_key_mode,
      },
      {
          .key = terminal_return_code_setting_key(),
          .default_value =
              SettingValue{std::string(default_terminal_return_code)},
          .validate = validate_terminal_return_code,
      },
  };
}

TerminalDisplaySettings terminal_display_settings(const SettingsStore &store) {
  return {
      .width = static_cast<glong>(setting_integer_value_or_default(
          store, terminal_width_setting_key(), default_terminal_width)),
      .height = static_cast<glong>(setting_integer_value_or_default(
          store, terminal_height_setting_key(), default_terminal_height)),
      .scrollback_lines = static_cast<glong>(setting_integer_value_or_default(
          store, terminal_scrollback_lines_setting_key(),
          default_terminal_scrollback_lines)),
      .zoom = setting_double_value_or_default(
          store, terminal_zoom_setting_key(), gdouble{1.0}),
  };
}

TerminalFontFamilies terminal_font_families(const SettingsStore &store) {
  return {
      .primary_family = terminal_font_family_value(
          setting_string_value_or_default(
              store, terminal_font_primary_family_setting_key(), ""),
          default_terminal_font_primary_family),
      .fallback_family = terminal_font_family_value(
          setting_string_value_or_default(
              store, terminal_font_fallback_family_setting_key(), ""),
          default_terminal_font_fallback_family),
  };
}

bool terminal_auto_close(const SettingsStore &store) {
  return setting_boolean_value_or_default(
      store, terminal_auto_close_setting_key(), default_terminal_auto_close);
}

bool terminal_show_border(const SettingsStore &store) {
  return setting_boolean_value_or_default(
      store, terminal_show_border_setting_key(), default_terminal_show_border);
}

gint terminal_border_width(const SettingsStore &store) {
  return static_cast<gint>(setting_integer_value_or_default(
      store, terminal_border_width_setting_key(),
      default_terminal_border_width));
}

TerminalBellSettings terminal_bell_settings(const SettingsStore &store) {
  const std::string value = setting_string_value_or_default(
      store, terminal_bell_sound_setting_key(), default_terminal_bell_sound);
  return {
      .sound_file = value == default_terminal_bell_sound
                        ? std::optional<std::filesystem::path>{}
                        : std::optional<std::filesystem::path>{value},
  };
}

std::string terminal_zoom_in_key(const SettingsStore &store) {
  return setting_string_value_or_default(store,
                                         terminal_zoom_in_key_setting_key(),
                                         default_terminal_zoom_in_key);
}

std::string terminal_zoom_out_key(const SettingsStore &store) {
  return setting_string_value_or_default(store,
                                         terminal_zoom_out_key_setting_key(),
                                         default_terminal_zoom_out_key);
}

std::string terminal_send_break_key(const SettingsStore &store) {
  return setting_string_value_or_default(
      store, terminal_send_break_key_setting_key(),
      default_terminal_send_break_key);
}

TerminalKeyBindings terminal_key_bindings(const SettingsStore &store) {
  const KeyBindingParseResult zoom_in =
      parse_key_binding(terminal_zoom_in_key(store));
  const KeyBindingParseResult zoom_out =
      parse_key_binding(terminal_zoom_out_key(store));
  const KeyBindingParseResult send_break =
      parse_key_binding(terminal_send_break_key(store));
  return {
      .zoom_in = zoom_in.binding,
      .zoom_out = zoom_out.binding,
      .send_break = send_break.binding,
  };
}

bool terminal_key_bindings_conflict(const SettingsStore &store) {
  const TerminalKeyBindings bindings = terminal_key_bindings(store);
  const auto conflict = [](const std::optional<KeyBinding> &left,
                           const std::optional<KeyBinding> &right) {
    return left.has_value() && right.has_value() &&
           key_bindings_equal(*left, *right);
  };
  return conflict(bindings.zoom_in, bindings.zoom_out) ||
         conflict(bindings.zoom_in, bindings.send_break) ||
         conflict(bindings.zoom_out, bindings.send_break);
}

} // namespace elder_terms
