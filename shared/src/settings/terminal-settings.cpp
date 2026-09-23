#include <elder-terms/settings/terminal-settings.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
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
static constexpr bool default_terminal_show_border = false;
static constexpr gint64 default_terminal_border_width = 4;
static constexpr gint64 minimum_terminal_border_width = 1;
static constexpr gint64 maximum_terminal_border_width = 1000;
static constexpr char terminal_section[] = "terminal";
static constexpr char terminal_width_key[] = "width";
static constexpr char terminal_height_key[] = "height";
static constexpr char terminal_scrollback_lines_key[] = "scrollback_lines";
static constexpr char terminal_zoom_key[] = "zoom";
static constexpr char terminal_indicator_color_key[] = "indicator_color";
static constexpr char terminal_indicator_off_color_key[] = "indicator_off_color";
static constexpr char terminal_font_families_key[] = "font_families";
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

bool terminal_font_families_are_valid(
    const std::vector<std::string> &families, std::string *reason) {
  std::vector<std::string> normalized;
  for (const auto &family : families) {
    if (!g_utf8_validate(family.data(), static_cast<gssize>(family.size()),
                         nullptr)) {
      *reason = "font family names must be valid UTF-8 without NUL characters";
      return false;
    }
    for (const char *character = family.c_str(); *character != '\0';
         character = g_utf8_next_char(character)) {
      if (g_unichar_iscntrl(g_utf8_get_char(character)) || *character == ',') {
        *reason = "font family names must not contain commas or control characters";
        return false;
      }
    }
    const auto name = trim_ascii_whitespace(family);
    if (name.empty() || std::find(normalized.begin(), normalized.end(), name) !=
                            normalized.end()) {
      *reason = "font family names must be non-empty and unique";
      return false;
    }
    normalized.push_back(name);
  }
  return true;
}

static std::optional<RgbColor> parse_indicator_color(const std::string &text) {
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
                          parse_indicator_color(*text).has_value())) {
    return true;
  }
  *reason = "must be default or #RRGGBB";
  return false;
}

static bool validate_font_families(const SettingValue &value,
                                   std::string *reason) {
  return terminal_font_families_are_valid(
      std::get<std::vector<std::string>>(value), reason);
}

static void normalize_font_families(SettingValue &value) {
  for (auto &family : std::get<std::vector<std::string>>(value)) {
    family = trim_ascii_whitespace(family);
  }
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

SettingKey terminal_indicator_color_setting_key() {
  return terminal_key(terminal_indicator_color_key);
}

std::optional<RgbColor> terminal_indicator_color(const SettingsStore &store) {
  return parse_indicator_color(setting_string_value_or_default(
      store, terminal_indicator_color_setting_key(), "default"));
}

SettingKey terminal_indicator_off_color_setting_key() {
  return terminal_key(terminal_indicator_off_color_key);
}

std::optional<RgbColor> terminal_indicator_off_color(const SettingsStore &store) {
  return parse_indicator_color(setting_string_value_or_default(
      store, terminal_indicator_off_color_setting_key(), "default"));
}

SettingKey terminal_font_families_setting_key() {
  return terminal_key(terminal_font_families_key);
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
          .key = terminal_indicator_color_setting_key(),
          .default_value = SettingValue{std::string("default")},
          .validate = validate_indicator_color,
      },
      {
          .key = terminal_font_families_setting_key(),
          .default_value = SettingValue{std::vector<std::string>{}},
          .validate = validate_font_families,
          .normalize = normalize_font_families,
      },
      {
          .key = terminal_indicator_off_color_setting_key(),
          .default_value = SettingValue{std::string("default")},
          .validate = validate_indicator_color,
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
  auto families = std::get<std::vector<std::string>>(setting_value_or_default(
      store, terminal_font_families_setting_key(),
      SettingValue{std::vector<std::string>{}}));
  if (families.empty()) families = {"Noto Sans Mono", "Monospace"};
  return {.families = std::move(families)};
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
