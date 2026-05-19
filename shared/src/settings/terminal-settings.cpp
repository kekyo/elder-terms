#include <elder-terms/settings/terminal-settings.h>

#include <cmath>

namespace elder_terms {

static constexpr glong default_terminal_width = 80;
static constexpr glong default_terminal_height = 24;
static constexpr bool default_terminal_auto_close = true;
static constexpr char terminal_section[] = "terminal";
static constexpr char terminal_width_key[] = "width";
static constexpr char terminal_height_key[] = "height";
static constexpr char terminal_zoom_key[] = "zoom";
static constexpr char terminal_auto_close_key[] = "auto_close";

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

static SettingKey terminal_key(const char *name) {
  return make_setting_key(terminal_section, name);
}

TerminalDisplaySettings default_terminal_display_settings(gdouble default_zoom) {
  return {
      .width = default_terminal_width,
      .height = default_terminal_height,
      .zoom = default_zoom,
  };
}

SettingKey terminal_width_setting_key() {
  return terminal_key(terminal_width_key);
}

SettingKey terminal_height_setting_key() {
  return terminal_key(terminal_height_key);
}

SettingKey terminal_zoom_setting_key() {
  return terminal_key(terminal_zoom_key);
}

SettingKey terminal_auto_close_setting_key() {
  return terminal_key(terminal_auto_close_key);
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
          .key = terminal_zoom_setting_key(),
          .default_value = SettingValue{terminal_defaults.zoom},
          .validate = validate_zoom,
      },
      {
          .key = terminal_auto_close_setting_key(),
          .default_value = SettingValue{default_terminal_auto_close},
          .validate = nullptr,
      },
  };
}

TerminalDisplaySettings terminal_display_settings(const SettingsStore &store) {
  return {
      .width = static_cast<glong>(setting_integer_value_or_default(
          store, terminal_width_setting_key(), default_terminal_width)),
      .height = static_cast<glong>(setting_integer_value_or_default(
          store, terminal_height_setting_key(), default_terminal_height)),
      .zoom = setting_double_value_or_default(
          store, terminal_zoom_setting_key(), gdouble{1.0}),
  };
}

bool terminal_auto_close(const SettingsStore &store) {
  return setting_boolean_value_or_default(
      store, terminal_auto_close_setting_key(), default_terminal_auto_close);
}

} // namespace elder_terms
