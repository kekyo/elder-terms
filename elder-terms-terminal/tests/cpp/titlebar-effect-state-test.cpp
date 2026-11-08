// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "../../../shared/src/connection-runtime/titlebar-effect-state.h"
#include <cassert>
#include <cmath>
#include <cstdio>

using namespace elder_terms;

int main() {
  const TitlebarEffectFrame original{.25, .1, .15, true};
  for (const auto strength : {TitlebarEffectStrength::disabled, TitlebarEffectStrength::weak,
      TitlebarEffectStrength::medium, TitlebarEffectStrength::strong}) {
    const auto frame = scale_titlebar_effect(original, strength, strength);
    const auto multiplier = titlebar_effect_multiplier(strength);
    assert(frame.phase == original.phase && frame.active == original.active);
    assert(std::abs(frame.receive - .1 * multiplier) < 1e-9 && std::abs(frame.bell - .15 * multiplier) < 1e-9);
    assert(std::abs(titlebar_effect_opacity(frame, .25) - .2 * multiplier) < 1e-9);
    const auto independent = scale_titlebar_effect(original, strength, TitlebarEffectStrength::disabled);
    assert(independent.receive == 0 && independent.bell == frame.bell);
  }
  TitlebarEffectState state;
  assert(!sample_titlebar_effect(state, 0).active);
  notify_titlebar_receive(state, 100);
  assert(sample_titlebar_effect(state, 100).active);
  assert(sample_titlebar_effect(state, 600100).phase == 0.5);
  assert(!state.received);
  assert(sample_titlebar_effect(state, 1200100).receive == 0.1);
  assert(!state.cycle_start && state.fade_start);
  const auto fading = sample_titlebar_effect(state, 1350100);
  assert(fading.active && std::abs(fading.receive - 0.05) < 1e-9);
  const auto fading_tail = sample_titlebar_effect(state, 1440100);
  assert(fading_tail.active && fading_tail.receive > 0 && fading_tail.receive < fading.receive);
  assert(!sample_titlebar_effect(state, 1500100).active);

  // Every position in a cycle can remember reception; bursts do not queue cycles.
  for (const auto offset : {1, 200000, 600000, 1199999}) {
    state = {};
    notify_titlebar_receive(state, 0);
    notify_titlebar_receive(state, offset);
    notify_titlebar_receive(state, offset);
    const auto before = sample_titlebar_effect(state, 1199999);
    const auto boundary = sample_titlebar_effect(state, 1200000);
    assert(boundary.active && boundary.receive == before.receive && boundary.phase == 0);
    assert(!state.received && state.cycle_start == 1200000);
    // A boundary event belongs to the new cycle and keeps that cycle's phase.
    notify_titlebar_receive(state, 1200000);
    assert(state.received && sample_titlebar_effect(state, 1800000).phase == 0.5);
    assert(sample_titlebar_effect(state, 2400000).receive == 0.1);
    assert(!sample_titlebar_effect(state, 3900000).active);
  }

  state = {};
  notify_titlebar_receive(state, 0);
  const auto fade = sample_titlebar_effect(state, 1260000);
  notify_titlebar_receive(state, 1260000);
  assert(std::abs(sample_titlebar_effect(state, 1260000).receive - fade.receive) < 1e-9);
  assert(sample_titlebar_effect(state, 1340000).receive == 0.1);
  notify_titlebar_bell(state, 1340000);
  assert(sample_titlebar_effect(state, 1340000).bell == 0.15);
  notify_titlebar_bell(state, 1440000);
  assert(sample_titlebar_effect(state, 1440000).bell < 0.15);
  assert(sample_titlebar_effect(state, 1600000).bell == 0);
  assert(state.cycle_start == 1260000 && !state.received);
  notify_titlebar_bell(state, 1600000);
  assert(sample_titlebar_effect(state, 1600000).bell == 0);
  notify_titlebar_bell(state, 1740000);
  assert(sample_titlebar_effect(state, 1740000).bell == 0.15);
  reset_titlebar_effect(state);
  assert(!sample_titlebar_effect(state, 1740000).active);
  notify_titlebar_receive(state, 2000000);
  notify_titlebar_receive(state, 2000010);
  const auto before_bell = sample_titlebar_effect(state, 2600000);
  notify_titlebar_bell(state, 2600000);
  const auto after_bell = sample_titlebar_effect(state, 2600000);
  assert(after_bell.phase == before_bell.phase && after_bell.receive == before_bell.receive);
  assert(state.received && state.cycle_start == 2000000);
  assert(!sample_titlebar_effect(state, 100000000).active);

  for (double phase = 0; phase < 1; phase += .05) {
    const TitlebarEffectFrame frame{phase, .1, .15, true};
    assert(std::abs(titlebar_effect_opacity(frame, 0) - titlebar_effect_opacity(frame, 1)) < 1e-9);
    for (double x = 0; x < 1; x += .01)
      assert(titlebar_effect_opacity(frame, x) >= .15 && titlebar_effect_opacity(frame, x) <= .2);
  }
  assert(titlebar_effect_opacity({.25, .1, 0, true}, .25) >
      titlebar_effect_opacity({.25, .1, 0, true}, .75));
  std::puts("PASS receive cycles, boundary ordering, fade restart, BEL merging, visibility and bounded periodic color");
}
