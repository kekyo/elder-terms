// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace elder_terms {

/** Time-driven background effects. All timestamps use monotonic microseconds. */
struct TitlebarEffectState {
  /** Last final title, including connection-state suffixes; absent on first use. */
  std::optional<std::string> title;
  /** Start of the current 1.2-second cycle, or no running cycle. */
  std::optional<std::int64_t> cycle_start;
  /** Whether a different title arrived anywhere in the current cycle. */
  bool changed = false;
  /** Start of the final 120-millisecond fade, or no fade. */
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
  /** Title-band opacity, from zero to 0.10. */
  double title = 0;
  /** Whole-background flash opacity, from zero to 0.15. */
  double bell = 0;
  /** Whether another frame is needed, including the final fade. */
  bool active = false;
};

/**
 * Advances cycle boundaries before sampling a frame.
 * @param state Mutable state owned by one window.
 * @param now Monotonic time in microseconds.
 * @returns Current background effect; missed cycles are never replayed.
 */
TitlebarEffectFrame sample_titlebar_effect(TitlebarEffectState &state, std::int64_t now);

/**
 * Records a final title after the caller has applied it to the UI.
 * @param state State owned by one window.
 * @param title Final visible string, including transient suffixes.
 * @param now Monotonic time in microseconds.
 * @param enabled Whether the window is visible and title animation is enabled.
 * @remarks Initial and identical strings never start or extend an effect.
 */
void notify_titlebar_title(TitlebarEffectState &state, const std::string &title,
    std::int64_t now, bool enabled);

/**
 * Starts a bounded flash, merging repeated BELs without extending its lifetime.
 * @param state State owned by one window.
 * @param now Monotonic time in microseconds.
 */
void notify_titlebar_bell(TitlebarEffectState &state, std::int64_t now);

/**
 * Cancels all animation without forgetting the latest title.
 * @param state State owned by one window.
 */
void reset_titlebar_effect(TitlebarEffectState &state);

/**
 * Samples the periodic band and combines it with BEL at a bounded opacity.
 * @param frame Sampled time state.
 * @param position Horizontal position in turns; endpoints have equal values.
 * @returns Opacity in [0, 0.20] toward the contrasting lightness extreme.
 */
double titlebar_effect_opacity(const TitlebarEffectFrame &frame, double position);

} // namespace elder_terms
