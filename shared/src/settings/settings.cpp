#include <elder-terms/settings.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <system_error>
#include <utility>

#include <elder-terms/settings/general-settings.h>

namespace elder_terms {

static void append_definitions(std::vector<SettingDefinition> *definitions,
                               std::vector<SettingDefinition> next) {
  for (SettingDefinition &definition : next) {
    definitions->push_back(std::move(definition));
  }
}

static std::string glib_error_message(GError *error) {
  if (error == nullptr || error->message == nullptr) {
    return "unknown error";
  }
  return error->message;
}

static std::string path_string(const std::filesystem::path &path) {
  return path.empty() ? std::string("<empty>") : path.string();
}

static std::string connection_name_from_path(
    const std::optional<std::filesystem::path> &path) {
  if (!path.has_value()) {
    return {};
  }
  return path->stem().string();
}

static std::string
default_connection_name(const SettingsLoadOptions &options) {
  std::string name = connection_name_from_path(options.config_path);
  if (name.empty()) {
    name = connection_name_from_path(options.startup_config_path);
  }
  return name.empty() ? std::string("elder-terms") : name;
}

static std::string trim_ascii_whitespace(std::string value) {
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

static void set_key_file_value(GKeyFile *key_file, const SettingEntry &entry) {
  const char *section = entry.definition.key.section.c_str();
  const char *name = entry.definition.key.name.c_str();

  if (const auto *integer = std::get_if<gint64>(&entry.value)) {
    g_key_file_set_integer(key_file, section, name,
                           static_cast<gint>(*integer));
    return;
  }
  if (const auto *number = std::get_if<gdouble>(&entry.value)) {
    g_key_file_set_double(key_file, section, name, *number);
    return;
  }
  if (const auto *text = std::get_if<std::string>(&entry.value)) {
    g_key_file_set_string(key_file, section, name, text->c_str());
    return;
  }

  g_key_file_set_boolean(key_file, section, name,
                         std::get<bool>(entry.value) ? TRUE : FALSE);
}

static bool load_settings_file(SettingsStore *store,
                               const std::filesystem::path &path,
                               std::vector<std::string> *warnings) {
  std::error_code exists_error;
  const bool exists = std::filesystem::exists(path, exists_error);
  if (!exists && !exists_error) {
    warnings->push_back("Warning: configuration file not found: " +
                        path_string(path));
    return false;
  }

  GError *error = nullptr;
  GKeyFile *key_file = g_key_file_new();
  if (!g_key_file_load_from_file(key_file, path.c_str(), G_KEY_FILE_NONE,
                                 &error)) {
    warnings->push_back("Warning: failed to read configuration file " +
                        path_string(path) + ": " +
                        glib_error_message(error));
    g_clear_error(&error);
    g_key_file_unref(key_file);
    return false;
  }

  load_settings_store_from_key_file(store, key_file, warnings);
  g_key_file_unref(key_file);
  return true;
}

static void resolve_terminal_key_binding_conflict(
    SettingsStore *store, std::vector<std::string> *warnings) {
  if (!terminal_key_bindings_conflict(*store)) {
    return;
  }

  warnings->push_back(
      "Warning: conflicting terminal key bindings [terminal] zoom_in_key "
      "and [terminal] zoom_out_key; using defaults");
  clear_explicit_setting_value(store, terminal_zoom_in_key_setting_key());
  clear_explicit_setting_value(store, terminal_zoom_out_key_setting_key());
}

SettingsStore create_default_settings(TerminalDisplaySettings terminal_defaults,
                                      std::string default_connection_name) {
  std::vector<SettingDefinition> definitions;
  append_definitions(
      &definitions,
      general_setting_definitions(std::move(default_connection_name)));
  append_definitions(&definitions, terminal_log_setting_definitions());
  append_definitions(&definitions, terminal_setting_definitions(terminal_defaults));
  append_definitions(&definitions, local_shell_connection_setting_definitions());
  append_definitions(&definitions, telnet_connection_setting_definitions());
  append_definitions(&definitions, ssh_connection_setting_definitions());
  append_definitions(&definitions, serial_connection_setting_definitions());
  append_definitions(&definitions, transfer_setting_definitions());
  return create_settings_store(std::move(definitions));
}

SettingsLoadResult
load_settings(const SettingsLoadOptions &options, gdouble default_terminal_zoom) {
  SettingsLoadResult result{
      .store = create_default_settings(
          default_terminal_display_settings(default_terminal_zoom),
          default_connection_name(options)),
      .loaded = true,
      .warnings = {},
  };

  if (options.config_path.has_value()) {
    result.loaded = load_settings_file(&result.store,
                                       options.config_path.value(),
                                       &result.warnings) &&
                    result.loaded;
  }
  if (options.startup_config_path.has_value()) {
    result.loaded = load_settings_file(&result.store,
                                       options.startup_config_path.value(),
                                       &result.warnings) &&
                    result.loaded;
  }

  resolve_terminal_key_binding_conflict(&result.store, &result.warnings);

  if (general_settings_select_telnet_connection(result.store)) {
    append_telnet_connection_warnings(result.store, &result.warnings);
  }
  if (general_settings_select_ssh_connection(result.store)) {
    append_ssh_connection_warnings(result.store, &result.warnings);
  }
  if (general_settings_select_serial_connection(result.store)) {
    append_serial_connection_warnings(result.store, &result.warnings);
  }
  return result;
}

SettingsSaveResult save_settings(const SettingsStore &store,
                                 const std::filesystem::path &config_path) {
  SettingsSaveResult result{
      .saved = false,
      .warnings = {},
  };
  if (config_path.empty()) {
    result.warnings.push_back(
        "Warning: failed to save configuration file <empty>: empty path");
    return result;
  }

  GKeyFile *key_file = g_key_file_new();
  for (const SettingEntry &entry : store.entries) {
    if (entry.value != entry.definition.default_value ||
        (entry.definition.save_when_loaded && entry.loaded)) {
      set_key_file_value(key_file, entry);
    }
  }

  GError *error = nullptr;
  gsize length = 0;
  gchar *data = g_key_file_to_data(key_file, &length, &error);
  if (error != nullptr) {
    result.warnings.push_back("Warning: failed to save configuration file " +
                              path_string(config_path) + ": " +
                              glib_error_message(error));
    g_clear_error(&error);
    g_key_file_unref(key_file);
    return result;
  }

  const gchar *contents = data == nullptr ? "" : data;
  if (!g_file_set_contents(config_path.c_str(), contents,
                           static_cast<gssize>(length), &error)) {
    result.warnings.push_back("Warning: failed to save configuration file " +
                              path_string(config_path) + ": " +
                              glib_error_message(error));
    g_clear_error(&error);
    g_free(data);
    g_key_file_unref(key_file);
    return result;
  }

  result.saved = true;
  g_free(data);
  g_key_file_unref(key_file);
  return result;
}

const char *terminal_backspace_code_to_string(TerminalBackspaceCode code) {
  return code == TerminalBackspaceCode::bs ? "bs" : "del";
}

const char *terminal_cursor_key_mode_to_string(TerminalCursorKeyMode mode) {
  return mode == TerminalCursorKeyMode::adm3 ? "adm3" : "normal";
}

TerminalTextSettings
default_terminal_text_settings(TerminalConnectionKind kind) {
  TerminalTextSettings settings;
  if (kind == TerminalConnectionKind::telnet ||
      kind == TerminalConnectionKind::serial) {
    settings.backspace_code = TerminalBackspaceCode::bs;
  }
  if (kind == TerminalConnectionKind::serial) {
    settings.cursor_key_mode = TerminalCursorKeyMode::adm3;
  }
  return settings;
}

TerminalTextSettings terminal_text_settings(const SettingsStore &store,
                                            TerminalConnectionKind kind) {
  TerminalTextSettings settings = default_terminal_text_settings(kind);
  if (setting_has_explicit_value(store, terminal_encoding_setting_key())) {
    settings.encoding = trim_ascii_whitespace(setting_string_value_or_default(
        store, terminal_encoding_setting_key(), settings.encoding));
  }
  if (setting_has_explicit_value(store,
                                 terminal_backspace_code_setting_key())) {
    const std::string configured = setting_string_value_or_default(
        store, terminal_backspace_code_setting_key(),
        terminal_backspace_code_to_string(settings.backspace_code));
    settings.backspace_code = configured == "bs" ? TerminalBackspaceCode::bs
                                                  : TerminalBackspaceCode::del;
  }
  if (setting_has_explicit_value(store,
                                 terminal_cursor_key_mode_setting_key())) {
    const std::string configured = setting_string_value_or_default(
        store, terminal_cursor_key_mode_setting_key(),
        terminal_cursor_key_mode_to_string(settings.cursor_key_mode));
    settings.cursor_key_mode = configured == "adm3"
                                   ? TerminalCursorKeyMode::adm3
                                   : TerminalCursorKeyMode::normal;
  }
  return settings;
}

TerminalConnectionProfile
terminal_connection_profile(const SettingsStore &store) {
  if (general_settings_select_telnet_connection(store)) {
    return {
        .name = general_connection_name(store),
        .kind = TerminalConnectionKind::telnet,
        .settings = telnet_connection_settings(store),
        .text_settings =
            terminal_text_settings(store, TerminalConnectionKind::telnet),
    };
  }

  if (general_settings_select_serial_connection(store)) {
    return {
        .name = general_connection_name(store),
        .kind = TerminalConnectionKind::serial,
        .settings = serial_connection_settings(store),
        .text_settings =
            terminal_text_settings(store, TerminalConnectionKind::serial),
    };
  }

  if (general_settings_select_ssh_connection(store)) {
    return {
        .name = general_connection_name(store),
        .kind = TerminalConnectionKind::ssh,
        .settings = ssh_connection_settings(store),
        .text_settings =
            terminal_text_settings(store, TerminalConnectionKind::ssh),
    };
  }

  return {
      .name = general_connection_name(store),
      .kind = TerminalConnectionKind::local_shell,
      .settings = LocalShellConnectionSettings{},
      .text_settings =
          terminal_text_settings(store, TerminalConnectionKind::local_shell),
  };
}

} // namespace elder_terms
