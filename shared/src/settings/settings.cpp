// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include <elder-terms/settings.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <string>
#include <system_error>
#include <set>
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

  if (entry.invalid_raw_value && !entry.validation_error.empty()) {
    g_key_file_set_value(key_file, section, name, entry.invalid_raw_value->c_str());
    return;
  }
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

  if (const auto *list = std::get_if<std::vector<std::string>>(&entry.value)) {
    std::vector<const char *> values;
    values.reserve(list->size() + 1);
    for (const auto &value : *list) {
      values.push_back(value.c_str());
    }
    values.push_back(nullptr);
    g_key_file_set_string_list(key_file, section, name, values.data(),
                               list->size());
    return;
  }
  g_key_file_set_boolean(key_file, section, name,
                         std::get<bool>(entry.value) ? TRUE : FALSE);
}

static std::string key_file_string(GKeyFile *key_file,
                                   const std::string &group,
                                   const char *key, GError **error) {
  gchar *value = g_key_file_get_string(key_file, group.c_str(), key, error);
  if (value == nullptr) {
    return {};
  }
  std::string result = value;
  g_free(value);
  return result;
}

static void warn_invalid_macro(std::vector<std::string> *warnings,
                               const std::string &group,
                               const std::string &reason) {
  warnings->push_back("Warning: invalid macro [" + group + "]: " + reason +
                      "; ignoring rule");
}

static std::vector<MacroRule>
load_macro_rules_from_key_file(GKeyFile *key_file,
                               std::vector<std::string> *warnings) {
  std::vector<MacroRule> rules;
  gsize group_count = 0;
  gchar **groups = g_key_file_get_groups(key_file, &group_count);
  for (gsize group_index = 0; group_index < group_count; ++group_index) {
    const std::string group = groups[group_index];
    constexpr char prefix[] = "macro.";
    if (!group.starts_with(prefix)) {
      continue;
    }

    MacroRule rule;
    rule.id = group.substr(sizeof(prefix) - 1);
    std::string reason;
    if (!macro_rule_id_is_valid(rule.id, &reason)) {
      warn_invalid_macro(warnings, group, reason);
      continue;
    }
    const bool duplicate =
        std::any_of(rules.begin(), rules.end(), [&rule](const MacroRule &other) {
          return other.id == rule.id;
        });
    if (duplicate) {
      warn_invalid_macro(warnings, group, "identifier is duplicated");
      continue;
    }

    const bool has_regex =
        g_key_file_has_key(key_file, group.c_str(), "regex", nullptr);
    const bool has_send =
        g_key_file_has_key(key_file, group.c_str(), "send", nullptr);
    const bool has_command =
        g_key_file_has_key(key_file, group.c_str(), "command", nullptr);
    const bool has_arguments =
        g_key_file_has_key(key_file, group.c_str(), "arguments", nullptr);
    if (!has_regex) {
      warn_invalid_macro(warnings, group, "missing regex");
      continue;
    }
    if (has_send == has_command) {
      warn_invalid_macro(
          warnings, group,
          "exactly one of send or command must be specified");
      continue;
    }
    if (has_send && has_arguments) {
      warn_invalid_macro(warnings, group,
                         "arguments may be used only with command");
      continue;
    }

    GError *error = nullptr;
    rule.pattern = key_file_string(key_file, group, "regex", &error);
    if (error != nullptr) {
      warn_invalid_macro(warnings, group, glib_error_message(error));
      g_clear_error(&error);
      continue;
    }

    if (has_send) {
      const std::string text =
          key_file_string(key_file, group, "send", &error);
      if (error == nullptr) {
        rule.action = MacroSendAction{.text = text};
      }
    } else {
      MacroCommandAction action{
          .command = key_file_string(key_file, group, "command", &error),
          .arguments = {},
      };
      if (error == nullptr && has_arguments) {
        gsize argument_count = 0;
        gchar **arguments = g_key_file_get_string_list(
            key_file, group.c_str(), "arguments", &argument_count, &error);
        if (arguments != nullptr) {
          action.arguments.reserve(argument_count);
          for (gsize index = 0; index < argument_count; ++index) {
            action.arguments.emplace_back(arguments[index]);
          }
          g_strfreev(arguments);
        }
      }
      if (error == nullptr) {
        rule.action = std::move(action);
      }
    }

    if (error != nullptr) {
      warn_invalid_macro(warnings, group, glib_error_message(error));
      g_clear_error(&error);
      continue;
    }
    if (!macro_rule_is_valid(rule, &reason)) {
      warn_invalid_macro(warnings, group, reason);
      continue;
    }
    rules.push_back(std::move(rule));
  }
  g_strfreev(groups);
  return rules;
}

static void set_key_file_macro_rule(GKeyFile *key_file,
                                    const MacroRule &rule) {
  const std::string group = "macro." + rule.id;
  g_key_file_set_string(key_file, group.c_str(), "regex",
                        rule.pattern.c_str());
  if (const auto *send = std::get_if<MacroSendAction>(&rule.action)) {
    g_key_file_set_string(key_file, group.c_str(), "send",
                          send->text.c_str());
    return;
  }

  const auto &command = std::get<MacroCommandAction>(rule.action);
  g_key_file_set_string(key_file, group.c_str(), "command",
                        command.command.c_str());
  if (command.arguments.empty()) {
    return;
  }
  std::vector<const gchar *> arguments;
  arguments.reserve(command.arguments.size());
  for (const std::string &argument : command.arguments) {
    arguments.push_back(argument.c_str());
  }
  g_key_file_set_string_list(key_file, group.c_str(), "arguments",
                             arguments.data(), arguments.size());
}

static void warn_invalid_hyperlink(std::vector<std::string> *warnings,
                                   const std::string &group,
                                   const std::string &reason) {
  warnings->push_back("Warning: invalid hyperlink [" + group + "]: " +
                      reason + "; ignoring rule");
}

