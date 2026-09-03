#pragma once

#include <string>
#include <variant>
#include <vector>

#include <glib.h>

#include <elder-terms/export.h>
#include <elder-terms/settings/hyperlink-settings.h>
#include <elder-terms/settings/macro-settings.h>

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
 * Identifies the active source for a setting value.
 */
enum class SettingValueSource {
  /** Value comes from the compiled-in setting definition. */
  built_in,
  /** Value comes from global.ini. */
  global,
  /** Value is an explicit connection, startup, or in-memory override. */
  override,
};

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
  /** Value used when no explicit override is present. */
  SettingValue fallback_value;
  /** Source of fallback_value. */
  SettingValueSource fallback_source = SettingValueSource::built_in;
  /** Current effective value, initialized from fallback_value. */
  SettingValue value;
  /** True when value is an explicit connection, startup, or in-memory
   * override. */
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
  /** True when terminal link recognition and command activation is enabled. */
  bool hyperlink_actions_enabled = true;
  /** Effective ordered link recognition and command rules. */
  std::vector<HyperlinkActionRule> hyperlink_rules;
  /** True when global.ini explicitly configures hyperlink actions. */
  bool hyperlink_settings_configured = false;
  /** True after hyperlink settings are changed in memory. */
  bool hyperlink_settings_dirty = false;
  /** Ordered connection macro rules. */
  std::vector<MacroRule> macro_rules;
  /** True after the macro rule collection is changed in memory. */
  bool macro_rules_dirty = false;
};

/**
 * Creates a setting key from section and key names.
 *
 * @param section INI section name.
 * @param name INI key name.
 * @returns Setting key value.
 */
ELDER_TERMS_API SettingKey make_setting_key(std::string section,
                                           std::string name);

/**
 * Creates a settings store initialized with definition defaults.
 *
 * @param definitions Registered setting definitions.
 * @returns Settings store containing all registered keys.
 */
ELDER_TERMS_API SettingsStore
create_settings_store(std::vector<SettingDefinition> definitions);

/**
 * Replaces all ordered connection macro rules.
 *
 * @param store Target settings store.
 * @param rules New ordered rule collection.
 */
ELDER_TERMS_API void set_macro_rules(SettingsStore *store,
                                     std::vector<MacroRule> rules);

/**
 * Replaces the global terminal link action settings.
 *
 * @param store Target settings store.
 * @param enabled Whether terminal link activation is enabled.
 * @param rules New ordered rule collection.
 */
ELDER_TERMS_API void
set_hyperlink_actions(SettingsStore *store, bool enabled,
                      std::vector<HyperlinkActionRule> rules);

/**
 * Loads all registered keys from a parsed GLib key file.
 *
 * @param store Target settings store.
 * @param key_file Parsed INI file.
 * @param warnings Non-fatal warning sink.
 */
ELDER_TERMS_API void
load_settings_store_from_key_file(SettingsStore *store, GKeyFile *key_file,
                                  std::vector<std::string> *warnings);

/**
 * Returns the value for a setting, or fallback when the key is not registered.
 *
 * @param store Source settings store.
 * @param key Setting key.
 * @param fallback Value returned when key is absent.
 * @returns Current or fallback value.
 */
ELDER_TERMS_API SettingValue setting_value_or_default(
  const SettingsStore &store, const SettingKey &key, SettingValue fallback);

/**
 * Returns the inherited fallback for a setting.
 *
 * @param store Source settings store.
 * @param key Setting key.
 * @param fallback Value returned when key is not registered.
 * @returns Registered fallback or the caller-provided fallback.
 */
ELDER_TERMS_API SettingValue setting_fallback_value(
    const SettingsStore &store, const SettingKey &key, SettingValue fallback);

/**
 * Returns the active source for a setting.
 *
 * @param store Source settings store.
 * @param key Setting key.
 * @returns Override, global, or built-in source.
 */
ELDER_TERMS_API SettingValueSource
setting_value_source(const SettingsStore &store, const SettingKey &key);

/**
 * Returns the source used after an explicit override is cleared.
 *
 * @param store Source settings store.
 * @param key Setting key.
 * @returns Global or built-in fallback source.
 */
ELDER_TERMS_API SettingValueSource
setting_fallback_source(const SettingsStore &store, const SettingKey &key);

