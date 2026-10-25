// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

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
static constexpr char terminal_section[] = "terminal";
static constexpr char terminal_width_key[] = "width";
static constexpr char terminal_height_key[] = "height";
static constexpr char terminal_scrollback_lines_key[] = "scrollback_lines";
static constexpr char terminal_ambiguous_width_key[] = "ambiguous_width";
static constexpr char terminal_block_copy_trim_trailing_spaces_key[] =
    "block_copy_trim_trailing_spaces";
static constexpr char terminal_font_size_dip_key[] = "font_size_dip";
static constexpr char terminal_font_families_key[] = "font_families";
static constexpr char terminal_accessibility_enabled_key[] = "accessibility_enabled";
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

static bool validate_ambiguous_width(const SettingValue &value, std::string *reason) {
  const auto *width = std::get_if<std::string>(&value);
  if (width == nullptr || (*width != "narrow" && *width != "wide" && *width != "font")) {
    *reason = "must be narrow, wide, or font";
    return false;
  }
  return true;
}

static bool validate_font_size_dip(const SettingValue &value, std::string *reason) {
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

static bool validate_terminal_cursor_shape(const SettingValue &value, std::string *reason) {
  const auto *text = std::get_if<std::string>(&value);
  if (!text || (*text != "block" && *text != "bar" && *text != "underline")) {
    *reason = "must be block, bar, or underline";
    return false;
  }
  return true;
}

static bool validate_color_filter(const SettingValue &value, std::string *reason) {
  const auto *text = std::get_if<std::string>(&value);
  if (text && (*text == "none" || *text == "oklch" || *text == "reverse-lightness" || *text == "reverse-contrast" || *text == "reverse-roles" || *text == "reverse-cam16" || *text == "reverse-palette")) return true;
  *reason = "must be none, oklch, reverse-lightness, reverse-contrast, reverse-roles, reverse-cam16, or reverse-palette";
  return false;
}

static bool validate_color_vision(const SettingValue &value, std::string *reason) {
  const auto *text = std::get_if<std::string>(&value);
  if (text && (*text == "none" || *text == "protan" || *text == "deutan" || *text == "tritan")) return true;
  *reason = "must be none, protan, deutan, or tritan";
  return false;
}

static bool validate_background_mode(const SettingValue &value, std::string *reason) {
  const auto *text = std::get_if<std::string>(&value);
  if (text && (*text == "opaque" || *text == "transparent" || *text == "blur")) return true;
  *reason = "must be opaque, transparent, or blur";
  return false;
}

static bool validate_background_percent(const SettingValue &value, std::string *reason) {
  const auto *number = std::get_if<gint64>(&value);
  if (number && *number >= 0 && *number <= 100) return true;
  *reason = "must be an integer from 0 to 100";
  return false;
}

static bool validate_background_tint(const SettingValue &value, std::string *reason) {
  const auto *text = std::get_if<std::string>(&value);
  if (text && parse_rgb_color(*text)) return true;
  *reason = "must be #RRGGBB";
  return false;
}

static bool validate_background_image_path(const SettingValue &value, std::string *reason) {
  const auto *text = std::get_if<std::string>(&value);
  if (text && (text->empty() || (g_path_is_absolute(text->c_str()) &&
      g_utf8_validate(text->c_str(), text->size(), nullptr) &&
      std::none_of(text->begin(), text->end(), [](unsigned char c) { return c < 32 || c == 127; })))) return true;
  *reason = "must be empty or an absolute local image path without control characters";
  return false;
}

static bool validate_background_image_fit(const SettingValue &value, std::string *reason) {
  const auto *text = std::get_if<std::string>(&value);
  if (text && (*text == "cover" || *text == "contain" || *text == "stretch")) return true;
  *reason = "must be cover, contain, or stretch";
  return false;
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

TerminalDisplaySettings default_terminal_display_settings(gdouble default_font_size_dip) {
  return {
      .width = default_terminal_width,
      .height = default_terminal_height,
      .scrollback_lines = default_terminal_scrollback_lines,
      .font_size_dip = default_font_size_dip,
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

SettingKey terminal_ambiguous_width_setting_key() {
  return terminal_key(terminal_ambiguous_width_key);
}

TerminalAmbiguousWidth terminal_ambiguous_width(const SettingsStore &store) {
  const auto width = setting_string_value_or_default(store, terminal_ambiguous_width_setting_key(), "font");
  if (width == "narrow") return TerminalAmbiguousWidth::narrow;
  return width == "wide" ? TerminalAmbiguousWidth::wide : TerminalAmbiguousWidth::font;
}

SettingKey terminal_block_copy_trim_trailing_spaces_setting_key() {
  return terminal_key(terminal_block_copy_trim_trailing_spaces_key);
}

SettingKey terminal_font_size_dip_setting_key() {
  return terminal_key(terminal_font_size_dip_key);
}

SettingKey terminal_font_families_setting_key() {
  return terminal_key(terminal_font_families_key);
}

SettingKey terminal_cursor_shape_setting_key() { return terminal_key("cursor_shape"); }
SettingKey terminal_cursor_blink_setting_key() { return terminal_key("cursor_blink"); }
SettingKey terminal_color_filter_setting_key() { return terminal_key("color_filter"); }
TerminalColorFilter terminal_color_filter(const SettingsStore &store) {
  const auto value = setting_string_value_or_default(store, terminal_color_filter_setting_key(), "none");
  if (value == "reverse-palette") return TerminalColorFilter::reverse_palette;
  if (value == "reverse-cam16") return TerminalColorFilter::reverse_cam16;
  if (value == "reverse-roles") return TerminalColorFilter::reverse_roles;
  if (value == "reverse-contrast") return TerminalColorFilter::reverse_contrast;
  if (value == "reverse-lightness") return TerminalColorFilter::reverse_lightness;
  return value == "oklch" ? TerminalColorFilter::oklch : TerminalColorFilter::none;
}

SettingKey terminal_color_vision_setting_key() { return terminal_key("color_vision"); }
SettingKey terminal_color_vision_strength_setting_key() { return terminal_key("color_vision_strength"); }
TerminalColorVisionSettings terminal_color_vision_settings(const SettingsStore &store) {
  const auto profile = setting_string_value_or_default(store, terminal_color_vision_setting_key(), "none");
  return {profile == "protan" ? TerminalColorVision::protan : profile == "deutan" ? TerminalColorVision::deutan :
      profile == "tritan" ? TerminalColorVision::tritan : TerminalColorVision::none,
      static_cast<int>(setting_integer_value_or_default(store, terminal_color_vision_strength_setting_key(), 100))};
}

TerminalCursorSettings terminal_cursor_settings(const SettingsStore &store) {
  const auto shape = setting_string_value_or_default(store, terminal_cursor_shape_setting_key(), "block");
  return {shape == "bar" ? TerminalCursorShape::bar :
          shape == "underline" ? TerminalCursorShape::underline : TerminalCursorShape::block,
      setting_boolean_value_or_default(store, terminal_cursor_blink_setting_key(), true)};
}

SettingKey terminal_background_mode_setting_key() { return terminal_key("background_mode"); }
SettingKey terminal_background_opacity_setting_key() { return terminal_key("background_opacity"); }
SettingKey terminal_background_tint_color_setting_key() { return terminal_key("background_tint_color"); }
SettingKey terminal_background_tint_opacity_setting_key() { return terminal_key("background_tint_opacity"); }
SettingKey terminal_background_image_setting_key() { return terminal_key("background_image"); }
SettingKey terminal_background_image_opacity_setting_key() { return terminal_key("background_image_opacity"); }
SettingKey terminal_background_image_fit_setting_key() { return terminal_key("background_image_fit"); }

TerminalBackgroundImageSettings terminal_background_image_settings(const SettingsStore &store) {
  const auto fit = setting_string_value_or_default(store, terminal_background_image_fit_setting_key(), "cover");
  return {setting_string_value_or_default(store, terminal_background_image_setting_key(), ""),
      static_cast<int>(setting_integer_value_or_default(store, terminal_background_image_opacity_setting_key(), 100)),
      fit == "contain" ? TerminalBackgroundImageFit::contain : fit == "stretch" ? TerminalBackgroundImageFit::stretch :
      TerminalBackgroundImageFit::cover};
}

TerminalBackgroundSettings terminal_background_settings(const SettingsStore &store) {
  const auto mode = setting_string_value_or_default(store, terminal_background_mode_setting_key(), "opaque");
  return {mode == "blur" ? TerminalBackgroundMode::blur : mode == "transparent"
          ? TerminalBackgroundMode::transparent : TerminalBackgroundMode::opaque,
      static_cast<int>(setting_integer_value_or_default(store, terminal_background_opacity_setting_key(), 80)),
      parse_rgb_color(setting_string_value_or_default(store, terminal_background_tint_color_setting_key(), "#000000"))
          .value_or(RgbColor{0, 0, 0}),
      static_cast<int>(setting_integer_value_or_default(store, terminal_background_tint_opacity_setting_key(), 0))};
}

SettingKey terminal_accessibility_enabled_setting_key() {
  return terminal_key(terminal_accessibility_enabled_key);
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
      {.key = terminal_ambiguous_width_setting_key(), .default_value = SettingValue{std::string("font")},
       .validate = validate_ambiguous_width},
      {.key = terminal_cursor_shape_setting_key(), .default_value = SettingValue{std::string("block")},
       .validate = validate_terminal_cursor_shape},
      {.key = terminal_cursor_blink_setting_key(), .default_value = SettingValue{true}, .validate = nullptr},
      {.key = terminal_color_filter_setting_key(), .default_value = SettingValue{std::string("none")}, .validate = validate_color_filter},
      {.key = terminal_color_vision_setting_key(), .default_value = SettingValue{std::string("none")}, .validate = validate_color_vision},
      {.key = terminal_color_vision_strength_setting_key(), .default_value = SettingValue{gint64{100}}, .validate = validate_background_percent},
      {.key = terminal_background_mode_setting_key(), .default_value = SettingValue{std::string("opaque")}, .validate = validate_background_mode},
      {.key = terminal_background_opacity_setting_key(), .default_value = SettingValue{gint64{80}}, .validate = validate_background_percent},
      {.key = terminal_background_tint_color_setting_key(), .default_value = SettingValue{std::string("#000000")}, .validate = validate_background_tint},
      {.key = terminal_background_tint_opacity_setting_key(), .default_value = SettingValue{gint64{0}}, .validate = validate_background_percent},
      {.key = terminal_background_image_setting_key(), .default_value = SettingValue{std::string()}, .validate = validate_background_image_path},
      {.key = terminal_background_image_opacity_setting_key(), .default_value = SettingValue{gint64{100}}, .validate = validate_background_percent},
      {.key = terminal_background_image_fit_setting_key(), .default_value = SettingValue{std::string("cover")}, .validate = validate_background_image_fit},
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
          .key = terminal_block_copy_trim_trailing_spaces_setting_key(),
          .default_value = SettingValue{true},
          .validate = nullptr,
      },
      {
          .key = terminal_font_size_dip_setting_key(),
          .default_value = SettingValue{terminal_defaults.font_size_dip},
          .validate = validate_font_size_dip,
      },
      {
          .key = terminal_font_families_setting_key(),
          .default_value = SettingValue{std::vector<std::string>{}},
          .validate = validate_font_families,
          .normalize = normalize_font_families,
      },
      {
          .key = terminal_accessibility_enabled_setting_key(),
          .default_value = SettingValue{false},
          .validate = nullptr,
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
      .font_size_dip = setting_double_value_or_default(
          store, terminal_font_size_dip_setting_key(), gdouble{12.0}),
  };
}

bool terminal_block_copy_trim_trailing_spaces(const SettingsStore &store) {
  return setting_boolean_value_or_default(
      store, terminal_block_copy_trim_trailing_spaces_setting_key(), true);
}

TerminalFontFamilies terminal_font_families(const SettingsStore &store) {
  auto families = std::get<std::vector<std::string>>(setting_value_or_default(
      store, terminal_font_families_setting_key(),
      SettingValue{std::vector<std::string>{}}));
  if (families.empty()) families = {"Noto Sans Mono", "Monospace"};
  return {.families = std::move(families)};
}

bool terminal_accessibility_enabled(const SettingsStore &store) {
  return setting_boolean_value_or_default(
      store, terminal_accessibility_enabled_setting_key(), false);
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
