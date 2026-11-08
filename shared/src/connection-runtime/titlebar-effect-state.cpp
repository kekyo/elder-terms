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
static constexpr std::int64_t fade_duration = 300000;
static constexpr std::int64_t attack_duration = 80000;
static constexpr std::int64_t bell_duration = 260000;
static constexpr std::int64_t bell_interval = 400000;

static double smooth(double value) {
  const auto t = std::clamp(value, 0.0, 1.0);
  return t * t * (3 - 2 * t);
}

TitlebarEffectFrame sample_titlebar_effect(TitlebarEffectState &state, std::int64_t now) {
  // At most one remembered cycle exists. A delayed frame consumes it and then
  // stops; it cannot replay a queue of receives accumulated while not drawing.
  while (state.cycle_start && now >= *state.cycle_start + cycle_duration) {
    const auto boundary = *state.cycle_start + cycle_duration;
    if (state.received) {
      state.received = false;
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
    frame.receive = .1 * (state.attacking ? state.attack_from +
        (1 - state.attack_from) * smooth(static_cast<double>(elapsed) / attack_duration) : 1);
    frame.active = true;
  } else if (state.fade_start) {
    frame.receive = .1 * (1 - smooth(static_cast<double>(now - *state.fade_start) / fade_duration));
    if (now >= *state.fade_start + fade_duration) state.fade_start.reset();
    else frame.active = true;
  }
  if (state.bell_start && now < *state.bell_start + bell_duration) {
    frame.bell = .15 * (1 - smooth(static_cast<double>(now - *state.bell_start) / bell_duration));
    frame.active = true;
  }
  return frame;
}

void notify_titlebar_receive(TitlebarEffectState &state, std::int64_t now) {
  const auto frame = sample_titlebar_effect(state, now);
  if (state.cycle_start) {
    state.received = true;
  } else {
    state.attack_from = frame.receive / .1;
    state.attacking = true;
    state.cycle_start = now;
    state.fade_start.reset();
    state.received = false;
  }
}

void notify_titlebar_bell(TitlebarEffectState &state, std::int64_t now) {
  if (!state.bell_start || now >= *state.bell_start + bell_interval) state.bell_start = now;
}

void reset_titlebar_effect(TitlebarEffectState &state) {
  state = {};
}

TitlebarEffectFrame scale_titlebar_effect(TitlebarEffectFrame frame,
    TitlebarEffectStrength bell, TitlebarEffectStrength receive) {
  const auto bell_scale = titlebar_effect_multiplier(bell);
  const auto receive_scale = titlebar_effect_multiplier(receive);
  frame.bell *= bell_scale;
  frame.receive *= receive_scale;
  frame.limit *= std::max(bell_scale, receive_scale);
  return frame;
}

double titlebar_effect_opacity(const TitlebarEffectFrame &frame, double position) {
  const auto wave = (1 + std::cos(2 * std::numbers::pi * (position - frame.phase))) / 2;
  return std::clamp(frame.bell + frame.receive * std::pow(wave, 8), 0.0, frame.limit);
}

} // namespace elder_terms