/**
 * Returns a string setting value, or fallback when the key is not registered.
 *
 * @param store Source settings store.
 * @param key Setting key.
 * @param fallback Value returned when key is absent.
 * @returns Current or fallback value.
 */
ELDER_TERMS_API std::string setting_string_value_or_default(
  const SettingsStore &store, const SettingKey &key, std::string fallback);

/**
 * Returns an integer setting value, or fallback when the key is not registered.
 *
 * @param store Source settings store.
 * @param key Setting key.
 * @param fallback Value returned when key is absent.
 * @returns Current or fallback value.
 */
ELDER_TERMS_API gint64
setting_integer_value_or_default(const SettingsStore &store,
                                 const SettingKey &key, gint64 fallback);

/**
 * Returns a floating-point setting value, or fallback when the key is not registered.
 *
 * @param store Source settings store.
 * @param key Setting key.
 * @param fallback Value returned when key is absent.
 * @returns Current or fallback value.
 */
ELDER_TERMS_API gdouble
setting_double_value_or_default(const SettingsStore &store,
                                const SettingKey &key, gdouble fallback);

/**
 * Returns a boolean setting value, or fallback when the key is not registered.
 *
 * @param store Source settings store.
 * @param key Setting key.
 * @param fallback Value returned when key is absent.
 * @returns Current or fallback value.
 */
ELDER_TERMS_API bool
setting_boolean_value_or_default(const SettingsStore &store,
                                 const SettingKey &key, bool fallback);

/**
 * Updates a setting and marks it dirty when the value changes.
 *
 * @param store Target settings store.
 * @param key Setting key.
 * @param value New value.
 * @returns True when the key exists and the value was accepted.
 *
 * @remarks A changed value equal to its inherited fallback becomes
 * non-explicit. Use set_explicit_setting_value() to persist an override equal
 * to its fallback.
 */
ELDER_TERMS_API bool set_setting_value(
  SettingsStore *store, const SettingKey &key, SettingValue value);

/**
 * Updates a setting as an explicit override and marks it dirty when the
 * persisted state changes.
 *
 * @param store Target settings store.
 * @param key Setting key.
 * @param value New explicit value.
 * @returns True when the key exists and the value was accepted.
 */
ELDER_TERMS_API bool set_explicit_setting_value(
  SettingsStore *store, const SettingKey &key, SettingValue value);

/**
 * Clears a setting's explicit override and restores its inherited fallback.
 *
 * @param store Target settings store.
 * @param key Setting key.
 * @returns True when the key exists.
 */
ELDER_TERMS_API bool clear_explicit_setting_value(SettingsStore *store,
                                                  const SettingKey &key);

/**
 * Checks whether one setting has an explicit loaded or in-memory override.
 *
 * @param store Source settings store.
 * @param key Setting key.
 * @returns True when the setting is explicit rather than inherited.
 */
ELDER_TERMS_API bool
setting_has_explicit_value(const SettingsStore &store,
                           const SettingKey &key);

/**
 * Checks whether a setting is configured outside the compiled-in defaults.
 *
 * @param store Source settings store.
 * @param key Setting key.
 * @returns True for a global value or explicit override.
 */
ELDER_TERMS_API bool
setting_has_configured_value(const SettingsStore &store,
                             const SettingKey &key);

/**
 * Replaces inherited fallbacks while preserving explicit values and dirtiness.
 *
 * @param store Draft settings store to update.
 * @param fallbacks Store whose effective values become the new fallbacks.
 *
 * @remarks Any configured value in fallbacks is treated as a global fallback.
 * The connection-specific [general] name setting is never rebased.
 */
ELDER_TERMS_API void
rebase_settings_store_fallbacks(SettingsStore *store,
                                const SettingsStore &fallbacks);

/**
 * Checks whether one setting has unsaved changes.
 *
 * @param store Source settings store.
 * @param key Setting key.
 * @returns True when the setting is dirty.
 */
ELDER_TERMS_API bool setting_is_dirty(const SettingsStore &store,
                                      const SettingKey &key);

/**
 * Checks whether any setting has unsaved changes.
 *
 * @param store Source settings store.
 * @returns True when at least one setting is dirty.
 */
ELDER_TERMS_API bool settings_store_is_dirty(const SettingsStore &store);

} // namespace elder_terms
