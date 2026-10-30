// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once

#include <boxrig/ui.h>
#include "terminal-hyperlink-action.h"

namespace elder_terms {

/** Owns compiled rules and the current screen recognizers on the UI thread. */
struct TerminalLinks;

/** Counters for the OSC 8 regex cache within one terminal link context. */
struct TerminalLinksCacheStatistics {
  /** Number of attempts to construct a boxrig matcher. */
  std::uint64_t matcher_creations = 0;
  /** Number of completed regex outcomes reused from the cache. */
  std::uint64_t cache_hits = 0;
  /** Number of URI matches stopped at the synchronous work ceiling. */
  std::uint64_t work_ceiling_exhaustions = 0;
  /** Number of owned URI entries evicted to keep memory bounded. */
  std::uint64_t cache_evictions = 0;
  /** Current number of completed regex outcomes held in the cache. */
  std::size_t cache_entries = 0;
  /** Current number of owned URI bytes held in the cache. */
  std::size_t cache_uri_bytes = 0;
};

/**
 * Reads OSC 8 matcher and cache counters for diagnostics.
 * @param links Live link state.
 * @return Counters since this link state was created.
 */
TerminalLinksCacheStatistics terminal_links_cache_statistics(const TerminalLinks *links);

#if defined(ELDER_TERMS_CACHE_TESTING)
/**
 * Makes the next optional cache insertion fail for deterministic OOM tests.
 * @param links Live link state; matching must continue without the insertion.
 */
void terminal_links_fail_next_cache_insert_for_test(TerminalLinks *links);
#endif

/**
 * Creates ordered link recognition against an existing terminal view.
 * @param view Borrowed view, detached before its destruction.
 * @param rules Ordered OSC 8 and visible-text rules.
 * @return Owned link state; invalid rules are diagnosed and omitted.
 */
TerminalLinks *create_terminal_links(boxrig_ui_view *view, std::vector<HyperlinkActionRule> rules);

/**
 * Moves link presentation to another view of the same terminal.
 * @param links Live link state.
 * @param view Borrowed replacement, or nullptr while detached.
 */
void terminal_links_attach_view(TerminalLinks *links, boxrig_ui_view *view);

/**
 * Advances bounded recognition and updates link annotations.
 * @param links Live link state.
 * @param budget Positive maximum recognition work for this dispatch.
 * @return True when another immediate dispatch is needed.
 */
bool terminal_links_dispatch(TerminalLinks *links, std::size_t budget);

/**
 * Resolves a current pointer hit using retained captures and rule priority.
 * @param links Live link state with an attached view.
 * @param hit Revisioned pointer hit; stale and incomplete results are ignored.
 * @return Expanded action, rejection reason, or no matching rule.
 */
std::optional<TerminalHyperlinkResolution> terminal_links_resolve(
    TerminalLinks *links, const boxrig_ui_hit_test_result &hit);

/**
 * Copies the complete recognized text or OSC 8 target at a current pointer hit.
 * @param links Live link state with an attached view.
 * @param hit Revisioned pointer hit; stale and incomplete results are ignored.
 * @return Owned UTF-8 target, or no value for an absent or non-textual target.
 * @remarks Uses action rule priority without executing commands or expanding
 * the copied text. An action rejection does not prevent copying its target.
 */
std::optional<std::string> terminal_links_copy_target(
    TerminalLinks *links, const boxrig_ui_hit_test_result &hit);

/**
 * Clears annotations and unregisters recognizers before releasing rules.
 * @param links Owned state, or nullptr.
 */
void destroy_terminal_links(TerminalLinks *links);

}
