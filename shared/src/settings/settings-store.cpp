#include <elder-terms/settings/settings-store.h>

#include <algorithm>
#include <utility>

namespace elder_terms {

static bool keys_match(const SettingKey &left, const SettingKey &right) {
  return left.section == right.section && left.name == right.name;
}

static std::string setting_label(const SettingKey &key) {
  return "[" + key.section + "] " + key.name;
}

static SettingEntry make_entry(SettingDefinition definition) {
  SettingEntry entry;
  entry.definition = std::move(definition);
  entry.value = entry.definition.default_value;
  return entry;
}

static SettingEntry *find_entry(SettingsStore *store, const SettingKey &key) {
  auto iterator =
      std::find_if(store->entries.begin(), store->entries.end(),
                   [&key](const SettingEntry &entry) {
                     return keys_match(entry.definition.key, key);
                   });
  return iterator == store->entries.end() ? nullptr : &*iterator;
}

static const SettingEntry *find_entry(const SettingsStore &store,
                                      const SettingKey &key) {
  auto iterator =
      std::find_if(store.entries.begin(), store.entries.end(),
                   [&key](const SettingEntry &entry) {
                     return keys_match(entry.definition.key, key);
                   });
  return iterator == store.entries.end() ? nullptr : &*iterator;
}

static std::string glib_error_message(GError *error) {
  if (error == nullptr || error->message == nullptr) {
    return "unknown error";
  }
  return error->message;
}

static void warn_invalid_value(std::vector<std::string> *warnings,
                               const SettingKey &key,
                               const std::string &reason) {
  warnings->push_back("Warning: invalid configuration value " +
                      setting_label(key) + ": " + reason +
                      "; using default");
}

static bool validate_setting_definition(const SettingDefinition &definition,
                                        const SettingValue &value,
                                        std::string *reason) {
  if (value.index() != definition.default_value.index()) {
    *reason = "has an incompatible value type";
    return false;
  }

  if (definition.validate == nullptr) {
    return true;
  }
  return definition.validate(value, reason);
}

static SettingValue read_setting_value(GKeyFile *key_file,
                                       const SettingEntry &entry,
                                       GError **error) {
  const char *section = entry.definition.key.section.c_str();
  const char *name = entry.definition.key.name.c_str();
  if (std::holds_alternative<gint64>(entry.definition.default_value)) {
    return static_cast<gint64>(
        g_key_file_get_integer(key_file, section, name, error));
  }
  if (std::holds_alternative<gdouble>(entry.definition.default_value)) {
    return g_key_file_get_double(key_file, section, name, error);
  }
  if (std::holds_alternative<std::string>(entry.definition.default_value)) {
    char *value = g_key_file_get_string(key_file, section, name, error);
    if (value == nullptr) {
      return std::string();
    }
    std::string text = value;
    g_free(value);
    return text;
  }

  return g_key_file_get_boolean(key_file, section, name, error) != FALSE;
}

SettingKey make_setting_key(std::string section, std::string name) {
  return {
      .section = std::move(section),
      .name = std::move(name),
  };
}

SettingsStore create_settings_store(std::vector<SettingDefinition> definitions) {
  SettingsStore store;
  store.entries.reserve(definitions.size());
  for (SettingDefinition &definition : definitions) {
    store.entries.push_back(make_entry(std::move(definition)));
  }
  return store;
}

void load_settings_store_from_key_file(SettingsStore *store,
                                       GKeyFile *key_file,
                                       std::vector<std::string> *warnings) {
  for (SettingEntry &entry : store->entries) {
    const char *section = entry.definition.key.section.c_str();
    const char *name = entry.definition.key.name.c_str();
    if (!g_key_file_has_group(key_file, section) ||
        !g_key_file_has_key(key_file, section, name, nullptr)) {
      continue;
    }

    GError *error = nullptr;
    SettingValue value = read_setting_value(key_file, entry, &error);
    if (error != nullptr) {
      warn_invalid_value(warnings, entry.definition.key,
                         glib_error_message(error));
      g_clear_error(&error);
      continue;
    }

    std::string reason;
    if (!validate_setting_definition(entry.definition, value, &reason)) {
      warn_invalid_value(warnings, entry.definition.key, reason);
      continue;
    }

    entry.value = std::move(value);
    entry.loaded = true;
    entry.dirty = false;
  }
}

SettingValue setting_value_or_default(const SettingsStore &store,
                                      const SettingKey &key,
                                      SettingValue fallback) {
  const SettingEntry *entry = find_entry(store, key);
  if (entry == nullptr) {
    return fallback;
  }
  return entry->value;
}

std::string setting_string_value_or_default(const SettingsStore &store,
                                            const SettingKey &key,
                                            std::string fallback) {
  return std::get<std::string>(
      setting_value_or_default(store, key, SettingValue{std::move(fallback)}));
}

gint64 setting_integer_value_or_default(const SettingsStore &store,
                                        const SettingKey &key,
                                        gint64 fallback) {
  return std::get<gint64>(
      setting_value_or_default(store, key, SettingValue{fallback}));
}

gdouble setting_double_value_or_default(const SettingsStore &store,
                                        const SettingKey &key,
                                        gdouble fallback) {
  return std::get<gdouble>(
      setting_value_or_default(store, key, SettingValue{fallback}));
}

bool setting_boolean_value_or_default(const SettingsStore &store,
                                      const SettingKey &key, bool fallback) {
  return std::get<bool>(
      setting_value_or_default(store, key, SettingValue{fallback}));
}

bool set_setting_value(SettingsStore *store, const SettingKey &key,
                       SettingValue value) {
  SettingEntry *entry = find_entry(store, key);
  if (entry == nullptr ||
      value.index() != entry->definition.default_value.index()) {
    return false;
  }

  std::string reason;
  if (!validate_setting_definition(entry->definition, value, &reason)) {
    return false;
  }

  if (entry->value != value) {
    entry->value = std::move(value);
    entry->dirty = true;
  }
  return true;
}

bool setting_is_dirty(const SettingsStore &store, const SettingKey &key) {
  const SettingEntry *entry = find_entry(store, key);
  return entry != nullptr && entry->dirty;
}

bool settings_store_is_dirty(const SettingsStore &store) {
  return std::any_of(store.entries.begin(), store.entries.end(),
                     [](const SettingEntry &entry) { return entry.dirty; });
}

} // namespace elder_terms
