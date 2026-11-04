// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "../../src/titlebar-effect-state.h"
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
    assert(std::abs(frame.title - .1 * multiplier) < 1e-9 && std::abs(frame.bell - .15 * multiplier) < 1e-9);
    assert(std::abs(titlebar_effect_opacity(frame, .25) - .2 * multiplier) < 1e-9);
    const auto independent = scale_titlebar_effect(original, strength, TitlebarEffectStrength::disabled);
    assert(independent.title == 0 && independent.bell == frame.bell);
  }
  TitlebarEffectState state;
  notify_titlebar_title(state, "A", 0, true);
  assert(!sample_titlebar_effect(state, 0).active);
  notify_titlebar_title(state, "A", 10, true);
  assert(!sample_titlebar_effect(state, 10).active);
  notify_titlebar_title(state, "B", 100, true);
  assert(sample_titlebar_effect(state, 100).active);
  assert(sample_titlebar_effect(state, 600100).phase == 0.5);
  notify_titlebar_title(state, "B", 700100, true);
  assert(!state.changed);
  assert(sample_titlebar_effect(state, 1200100).title == 0.1);
  assert(!state.cycle_start && state.fade_start);
  assert(!sample_titlebar_effect(state, 1320100).active);

  // Every position in a cycle can remember a change. A -> B -> A is not lost.
  for (const auto offset : {1, 200000, 600000, 1199999}) {
    state = {};
    notify_titlebar_title(state, "A", 0, true);
    notify_titlebar_title(state, "B", 0, true);
    notify_titlebar_title(state, "A", offset, true);
    notify_titlebar_title(state, "B", offset, true);
    const auto before = sample_titlebar_effect(state, 1199999);
    const auto boundary = sample_titlebar_effect(state, 1200000);
    assert(boundary.active && boundary.title == before.title && boundary.phase == 0);
    assert(!state.changed && state.cycle_start == 1200000);
    // A boundary event belongs to the new cycle and keeps that cycle's phase.
    notify_titlebar_title(state, "C", 1200000, true);
    assert(state.changed && sample_titlebar_effect(state, 1800000).phase == 0.5);
    assert(sample_titlebar_effect(state, 2400000).title == 0.1);
    assert(!sample_titlebar_effect(state, 3720000).active);
  }

  state = {};
  notify_titlebar_title(state, "A", 0, true);
  notify_titlebar_title(state, "B", 0, true);
  const auto fade = sample_titlebar_effect(state, 1260000);
  notify_titlebar_title(state, "C", 1260000, true);
  assert(std::abs(sample_titlebar_effect(state, 1260000).title - fade.title) < 1e-9);
  assert(sample_titlebar_effect(state, 1340000).title == 0.1);
  notify_titlebar_bell(state, 1340000);
  assert(sample_titlebar_effect(state, 1340000).bell == 0.15);
  notify_titlebar_bell(state, 1440000);
  assert(sample_titlebar_effect(state, 1440000).bell < 0.15);
  assert(sample_titlebar_effect(state, 1600000).bell == 0);
  assert(state.cycle_start == 1260000 && !state.changed);
  notify_titlebar_bell(state, 1600000);
  assert(sample_titlebar_effect(state, 1600000).bell == 0);
  notify_titlebar_bell(state, 1740000);
  assert(sample_titlebar_effect(state, 1740000).bell == 0.15);
  reset_titlebar_effect(state);
  assert(!sample_titlebar_effect(state, 1740000).active && state.title == "C");
  notify_titlebar_title(state, "Hidden", 1800000, false);
  notify_titlebar_title(state, "Hidden", 1900000, true);
  assert(!sample_titlebar_effect(state, 1900000).active);
  notify_titlebar_title(state, "Shown", 2000000, true);
  notify_titlebar_title(state, "New", 2000010, true);
  assert(!sample_titlebar_effect(state, 100000000).active);

  for (double phase = 0; phase < 1; phase += .05) {
    const TitlebarEffectFrame frame{phase, .1, .15, true};
    assert(std::abs(titlebar_effect_opacity(frame, 0) - titlebar_effect_opacity(frame, 1)) < 1e-9);
    for (double x = 0; x < 1; x += .01)
      assert(titlebar_effect_opacity(frame, x) >= .15 && titlebar_effect_opacity(frame, x) <= .2);
  }
  assert(titlebar_effect_opacity({.25, .1, 0, true}, .25) >
      titlebar_effect_opacity({.25, .1, 0, true}, .75));
  std::puts("PASS title cycles, boundary ordering, fade restart, BEL merging, visibility and bounded periodic color");
}