static std::vector<HyperlinkActionRule>
load_hyperlink_rules_from_key_file(GKeyFile *key_file,
                                   std::vector<std::string> *warnings) {
  std::vector<HyperlinkActionRule> rules;
  gsize group_count = 0;
  gchar **groups = g_key_file_get_groups(key_file, &group_count);
  for (gsize group_index = 0; group_index < group_count; ++group_index) {
    const std::string group = groups[group_index];
    constexpr char prefix[] = "hyperlink.";
    if (!group.starts_with(prefix)) {
      continue;
    }

    HyperlinkActionRule rule;
    rule.id = group.substr(sizeof(prefix) - 1);
    std::string reason;
    if (!hyperlink_action_rule_id_is_valid(rule.id, &reason)) {
      warn_invalid_hyperlink(warnings, group, reason);
      continue;
    }
    const bool duplicate = std::any_of(
        rules.begin(), rules.end(), [&rule](const HyperlinkActionRule &other) {
          return other.id == rule.id;
        });
    if (duplicate) {
      warn_invalid_hyperlink(warnings, group, "identifier is duplicated");
      continue;
    }

    const bool has_regex =
        g_key_file_has_key(key_file, group.c_str(), "regex", nullptr);
    const bool has_command =
        g_key_file_has_key(key_file, group.c_str(), "command", nullptr);
    const bool has_arguments =
        g_key_file_has_key(key_file, group.c_str(), "arguments", nullptr);
    const bool has_source =
        g_key_file_has_key(key_file, group.c_str(), "source", nullptr);
    const bool has_path_validation = g_key_file_has_key(
        key_file, group.c_str(), "path_validation", nullptr);
    const bool has_path =
        g_key_file_has_key(key_file, group.c_str(), "path", nullptr);
    if (!has_regex) {
      warn_invalid_hyperlink(warnings, group, "missing regex");
      continue;
    }
    if (!has_command) {
      warn_invalid_hyperlink(warnings, group, "missing command");
      continue;
    }

    GError *error = nullptr;
    rule.pattern = key_file_string(key_file, group, "regex", &error);
    if (error == nullptr) {
      rule.command = key_file_string(key_file, group, "command", &error);
    }
    if (error == nullptr && has_arguments) {
      gsize argument_count = 0;
      gchar **arguments = g_key_file_get_string_list(
          key_file, group.c_str(), "arguments", &argument_count, &error);
      if (arguments != nullptr) {
        rule.arguments.reserve(argument_count);
        for (gsize index = 0; index < argument_count; ++index) {
          rule.arguments.emplace_back(arguments[index]);
        }
        g_strfreev(arguments);
      }
    }
    if (error == nullptr && has_source) {
      const std::string source =
          key_file_string(key_file, group, "source", &error);
      if (error == nullptr) {
        const std::optional<HyperlinkRecognitionSource> parsed =
            hyperlink_recognition_source_from_string(source);
        if (!parsed.has_value()) {
          warn_invalid_hyperlink(warnings, group,
                                 "unsupported source: " + source);
          continue;
        }
        rule.recognition_source = *parsed;
      }
    }
    if (error == nullptr && has_path_validation) {
      const std::string validation =
          key_file_string(key_file, group, "path_validation", &error);
      if (error == nullptr) {
        const std::optional<HyperlinkPathValidation> parsed =
            hyperlink_path_validation_from_string(validation);
        if (!parsed.has_value()) {
          warn_invalid_hyperlink(
              warnings, group,
              "unsupported path validation: " + validation);
          continue;
        }
        rule.path_validation = *parsed;
      }
    }
    if (error == nullptr && has_path) {
      rule.path_template =
          key_file_string(key_file, group, "path", &error);
    }
    if (error == nullptr && g_key_file_has_key(
        key_file, group.c_str(), "whole_token", nullptr)) {
      rule.whole_token = g_key_file_get_boolean(
          key_file, group.c_str(), "whole_token", &error) != FALSE;
    }
    if (error != nullptr) {
      warn_invalid_hyperlink(warnings, group, glib_error_message(error));
      g_clear_error(&error);
      continue;
    }
    if (!hyperlink_action_rule_is_valid(rule, &reason)) {
      warn_invalid_hyperlink(warnings, group, reason);
      continue;
    }
    rules.push_back(std::move(rule));
  }
  g_strfreev(groups);
  return rules;
}

static bool key_file_has_hyperlink_rule_group(GKeyFile *key_file) {
  bool found = false;
  gsize group_count = 0;
  gchar **groups = g_key_file_get_groups(key_file, &group_count);
  for (gsize group_index = 0; group_index < group_count; ++group_index) {
    if (std::string(groups[group_index]).starts_with("hyperlink.")) {
      found = true;
      break;
    }
  }
  g_strfreev(groups);
  return found;
}

static void load_hyperlink_settings_from_key_file(
    SettingsStore *store, GKeyFile *key_file,
    std::vector<std::string> *warnings) {
  const bool has_root = g_key_file_has_group(key_file, "hyperlink");
  const bool has_rules = key_file_has_hyperlink_rule_group(key_file);

  bool enabled = true;
  if (has_root &&
      g_key_file_has_key(key_file, "hyperlink", "enabled", nullptr)) {
    GError *error = nullptr;
    enabled = g_key_file_get_boolean(key_file, "hyperlink", "enabled",
                                     &error) != FALSE;
    if (error != nullptr) {
      warnings->push_back(
          "Warning: invalid hyperlink [hyperlink]: " +
          glib_error_message(error) + "; using enabled=true");
      g_clear_error(&error);
      enabled = true;
    }
  }

  store->hyperlink_actions_enabled = enabled;
  store->hyperlink_rules =
      load_hyperlink_rules_from_key_file(key_file, warnings);
  store->hyperlink_settings_configured = has_root || has_rules;
  store->hyperlink_settings_dirty = false;
}

static void set_key_file_hyperlink_rule(GKeyFile *key_file,
                                        const HyperlinkActionRule &rule) {
  const std::string group = "hyperlink." + rule.id;
  g_key_file_set_string(
      key_file, group.c_str(), "source",
      hyperlink_recognition_source_to_string(rule.recognition_source));
  g_key_file_set_string(key_file, group.c_str(), "regex",
                        rule.pattern.c_str());
  g_key_file_set_string(key_file, group.c_str(), "command",
                        rule.command.c_str());
  g_key_file_set_boolean(key_file, group.c_str(), "whole_token", rule.whole_token);
  if (!rule.arguments.empty()) {
    std::vector<const gchar *> arguments;
    arguments.reserve(rule.arguments.size());
    for (const std::string &argument : rule.arguments) {
      arguments.push_back(argument.c_str());
    }
    g_key_file_set_string_list(key_file, group.c_str(), "arguments",
                               arguments.data(), arguments.size());
  }

  if (rule.path_validation != HyperlinkPathValidation::none) {
    g_key_file_set_string(
        key_file, group.c_str(), "path_validation",
        hyperlink_path_validation_to_string(rule.path_validation));
    g_key_file_set_string(key_file, group.c_str(), "path",
                          rule.path_template.c_str());
  }
}

