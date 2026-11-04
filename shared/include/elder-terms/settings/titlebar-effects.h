// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once
#include <elder-terms/settings/settings-store.h>

namespace elder_terms {

/** Independently selectable intensity for a titlebar background notification. */
enum class TitlebarEffectStrength {
  disabled, /**< No visual notification. */
  weak, /**< Original highlight intensity. */
  medium, /**< One and a half times the original intensity; the default. */
  strong, /**< Twice the original intensity. */
};

/**
 * Returns the opacity multiplier relative to the original highlight.
 * @param strength Selected intensity.
 * @returns Zero, one, one and a half, or two.
 */
constexpr double titlebar_effect_multiplier(TitlebarEffectStrength strength) {
  return strength == TitlebarEffectStrength::weak ? 1 :
      strength == TitlebarEffectStrength::medium ? 1.5 :
      strength == TitlebarEffectStrength::strong ? 2 : 0;
}

/**
 * Defines a validated disabled/weak/medium/strong setting, defaulting to medium.
 * @param key Setting to register.
 * @returns Definition with a string value and validation.
 */
ELDER_TERMS_API SettingDefinition titlebar_effect_setting_definition(SettingKey key);

/**
 * Reads the effective intensity after connection and global inheritance.
 * @param store Layered settings.
 * @param key Registered titlebar intensity setting.
 * @returns Effective intensity, defaulting to medium.
 */
ELDER_TERMS_API TitlebarEffectStrength titlebar_effect_strength(const SettingsStore &store, const SettingKey &key);

} // namespace elder_terms
