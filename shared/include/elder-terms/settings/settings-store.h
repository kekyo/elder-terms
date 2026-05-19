#pragma once

#include <string>
#include <variant>
#include <vector>

#include <glib.h>

namespace elder_terms {

/**
 * Identifies one configuration key inside an INI section.
 */
struct SettingKey {
  /** INI section name. */
  std::string section;
  /** INI key name. */
  std::string name;
};

/**
 * Stores a supported scalar setting value.
 *
 * @remarks Numeric settings use either integer or floating-point variants so
 * callers can reject accidental fractional values at the schema boundary.
 */
using SettingValue = std::variant<gint64, gdouble, std::string, bool>;

/**
 * Validates one loaded or updated setting value.
 *
 * @param value Candidate setting value.
 * @param reason Receives a human-readable reason when validation fails.
 * @returns True when the value is accepted.
 */
using SettingValueValidator = bool (*)(const SettingValue &value,
                                       std::string *reason);

/**
 * Describes a setting and its fallback value.
 */
struct SettingDefinition {
  /** INI key associated with this setting. */
  SettingKey key;
  /** Value used when the key is absent or invalid. */
  SettingValue default_value;
  /** Optional semantic validation applied after parsing. */
  SettingValueValidator validate = nullptr;
};

/**
 * Holds the current value and change state for one setting.
 */
struct SettingEntry {
  /** Setting schema. */
  SettingDefinition definition;
  /** Current value, initialized from default_value. */
  SettingValue value;
  /** True when the value came from a configuration file. */
  bool loaded = false;
  /** True after the value is changed in memory. */
  bool dirty = false;
};

/**
 * Owns all loaded settings for the application.
 */
struct SettingsStore {
  /** Registered setting entries. */
  std::vector<SettingEntry> entries;
};

/**
 * Creates a setting key from section and key names.
 *
 * @param section INI section name.
 * @param name INI key name.
 * @returns Setting key value.
 */
SettingKey make_setting_key(std::string section, std::string name);

/**
 * Creates a settings store initialized with definition defaults.
 *
 * @param definitions Registered setting definitions.
 * @returns Settings store containing all registered keys.
 */
SettingsStore create_settings_store(std::vector<SettingDefinition> definitions);

/**
 * Loads all registered keys from a parsed GLib key file.
 *
 * @param store Target settings store.
 * @param key_file Parsed INI file.
 * @param warnings Non-fatal warning sink.
 */
void load_settings_store_from_key_file(SettingsStore *store,
                                       GKeyFile *key_file,
                                       std::vector<std::string> *warnings);

/**
 * Returns the value for a setting, or fallback when the key is not registered.
 *
 * @param store Source settings store.
 * @param key Setting key.
 * @param fallback Value returned when key is absent.
 * @returns Current or fallback value.
 */
SettingValue setting_value_or_default(
  const SettingsStore &store, const SettingKey &key, SettingValue fallback);

/**
 * Returns a string setting value, or fallback when the key is not registered.
 *
 * @param store Source settings store.
 * @param key Setting key.
 * @param fallback Value returned when key is absent.
 * @returns Current or fallback value.
 */
std::string setting_string_value_or_default(
  const SettingsStore &store, const SettingKey &key, std::string fallback);

/**
 * Returns an integer setting value, or fallback when the key is not registered.
 *
 * @param store Source settings store.
 * @param key Setting key.
 * @param fallback Value returned when key is absent.
 * @returns Current or fallback value.
 */
gint64 setting_integer_value_or_default(const SettingsStore &store,
                                        const SettingKey &key,
                                        gint64 fallback);

/**
 * Returns a floating-point setting value, or fallback when the key is not registered.
 *
 * @param store Source settings store.
 * @param key Setting key.
 * @param fallback Value returned when key is absent.
 * @returns Current or fallback value.
 */
gdouble setting_double_value_or_default(const SettingsStore &store,
                                        const SettingKey &key,
                                        gdouble fallback);

/**
 * Returns a boolean setting value, or fallback when the key is not registered.
 *
 * @param store Source settings store.
 * @param key Setting key.
 * @param fallback Value returned when key is absent.
 * @returns Current or fallback value.
 */
bool setting_boolean_value_or_default(const SettingsStore &store,
                                      const SettingKey &key, bool fallback);

/**
 * Updates a setting and marks it dirty when the value changes.
 *
 * @param store Target settings store.
 * @param key Setting key.
 * @param value New value.
 * @returns True when the key exists and the value was accepted.
 */
bool set_setting_value(
  SettingsStore *store, const SettingKey &key, SettingValue value);

/**
 * Checks whether one setting has unsaved changes.
 *
 * @param store Source settings store.
 * @param key Setting key.
 * @returns True when the setting is dirty.
 */
bool setting_is_dirty(const SettingsStore &store, const SettingKey &key);

/**
 * Checks whether any setting has unsaved changes.
 *
 * @param store Source settings store.
 * @returns True when at least one setting is dirty.
 */
bool settings_store_is_dirty(const SettingsStore &store);

} // namespace elder_terms