static bool load_settings_file(SettingsStore *store,
                               const std::filesystem::path &path,
                               bool missing_is_optional,
                               bool exclude_connection_settings,
                               std::vector<MacroRule> *macro_rules,
                               std::vector<std::string> *warnings) {
  std::error_code exists_error;
  const bool exists = std::filesystem::exists(path, exists_error);
  if (!exists && !exists_error) {
    if (!missing_is_optional) {
      warnings->push_back("Warning: configuration file not found: " +
                          path_string(path));
    }
    return missing_is_optional;
  }
  if (exists_error) {
    warnings->push_back("Warning: failed to read configuration file " +
                        path_string(path) + ": " + exists_error.message());
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

  if (exclude_connection_settings) {
    g_key_file_remove_key(key_file, "general", "name", nullptr);
    g_key_file_remove_key(key_file, "general", "icon", nullptr);
    g_key_file_remove_key(key_file, "general", "connection_id", nullptr);
    g_key_file_remove_key(key_file, "general", "open_connection",
                          nullptr);
    g_key_file_remove_key(key_file, "general", "theme", nullptr);
  }
  load_settings_store_from_key_file(store, key_file, warnings);
  if (!exclude_connection_settings) {
    load_hyperlink_settings_from_key_file(store, key_file, warnings);
  }
  if (macro_rules != nullptr) {
    *macro_rules = load_macro_rules_from_key_file(key_file, warnings);
  }
  g_key_file_unref(key_file);
  return true;
}

static void mark_settings_store_clean(SettingsStore *store) {
  for (SettingEntry &entry : store->entries) {
    entry.dirty = false;
  }
  store->macro_rules_dirty = false;
  store->hyperlink_settings_dirty = false;
}

static void restore_setting_entry(SettingsStore *store,
                                  const SettingsStore &previous,
                                  const SettingKey &key) {
  const SettingEntry *previous_entry = nullptr;
  for (const SettingEntry &entry : previous.entries) {
    if (entry.definition.key.section == key.section &&
        entry.definition.key.name == key.name) {
      previous_entry = &entry;
      break;
    }
  }
  if (previous_entry == nullptr) {
    return;
  }

  for (SettingEntry &entry : store->entries) {
    if (entry.definition.key.section == key.section &&
        entry.definition.key.name == key.name) {
      entry = *previous_entry;
      return;
    }
  }
}

static void resolve_terminal_key_binding_conflict(
    SettingsStore *store, const SettingsStore &previous,
    std::vector<std::string> *warnings) {
  if (!terminal_key_bindings_conflict(*store)) {
    return;
  }

  warnings->push_back(
      "Warning: conflicting terminal key bindings among [terminal] "
      "zoom_in_key, [terminal] zoom_out_key, and [terminal] send_break_key; "
      "using fallbacks");
  restore_setting_entry(store, previous, terminal_zoom_in_key_setting_key());
  restore_setting_entry(store, previous, terminal_zoom_out_key_setting_key());
  restore_setting_entry(store, previous,
                        terminal_send_break_key_setting_key());
}

static bool is_connection_only_key(const SettingKey &key) {
  const SettingKey name = general_name_setting_key();
  const SettingKey icon = general_icon_setting_key();
  const SettingKey id = general_connection_id_setting_key();
  const SettingKey hotkey =
      general_open_connection_hotkey_setting_key();
  return (key.section == name.section && key.name == name.name) ||
         (key.section == icon.section && key.name == icon.name) ||
         (key.section == id.section && key.name == id.name) ||
         (key.section == hotkey.section && key.name == hotkey.name) ||
         (key.section == "general" && key.name == "theme");
}

bool setting_is_inheritable(const SettingKey &key) {
  if (is_connection_only_key(key)) return false;
  const auto application = application_setting_definitions();
  return std::none_of(application.begin(), application.end(), [&key](const auto &definition) {
    return definition.key.section == key.section && definition.key.name == key.name;
  });
}

SettingKey general_theme_setting_key() {
  return make_setting_key("general", "theme");
}

std::string selected_settings_theme(const SettingsStore &store) {
  return setting_string_value_or_default(store, general_theme_setting_key(), "");
}

SettingsStore create_theme_settings(const SettingsStore &global) {
  std::vector<SettingDefinition> definitions;
  for (const auto &entry : global.entries) {
    if (setting_is_inheritable(entry.definition.key)) definitions.push_back(entry.definition);
  }
  auto result = create_settings_store(std::move(definitions));
  rebase_settings_store_fallbacks(&result, global);
  return result;
}

static bool theme_id_is_valid(const std::string &id) {
  return !id.empty() && std::all_of(id.begin(), id.end(), [](unsigned char c) {
    return g_ascii_isalnum(c) || c == '-' || c == '_';
  });
}

SettingsThemeCatalog load_settings_themes(const std::filesystem::path &path,
                                          const SettingsStore &global) {
  SettingsThemeCatalog result;
  GKeyFile *file = g_key_file_new();
  GError *error = nullptr;
  if (!g_key_file_load_from_file(file, path.c_str(), G_KEY_FILE_NONE, &error)) {
    if (!g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
      result.loaded = false;
      result.warnings.push_back("Failed to read themes " + path_string(path) + ": " + glib_error_message(error));
    }
    g_clear_error(&error);
    g_key_file_unref(file);
    return result;
  }
  gsize count = 0;
  gchar **groups = g_key_file_get_groups(file, &count);
  std::set<std::string> names;
  for (gsize index = 0; index < count; ++index) {
    const std::string group = groups[index];
    if (!group.starts_with("theme.")) {
      result.warnings.push_back("Unknown theme section: " + group);
      continue;
    }
    const auto suffix = group.substr(6);
    if (suffix.find('.') != std::string::npos) continue;
    auto name = key_file_string(file, group, "name", &error);
    if (error || !theme_id_is_valid(suffix) || trim_ascii_whitespace(name).empty() ||
        !names.insert(name).second) {
      result.warnings.push_back("Invalid or duplicate theme ID/name: " + group);
      g_clear_error(&error);
    }
    result.themes.push_back({.id = suffix, .name = name, .store = create_theme_settings(global)});
  }
  for (gsize index = 0; index < count; ++index) {
    const std::string group = groups[index];
    if (!group.starts_with("theme.")) continue;
    const auto split = group.find('.', 6);
    const auto id = group.substr(6, split == std::string::npos ? split : split - 6);
    const auto found = std::find_if(result.themes.begin(), result.themes.end(),
        [&id](const auto &theme) { return theme.id == id; });
    if (found == result.themes.end()) {
      result.warnings.push_back("Theme has no name section: " + group);
      continue;
    }
    GKeyFile *values = g_key_file_new();
    gsize key_count = 0;
    gchar **keys = g_key_file_get_keys(file, group.c_str(), &key_count, nullptr);
    for (gsize key_index = 0; key_index < key_count; ++key_index) {
      const std::string key = keys[key_index];
      if (split == std::string::npos && key == "name") continue;
      const std::string section = split == std::string::npos ? "" : group.substr(split + 1);
      const auto entry = std::find_if(found->store.entries.begin(), found->store.entries.end(),
          [&section, &key](const auto &value) {
            return value.definition.key.section == section && value.definition.key.name == key;
          });
      if (entry == found->store.entries.end()) {
        result.warnings.push_back("Unknown or non-inheritable theme setting: [" + group + "] " + key);
        continue;
      }
      gchar *raw = g_key_file_get_value(file, group.c_str(), key.c_str(), nullptr);
      g_key_file_set_value(values, section.c_str(), key.c_str(), raw);
      g_free(raw);
    }
    std::vector<std::string> warnings;
    load_settings_store_from_key_file(&found->store, values, &warnings);
    for (const auto &warning : warnings) result.warnings.push_back(found->name + ": " + warning);
    g_strfreev(keys);
    g_key_file_unref(values);
  }
  g_strfreev(groups);
  g_key_file_unref(file);
  result.loaded = result.warnings.empty();
  return result;
}

bool rebase_settings_theme(SettingsStore *store,
                           std::shared_ptr<const SettingsThemeContext> context) {
  auto fallbacks = create_theme_settings(context->global);
  const auto id = selected_settings_theme(*store);
  const auto found = std::find_if(context->catalog.themes.begin(), context->catalog.themes.end(),
      [&id](const auto &theme) { return theme.id == id; });
  const bool resolved = id.empty() || (found != context->catalog.themes.end() && context->catalog.loaded);
  if (found != context->catalog.themes.end()) {
    for (auto &entry : fallbacks.entries) {
      const auto value = std::find_if(found->store.entries.begin(), found->store.entries.end(),
          [&entry](const auto &candidate) {
            return candidate.loaded && candidate.definition.key.section == entry.definition.key.section &&
                candidate.definition.key.name == entry.definition.key.name;
          });
      if (value == found->store.entries.end()) continue;
      entry.value = value->value;
      entry.validation_error = value->validation_error;
      entry.invalid_raw_value = value->invalid_raw_value;
      entry.fallback_source = SettingValueSource::theme;
      entry.fallback_theme_id = found->id;
      entry.fallback_theme_name = found->name;
    }
  }
  rebase_settings_store_fallbacks(store, fallbacks);
  for (auto &entry : store->entries) {
    if (entry.definition.key.section != "general" || entry.definition.key.name != "theme") continue;
    entry.validation_error = resolved ? "" : "Unresolved theme: " + id;
  }
  store->theme_context = std::move(context);
  return resolved;
}

static bool is_connection_only_entry(const SettingEntry &entry) {
  return is_connection_only_key(entry.definition.key);
}

static GKeyFile *serialize_settings(const SettingsStore &store,
                                    bool exclude_connection_settings) {
  GKeyFile *key_file = g_key_file_new();
  for (const SettingEntry &entry : store.entries) {
    if (!entry.loaded ||
        (exclude_connection_settings &&
         is_connection_only_entry(entry))) {
      continue;
    }
    set_key_file_value(key_file, entry);
  }
  if (!exclude_connection_settings) {
    for (const MacroRule &rule : store.macro_rules) {
      set_key_file_macro_rule(key_file, rule);
    }
    if (store.hyperlink_settings_configured) {
      g_key_file_set_boolean(key_file, "hyperlink", "enabled",
                             store.hyperlink_actions_enabled ? TRUE : FALSE);
      for (const HyperlinkActionRule &rule : store.hyperlink_rules) {
        set_key_file_hyperlink_rule(key_file, rule);
      }
    }
  }
  return key_file;
}

static SettingsSaveResult write_settings_key_file(
    GKeyFile *key_file, const std::filesystem::path &config_path) {
  SettingsSaveResult result{
      .saved = false,
      .warnings = {},
  };
  if (config_path.empty()) {
    result.warnings.push_back(
        "Warning: failed to save configuration file <empty>: empty path");
    g_key_file_unref(key_file);
    return result;
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

static std::filesystem::path
temporary_settings_path(const std::filesystem::path &target) {
  const auto timestamp =
      std::chrono::steady_clock::now().time_since_epoch().count();
  return target.string() + ".tmp-" + std::to_string(timestamp);
}

static SettingsSaveResult write_global_settings_key_file(
    GKeyFile *key_file, const std::filesystem::path &config_path) {
  if (config_path.empty()) {
    return write_settings_key_file(key_file, config_path);
  }

  const std::filesystem::path parent = config_path.parent_path();
  if (!parent.empty()) {
    std::error_code create_error;
    std::filesystem::create_directories(parent, create_error);
    if (create_error) {
      g_key_file_unref(key_file);
      return {
          .saved = false,
          .warnings = {
              "Warning: failed to save configuration file " +
              path_string(config_path) + ": " + create_error.message(),
          },
      };
    }
  }

  const std::filesystem::path temporary_path =
      temporary_settings_path(config_path);
  SettingsSaveResult write_result =
      write_settings_key_file(key_file, temporary_path);
  if (!write_result.saved) {
    return write_result;
  }

  std::error_code rename_error;
  std::filesystem::rename(temporary_path, config_path, rename_error);
  if (!rename_error) {
    return {
        .saved = true,
        .warnings = {},
    };
  }

  std::error_code remove_error;
  std::filesystem::remove(temporary_path, remove_error);
  return {
      .saved = false,
      .warnings = {
          "Warning: failed to save configuration file " +
          path_string(config_path) + ": " + rename_error.message(),
      },
  };
}

static SettingsStore
create_global_default_settings(TerminalDisplaySettings terminal_defaults) {
  SettingsStore connection =
      create_default_settings(std::move(terminal_defaults), "elder-terms");
  std::vector<SettingDefinition> definitions;
  definitions.reserve(connection.entries.size() + 3);
  for (SettingEntry &entry : connection.entries) {
    if (is_connection_only_entry(entry)) {
      continue;
    }
    definitions.push_back(std::move(entry.definition));
  }
  append_definitions(&definitions, application_setting_definitions());
  return create_settings_store(std::move(definitions));
}

SettingsStore create_default_settings(TerminalDisplaySettings terminal_defaults,
                                      std::string default_connection_name) {
  std::vector<SettingDefinition> definitions;
  append_definitions(
      &definitions,
      general_setting_definitions(std::move(default_connection_name)));
  definitions.push_back({.key = general_theme_setting_key(), .default_value = std::string(),
                         .retain_invalid = true});
  append_definitions(&definitions, terminal_log_setting_definitions());
  append_definitions(&definitions, terminal_setting_definitions(terminal_defaults));
  append_definitions(&definitions, local_shell_connection_setting_definitions());
  append_definitions(&definitions, telnet_connection_setting_definitions());
  append_definitions(&definitions, ssh_connection_setting_definitions());
  append_definitions(&definitions, ssh_proxy_setting_definitions());
  append_definitions(&definitions, sftp_connection_setting_definitions());
  append_definitions(&definitions, ftp_connection_setting_definitions());
  append_definitions(&definitions, webdav_connection_setting_definitions());
  append_definitions(&definitions, http_connection_setting_definitions());
  append_definitions(&definitions, smb_connection_setting_definitions());
  append_definitions(&definitions, nfs_connection_setting_definitions());
  append_definitions(&definitions, serial_connection_setting_definitions());
  append_definitions(&definitions, transfer_setting_definitions());
  return create_settings_store(std::move(definitions));
}

std::filesystem::path default_global_config_path() {
  const char *config_directory = g_get_user_config_dir();
  return std::filesystem::path(config_directory == nullptr
                                   ? std::string()
                                   : std::string(config_directory)) /
         "elder-terms" / "global.ini";
}

SettingsLoadResult
load_global_settings(const std::filesystem::path &global_config_path,
                     gdouble default_terminal_font_size_dip) {
  SettingsLoadResult result{
      .store = create_global_default_settings(
          default_terminal_display_settings(default_terminal_font_size_dip)),
      .loaded = true,
      .warnings = {},
  };
  const SettingsStore built_in = result.store;
  result.loaded =
      load_settings_file(&result.store, global_config_path, true, true,
                         nullptr,
                         &result.warnings);
  if (application_ui_language(result.store) ==
      ApplicationUiLanguage::system) {
    clear_explicit_setting_value(
        &result.store, application_ui_language_setting_key());
  }
  resolve_terminal_key_binding_conflict(&result.store, built_in,
                                        &result.warnings);
  mark_settings_store_clean(&result.store);
  return result;
}

SettingsLoadResult
load_settings(const SettingsLoadOptions &options, gdouble default_terminal_font_size_dip) {
  SettingsLoadResult result{
      .store = create_default_settings(
          default_terminal_display_settings(default_terminal_font_size_dip),
          default_connection_name(options)),
      .loaded = true,
      .warnings = {},
  };

  auto context = std::make_shared<SettingsThemeContext>();
  context->global = create_global_default_settings(
      default_terminal_display_settings(default_terminal_font_size_dip));
  if (options.global_config_path.has_value()) {
    auto global = load_global_settings(*options.global_config_path, default_terminal_font_size_dip);
    context->global = std::move(global.store);
    context->global_path = *options.global_config_path;
    result.loaded = global.loaded;
    result.warnings.insert(result.warnings.end(), global.warnings.begin(),
                           global.warnings.end());
  }
  context->theme_path = options.theme_config_path.value_or(
      options.global_config_path ? options.global_config_path->parent_path() / "theme.ini" : std::filesystem::path());
  if (!context->theme_path.empty()) {
    context->catalog = load_settings_themes(context->theme_path, context->global);
    result.warnings.insert(result.warnings.end(), context->catalog.warnings.begin(), context->catalog.warnings.end());
  }
  // Resolve selection before applying connection/startup values. Loading these
  // layers into an isolated store keeps their explicit values above the theme.
  auto overrides = result.store;
  for (const auto &path : {options.config_path, options.startup_config_path}) {
    if (!path) continue;
    std::vector<std::string> warnings;
    load_settings_file(&overrides, *path, false, false, nullptr, &warnings);
  }
  for (auto &entry : result.store.entries) {
    if (entry.definition.key.section == "general" && entry.definition.key.name == "theme") {
      for (const auto &value : overrides.entries)
        if (value.definition.key.section == "general" && value.definition.key.name == "theme") entry = value;
    }
  }
  const bool theme_resolved = rebase_settings_theme(&result.store, context);
  if (options.config_path.has_value()) {
    const SettingsStore previous = result.store;
    std::vector<MacroRule> macros;
    const bool loaded = load_settings_file(
        &result.store, options.config_path.value(), false, false, &macros,
        &result.warnings);
    if (loaded) {
      result.store.macro_rules = std::move(macros);
    }
    result.loaded = loaded && result.loaded;
    resolve_terminal_key_binding_conflict(&result.store, previous,
                                          &result.warnings);
  }
  if (options.startup_config_path.has_value()) {
    const SettingsStore previous = result.store;
    std::vector<MacroRule> macros;
    const bool loaded = load_settings_file(
        &result.store, options.startup_config_path.value(), false, false,
        &macros, &result.warnings);
    if (loaded) {
      result.store.macro_rules = std::move(macros);
    }
    result.loaded = loaded && result.loaded;
    resolve_terminal_key_binding_conflict(&result.store, previous,
                                          &result.warnings);
  }

  // Loading the reference clears parsing errors, so restore unresolved status
  // after all explicit layers have been read.
  rebase_settings_theme(&result.store, context);
  if (!theme_resolved) {
    result.loaded = false;
    result.warnings.push_back("Unresolved theme: " + selected_settings_theme(result.store));
  }

  mark_settings_store_clean(&result.store);

  if (general_settings_select_telnet_connection(result.store)) {
    append_telnet_connection_warnings(result.store, &result.warnings);
  }
  if (general_settings_select_ssh_connection(result.store) ||
      general_settings_select_sftp_connection(result.store)) {
    append_ssh_connection_warnings(result.store, &result.warnings);
  }
  if (general_settings_select_ftp_connection(result.store)) {
    append_ftp_connection_warnings(result.store, &result.warnings);
  }
  if (general_connection_kind(result.store) == ConnectionKind::http) {
    const auto connection = http_connection_settings(result.store);
    result.warnings.insert(result.warnings.end(), connection.validation_errors.begin(), connection.validation_errors.end());
  }
  if (general_connection_kind(result.store) == ConnectionKind::webdav) {
    const auto connection = webdav_connection_settings(result.store);
    result.warnings.insert(result.warnings.end(), connection.validation_errors.begin(), connection.validation_errors.end());
    if (connection.address.empty()) result.warnings.emplace_back("WebDAV server address is required");
  }
  if (general_connection_kind(result.store) == ConnectionKind::smb) {
    const auto connection = smb_connection_settings(result.store);
    result.warnings.insert(result.warnings.end(), connection.validation_errors.begin(), connection.validation_errors.end());
  }
  if (general_connection_kind(result.store) == ConnectionKind::nfs) {
    const auto connection = nfs_connection_settings(result.store);
    result.warnings.insert(result.warnings.end(), connection.validation_errors.begin(), connection.validation_errors.end());
  }
  if (general_settings_select_serial_connection(result.store)) {
    append_serial_connection_warnings(result.store, &result.warnings);
  }
  return result;
}

SettingsSaveResult save_settings(const SettingsStore &store,
                                 const std::filesystem::path &config_path) {
  return write_settings_key_file(serialize_settings(store, false),
                                 config_path);
}

SettingsSaveResult
save_global_settings(const SettingsStore &store,
                     const std::filesystem::path &global_config_path) {
  return write_global_settings_key_file(serialize_settings(store, true),
                                        global_config_path);
}

static void update_application_key_file_value(
    GKeyFile *key_file, const SettingsStore &store, const SettingKey &key) {
  if (!setting_has_explicit_value(store, key)) {
    (void)g_key_file_remove_key(key_file, key.section.c_str(),
                               key.name.c_str(), nullptr);
    return;
  }
  const std::string value =
      setting_string_value_or_default(store, key, std::string());
  g_key_file_set_string(key_file, key.section.c_str(), key.name.c_str(),
                        value.c_str());
}

static SettingsSaveResult save_application_setting_keys(
    const SettingsStore &store,
    const std::filesystem::path &global_config_path,
    const std::vector<SettingKey> &keys) {
  GKeyFile *key_file = g_key_file_new();
  GError *error = nullptr;
  if (!g_key_file_load_from_file(
          key_file, global_config_path.c_str(),
          static_cast<GKeyFileFlags>(G_KEY_FILE_KEEP_COMMENTS |
                                     G_KEY_FILE_KEEP_TRANSLATIONS),
          &error) &&
      !g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
    const std::string warning =
        "Warning: failed to load configuration file " +
        path_string(global_config_path) + ": " + glib_error_message(error);
    g_clear_error(&error);
    g_key_file_unref(key_file);
    return {
        .saved = false,
        .warnings = {warning},
    };
  }
  g_clear_error(&error);

  for (const SettingKey &key : keys) {
    update_application_key_file_value(key_file, store, key);
  }
  return write_global_settings_key_file(key_file, global_config_path);
}

SettingsSaveResult save_application_settings(
    const SettingsStore &store,
    const std::filesystem::path &global_config_path) {
  return save_application_setting_keys(
      store, global_config_path,
      {application_ui_language_setting_key(),
       application_startup_mode_setting_key(),
       application_open_hotkey_setting_key(),
       application_new_window_setting_key()});
}

SettingsSaveResult save_new_window_connection(
    const std::string &connection,
    const std::filesystem::path &global_config_path) {
  auto store = create_global_default_settings(
      default_terminal_display_settings(12.0));
  const auto key = application_new_window_setting_key();
  if (!connection.empty()) {
    set_explicit_setting_value(&store, key, SettingValue{connection});
  }
  return save_application_setting_keys(store, global_config_path, {key});
}

static SettingsFileSnapshot settings_file_snapshot(const std::filesystem::path &path) {
  SettingsFileSnapshot snapshot{.path = path, .contents = std::nullopt};
  gchar *contents = nullptr;
  gsize size = 0;
  GError *error = nullptr;
  if (g_file_get_contents(path.c_str(), &contents, &size, &error)) {
    snapshot.contents = std::string(contents, size);
    g_free(contents);
  } else if (!g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
    snapshot.readable = false;
  }
  g_clear_error(&error);
  return snapshot;
}

static bool settings_file_unchanged(const SettingsFileSnapshot &snapshot) {
  const auto current = settings_file_snapshot(snapshot.path);
  return snapshot.readable && current.readable && snapshot.contents == current.contents;
}

SettingsEditSession begin_settings_edit(const std::filesystem::path &global_path,
    const std::filesystem::path &theme_path, gdouble font_size) {
  SettingsEditSession session;
  session.global_file = settings_file_snapshot(global_path);
  session.theme_file = settings_file_snapshot(theme_path);
  auto global = load_global_settings(global_path, font_size);
  session.global = std::move(global.store);
  session.catalog = load_settings_themes(theme_path, session.global);
  session.warnings = std::move(global.warnings);
  session.warnings.insert(session.warnings.end(), session.catalog.warnings.begin(), session.catalog.warnings.end());
  session.loaded = global.loaded && session.catalog.loaded &&
      settings_file_unchanged(session.global_file) && settings_file_unchanged(session.theme_file);
  if (!session.loaded && session.warnings.empty()) session.warnings.push_back("Configuration changed while opening the editor");
  return session;
}

bool explicit_settings_equal(const SettingsStore &left, const SettingsStore &right) {
  const auto matches = [](const auto &source, const auto &target) {
    for (const auto &entry : source.entries) {
      if (!entry.loaded) continue;
      const auto found = std::find_if(target.entries.begin(), target.entries.end(), [&entry](const auto &other) {
        return other.definition.key.section == entry.definition.key.section && other.definition.key.name == entry.definition.key.name;
      });
      if (found == target.entries.end() || !found->loaded || entry.value != found->value ||
          entry.invalid_raw_value != found->invalid_raw_value || entry.validation_error != found->validation_error) return false;
    }
    return true;
  };
  return matches(left, right) && matches(right, left);
}

bool settings_themes_equal(const SettingsThemeCatalog &left, const SettingsThemeCatalog &right) {
  if (left.themes.size() != right.themes.size()) return false;
  for (std::size_t index = 0; index < left.themes.size(); ++index) {
    const auto &a = left.themes[index];
    const auto &b = right.themes[index];
    if (a.id != b.id || a.name != b.name || !explicit_settings_equal(a.store, b.store)) return false;
  }
  return true;
}

static std::vector<std::string> validate_explicit_settings(const SettingsStore &store) {
  std::vector<std::string> errors;
  for (const auto &entry : store.entries) {
    if (!entry.loaded) continue;
    std::string reason = entry.validation_error;
    if (entry.value.index() != entry.definition.default_value.index()) reason = "Invalid setting type";
    else if (entry.definition.validate) entry.definition.validate(entry.value, &reason);
    if (!reason.empty()) errors.push_back("[" + entry.definition.key.section + "] " + entry.definition.key.name + ": " + reason);
  }
  if (terminal_key_bindings_conflict(store)) errors.push_back("Conflicting terminal key bindings");
  return errors;
}

std::vector<std::string> validate_settings_themes(const SettingsThemeCatalog &catalog) {
  auto errors = catalog.loaded ? std::vector<std::string>() : catalog.warnings;
  if (!catalog.loaded && errors.empty()) errors.push_back("Theme catalog could not be loaded");
  std::set<std::string> ids, names;
  for (const auto &theme : catalog.themes) {
    if (!theme_id_is_valid(theme.id) || !ids.insert(theme.id).second) errors.push_back("Invalid or duplicate theme ID: " + theme.id);
    if (trim_ascii_whitespace(theme.name).empty() || !names.insert(trim_ascii_whitespace(theme.name)).second)
      errors.push_back("Theme names must be nonblank and unique");
    for (const auto &entry : theme.store.entries)
      if (!setting_is_inheritable(entry.definition.key)) errors.push_back("Non-inheritable theme setting: " + entry.definition.key.name);
    const auto values = validate_explicit_settings(theme.store);
    for (const auto &error : values) errors.push_back(theme.name + ": " + error);
  }
  return errors;
}

static GKeyFile *serialize_themes(const SettingsThemeCatalog &catalog) {
  GKeyFile *file = g_key_file_new();
  for (const auto &theme : catalog.themes) {
    const auto group = "theme." + theme.id;
    g_key_file_set_string(file, group.c_str(), "name", theme.name.c_str());
    for (auto entry : theme.store.entries) {
      if (!entry.loaded) continue;
      entry.definition.key.section = group + "." + entry.definition.key.section;
      set_key_file_value(file, entry);
    }
  }
  return file;
}

SettingsEditResult save_settings_edit(SettingsEditSession *session,
    const SettingsStore &global, const SettingsThemeCatalog &catalog) {
  SettingsEditResult result;
  if (!session->loaded) {
    result.warnings = session->warnings;
    return result;
  }
  result.warnings = validate_explicit_settings(global);
  const auto errors = validate_settings_themes(catalog);
  result.warnings.insert(result.warnings.end(), errors.begin(), errors.end());
  if (!result.warnings.empty()) return result;
  const auto save_file = [&result](SettingsFileSnapshot *snapshot, GKeyFile *file) {
    if (!settings_file_unchanged(*snapshot)) {
      g_key_file_unref(file);
      result.warnings.push_back("Configuration changed outside the editor: " + snapshot->path.string());
      return SettingsFileSaveStatus::failed;
    }
    const auto saved = write_global_settings_key_file(file, snapshot->path);
    result.warnings.insert(result.warnings.end(), saved.warnings.begin(), saved.warnings.end());
    if (!saved.saved) return SettingsFileSaveStatus::failed;
    *snapshot = settings_file_snapshot(snapshot->path);
    return SettingsFileSaveStatus::saved;
  };
  if (!explicit_settings_equal(global, session->global)) {
    auto previous_defaults = session->global;
    auto edited_defaults = global;
    const auto application_entry = [](const auto &entry) { return !setting_is_inheritable(entry.definition.key); };
    std::erase_if(previous_defaults.entries, application_entry);
    std::erase_if(edited_defaults.entries, application_entry);
    const bool defaults_changed = !explicit_settings_equal(previous_defaults, edited_defaults);
    // Keep the existing contracts: editing connection defaults normalizes the
    // file; application-only edits preserve unrelated keys and comments.
    GKeyFile *file = defaults_changed ? serialize_settings(global, true) : g_key_file_new();
    if (!defaults_changed && session->global_file.contents)
      g_key_file_load_from_data(file, session->global_file.contents->data(), session->global_file.contents->size(),
                               G_KEY_FILE_KEEP_COMMENTS, nullptr);
    for (const auto &entry : global.entries) {
      if (is_connection_only_entry(entry)) continue;
      if (entry.loaded) set_key_file_value(file, entry);
      else g_key_file_remove_key(file, entry.definition.key.section.c_str(), entry.definition.key.name.c_str(), nullptr);
    }
    gsize group_count = 0;
    gchar **groups = g_key_file_get_groups(file, &group_count);
    for (gsize index = 0; index < group_count; ++index) {
      gsize key_count = 0;
      gchar **keys = g_key_file_get_keys(file, groups[index], &key_count, nullptr);
      g_strfreev(keys);
      if (key_count == 0) g_key_file_remove_group(file, groups[index], nullptr);
    }
    g_strfreev(groups);
    result.global = save_file(&session->global_file, file);
    if (result.global == SettingsFileSaveStatus::saved) {
      session->global = global;
      mark_settings_store_clean(&session->global);
    }
  }
  if (!settings_themes_equal(catalog, session->catalog)) {
    result.themes = save_file(&session->theme_file, serialize_themes(catalog));
    if (result.themes == SettingsFileSaveStatus::saved) {
      session->catalog = catalog;
      for (auto &theme : session->catalog.themes) mark_settings_store_clean(&theme.store);
    }
  }
  result.complete = result.global != SettingsFileSaveStatus::failed && result.themes != SettingsFileSaveStatus::failed;
  return result;
}

const char *terminal_backspace_code_to_string(TerminalBackspaceCode code) {
  if (code == TerminalBackspaceCode::automatic) {
    return "auto";
  }
  return code == TerminalBackspaceCode::bs ? "bs" : "del";
}

const char *terminal_cursor_key_mode_to_string(TerminalCursorKeyMode mode) {
  return mode == TerminalCursorKeyMode::trs80 ? "trs80" : "normal";
}

const char *terminal_return_code_to_string(TerminalReturnCode code) {
  if (code == TerminalReturnCode::automatic) {
    return "auto";
  }
  if (code == TerminalReturnCode::cr) {
    return "cr";
  }
  return code == TerminalReturnCode::lf ? "lf" : "crlf";
}

TerminalTextSettings
default_terminal_text_settings(TerminalConnectionKind kind) {
  TerminalTextSettings settings;
  if (kind == TerminalConnectionKind::serial) {
    settings.backspace_code = TerminalBackspaceCode::bs;
    settings.cursor_key_mode = TerminalCursorKeyMode::trs80;
    settings.return_code = TerminalReturnCode::cr;
  }
  return settings;
}

TerminalTextSettings terminal_text_settings(const SettingsStore &store,
                                            TerminalConnectionKind kind) {
  TerminalTextSettings settings = default_terminal_text_settings(kind);
  if (setting_has_configured_value(store, terminal_encoding_setting_key())) {
    settings.encoding = trim_ascii_whitespace(setting_string_value_or_default(
        store, terminal_encoding_setting_key(), settings.encoding));
  }
  if (setting_has_configured_value(
          store, terminal_backspace_code_setting_key())) {
    const std::string configured = setting_string_value_or_default(
        store, terminal_backspace_code_setting_key(),
        terminal_backspace_code_to_string(settings.backspace_code));
    if (configured == "auto") {
      settings.backspace_code = TerminalBackspaceCode::automatic;
    } else {
      settings.backspace_code = configured == "bs"
                                    ? TerminalBackspaceCode::bs
                                    : TerminalBackspaceCode::del;
    }
  }
  if (setting_has_configured_value(
          store, terminal_cursor_key_mode_setting_key())) {
    const std::string configured = setting_string_value_or_default(
        store, terminal_cursor_key_mode_setting_key(),
        terminal_cursor_key_mode_to_string(settings.cursor_key_mode));
    settings.cursor_key_mode = configured == "trs80"
                                   ? TerminalCursorKeyMode::trs80
                                   : TerminalCursorKeyMode::normal;
  }
  if (setting_has_configured_value(store,
                                   terminal_return_code_setting_key())) {
    const std::string configured = setting_string_value_or_default(
        store, terminal_return_code_setting_key(),
        terminal_return_code_to_string(settings.return_code));
    if (configured == "auto") {
      settings.return_code = TerminalReturnCode::automatic;
    } else if (configured == "cr") {
      settings.return_code = TerminalReturnCode::cr;
    } else if (configured == "lf") {
      settings.return_code = TerminalReturnCode::lf;
    } else {
      settings.return_code = TerminalReturnCode::crlf;
    }
  }
  return settings;
}

std::optional<TerminalConnectionProfile>
terminal_connection_profile(const SettingsStore &store) {
  if (general_settings_select_sftp_connection(store) ||
      general_settings_select_ftp_connection(store) ||
      general_connection_kind(store) == ConnectionKind::webdav ||
      general_connection_kind(store) == ConnectionKind::http ||
      general_connection_kind(store) == ConnectionKind::nfs ||
      general_connection_kind(store) == ConnectionKind::smb) {
    return std::nullopt;
  }

  if (general_settings_select_telnet_connection(store)) {
    return TerminalConnectionProfile{
        .name = general_connection_name(store),
        .kind = TerminalConnectionKind::telnet,
        .settings = telnet_connection_settings(store),
        .text_settings =
            terminal_text_settings(store, TerminalConnectionKind::telnet),
        .ssh_proxy = ssh_proxy_connection_settings(store),
    };
  }

  if (general_settings_select_serial_connection(store)) {
    return TerminalConnectionProfile{
        .name = general_connection_name(store),
        .kind = TerminalConnectionKind::serial,
        .settings = serial_connection_settings(store),
        .text_settings =
            terminal_text_settings(store, TerminalConnectionKind::serial),
    };
  }

  if (general_settings_select_ssh_connection(store)) {
    return TerminalConnectionProfile{
        .name = general_connection_name(store),
        .kind = TerminalConnectionKind::ssh,
        .settings = ssh_connection_settings(store),
        .text_settings =
            terminal_text_settings(store, TerminalConnectionKind::ssh),
        .ssh_proxy = ssh_proxy_connection_settings(store),
    };
  }

  return TerminalConnectionProfile{
      .name = general_connection_name(store),
      .kind = TerminalConnectionKind::local_shell,
      .settings = local_shell_connection_settings(store),
      .text_settings =
          terminal_text_settings(store, TerminalConnectionKind::local_shell),
  };
}

} // namespace elder_terms
