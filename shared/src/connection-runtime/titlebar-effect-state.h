// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once

#include <cstdint>
#include <elder-terms/settings/titlebar-effects.h>
#include <optional>

namespace elder_terms {

/** Time-driven background effects. All timestamps use monotonic microseconds. */
struct TitlebarEffectState {
  /** Start of the current 1.2-second cycle, or no running cycle. */
  std::optional<std::int64_t> cycle_start;
  /** Whether more data arrived anywhere in the current cycle. */
  bool received = false;
  /** Start of the final 300-millisecond fade, or no fade. */
  std::optional<std::int64_t> fade_start;
  /** Initial opacity when starting or resuming a cycle during the final fade. */
  double attack_from = 0;
  /** Whether the current cycle is the first cycle of this run. */
  bool attacking = false;
  /** Last accepted BEL, also enforcing a 400-millisecond retrigger interval. */
  std::optional<std::int64_t> bell_start;
};

/** One sampled frame, independent of GTK and of audio playback. */
struct TitlebarEffectFrame {
  /** Horizontal position in turns, increasing left to right. */
  double phase = 0;
  /** Receive-band opacity; the unscaled weak maximum is 0.10. */
  double receive = 0;
  /** Whole-background flash opacity; the unscaled weak maximum is 0.15. */
  double bell = 0;
  /** Whether another frame is needed, including the final fade. */
  bool active = false;
  /** Maximum combined opacity for the selected strengths. */
  double limit = .2;
};

/**
 * Advances cycle boundaries before sampling a frame.
 * @param state Mutable state owned by one window.
 * @param now Monotonic time in microseconds.
 * @returns Current background effect; missed cycles are never replayed.
 */
TitlebarEffectFrame sample_titlebar_effect(TitlebarEffectState &state, std::int64_t now);

/**
 * Records received data, starting or continuing a cycle without queuing events.
 * @param state State owned by one window.
 * @param now Monotonic time in microseconds.
 * @remarks A receive at a cycle boundary belongs to the new cycle.
 */
void notify_titlebar_receive(TitlebarEffectState &state, std::int64_t now);

/**
 * Starts a bounded flash, merging repeated BELs without extending its lifetime.
 * @param state State owned by one window.
 * @param now Monotonic time in microseconds.
 */
void notify_titlebar_bell(TitlebarEffectState &state, std::int64_t now);

/**
 * Cancels all animation and remembered receive activity.
 * @param state State owned by one window.
 */
void reset_titlebar_effect(TitlebarEffectState &state);

/**
 * Scales a weak-intensity time sample without changing its phase or duration.
 * @param frame Original time sample.
 * @param bell BEL intensity.
 * @param receive Moving-band intensity.
 * @returns Scaled frame, with the combined limit scaled by the stronger choice.
 */
TitlebarEffectFrame scale_titlebar_effect(TitlebarEffectFrame frame,
    TitlebarEffectStrength bell, TitlebarEffectStrength receive);

/**
 * Samples the periodic band and combines it with BEL at a bounded opacity.
 * @param frame Sampled time state.
 * @param position Horizontal position in turns; endpoints have equal values.
 * @returns Opacity in [0, frame.limit] toward the contrasting lightness extreme.
 */
double titlebar_effect_opacity(const TitlebarEffectFrame &frame, double position);

} // namespace elder_terms
