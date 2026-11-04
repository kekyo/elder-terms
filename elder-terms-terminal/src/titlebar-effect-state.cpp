// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "titlebar-effect-state.h"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace elder_terms {

static constexpr std::int64_t cycle_duration = 1200000;
static constexpr std::int64_t fade_duration = 120000;
static constexpr std::int64_t attack_duration = 80000;
static constexpr std::int64_t bell_duration = 260000;
static constexpr std::int64_t bell_interval = 400000;

static double smooth(double value) {
  const auto t = std::clamp(value, 0.0, 1.0);
  return t * t * (3 - 2 * t);
}

TitlebarEffectFrame sample_titlebar_effect(TitlebarEffectState &state, std::int64_t now) {
  // At most one remembered cycle exists. A delayed frame consumes it and then
  // stops; it cannot replay a queue of changes accumulated while not drawing.
  while (state.cycle_start && now >= *state.cycle_start + cycle_duration) {
    const auto boundary = *state.cycle_start + cycle_duration;
    if (state.changed) {
      state.changed = false;
      state.attacking = false;
      state.cycle_start = boundary;
    } else {
      state.cycle_start.reset();
      state.fade_start = boundary;
    }
  }
  TitlebarEffectFrame frame;
  if (state.cycle_start) {
    const auto elapsed = std::max<std::int64_t>(0, now - *state.cycle_start);
    frame.phase = static_cast<double>(elapsed) / cycle_duration;
    frame.title = .1 * (state.attacking ? state.attack_from +
        (1 - state.attack_from) * smooth(static_cast<double>(elapsed) / attack_duration) : 1);
    frame.active = true;
  } else if (state.fade_start) {
    frame.title = .1 * (1 - smooth(static_cast<double>(now - *state.fade_start) / fade_duration));
    if (now >= *state.fade_start + fade_duration) state.fade_start.reset();
    else frame.active = true;
  }
  if (state.bell_start && now < *state.bell_start + bell_duration) {
    frame.bell = .15 * (1 - smooth(static_cast<double>(now - *state.bell_start) / bell_duration));
    frame.active = true;
  }
  return frame;
}

void notify_titlebar_title(TitlebarEffectState &state, const std::string &title,
    std::int64_t now, bool enabled) {
  const auto frame = sample_titlebar_effect(state, now);
  const bool changed = state.title && *state.title != title;
  state.title = title;
  if (!changed || !enabled) return;
  if (state.cycle_start) {
    state.changed = true;
  } else {
    state.attack_from = frame.title / .1;
    state.attacking = true;
    state.cycle_start = now;
    state.fade_start.reset();
    state.changed = false;
  }
}

void notify_titlebar_bell(TitlebarEffectState &state, std::int64_t now) {
  if (!state.bell_start || now >= *state.bell_start + bell_interval) state.bell_start = now;
}

void reset_titlebar_effect(TitlebarEffectState &state) {
  auto title = std::move(state.title);
  state = {};
  state.title = std::move(title);
}

TitlebarEffectFrame scale_titlebar_effect(TitlebarEffectFrame frame,
    TitlebarEffectStrength bell, TitlebarEffectStrength title) {
  const auto bell_scale = titlebar_effect_multiplier(bell);
  const auto title_scale = titlebar_effect_multiplier(title);
  frame.bell *= bell_scale;
  frame.title *= title_scale;
  frame.limit *= std::max(bell_scale, title_scale);
  return frame;
}

double titlebar_effect_opacity(const TitlebarEffectFrame &frame, double position) {
  const auto wave = (1 + std::cos(2 * std::numbers::pi * (position - frame.phase))) / 2;
  return std::clamp(frame.bell + frame.title * std::pow(wave, 8), 0.0, frame.limit);
}

} // namespace elder_terms
