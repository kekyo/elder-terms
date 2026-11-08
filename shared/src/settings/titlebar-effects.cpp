// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include <elder-terms/settings/titlebar-effects.h>
#include <utility>

namespace elder_terms {

SettingDefinition titlebar_effect_setting_definition(SettingKey key) {
  return {.key = std::move(key), .default_value = std::string("medium"),
      .validate = +[](const SettingValue &value, std::string *reason) {
        const auto *text = std::get_if<std::string>(&value);
        if (text && (*text == "disabled" || *text == "weak" || *text == "medium" || *text == "strong")) return true;
        *reason = "must be disabled, weak, medium, or strong";
        return false;
      }};
}

TitlebarEffectStrength titlebar_effect_strength(const SettingsStore &store, const SettingKey &key) {
  const auto value = setting_string_value_or_default(store, key, "medium");
  if (value == "disabled") return TitlebarEffectStrength::disabled;
  if (value == "weak") return TitlebarEffectStrength::weak;
  if (value == "strong") return TitlebarEffectStrength::strong;
  return TitlebarEffectStrength::medium;
}

} // namespace elder_terms
