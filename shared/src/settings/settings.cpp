#include <elder-terms/settings.h>

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

static void load_settings_file(SettingsStore *store,
                               const std::filesystem::path &path,
                               std::vector<std::string> *warnings) {
  std::error_code exists_error;
  const bool exists = std::filesystem::exists(path, exists_error);
  if (!exists && !exists_error) {
    warnings->push_back("Warning: configuration file not found: " +
                        path_string(path));
    return;
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
    return;
  }

  load_settings_store_from_key_file(store, key_file, warnings);
  g_key_file_unref(key_file);
}

SettingsStore create_default_settings(TerminalDisplaySettings terminal_defaults) {
  std::vector<SettingDefinition> definitions;
  append_definitions(&definitions, general_setting_definitions());
  append_definitions(&definitions, terminal_setting_definitions(terminal_defaults));
  append_definitions(&definitions, local_shell_connection_setting_definitions());
  append_definitions(&definitions, telnet_connection_setting_definitions());
  append_definitions(&definitions, serial_connection_setting_definitions());
  append_definitions(&definitions, transfer_setting_definitions());
  return create_settings_store(std::move(definitions));
}

SettingsLoadResult
load_settings(const SettingsLoadOptions &options, gdouble default_terminal_zoom) {
  SettingsLoadResult result{
      .store = create_default_settings(
          default_terminal_display_settings(default_terminal_zoom)),
      .warnings = {},
  };

  if (options.config_path.has_value()) {
    load_settings_file(&result.store, options.config_path.value(),
                       &result.warnings);
  }
  if (options.startup_config_path.has_value()) {
    load_settings_file(&result.store, options.startup_config_path.value(),
                       &result.warnings);
  }

  if (general_settings_select_telnet_connection(result.store)) {
    append_telnet_connection_warnings(result.store, &result.warnings);
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

TerminalConnectionProfile
terminal_connection_profile(const SettingsStore &store) {
  if (general_settings_select_telnet_connection(store)) {
    return {
        .kind = TerminalConnectionKind::telnet,
        .settings = telnet_connection_settings(store),
    };
  }

  if (general_settings_select_serial_connection(store)) {
    return {
        .kind = TerminalConnectionKind::serial,
        .settings = serial_connection_settings(store),
    };
  }

  return {
      .kind = TerminalConnectionKind::local_shell,
      .settings = LocalShellConnectionSettings{},
  };
}

} // namespace elder_terms
