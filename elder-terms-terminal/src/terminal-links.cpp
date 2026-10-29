// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "terminal-links.h"
#include <elder-terms/settings/regular-expression.h>

#include <algorithm>
#include <array>
#include <glib.h>
#include <list>
#include <new>
#include <string_view>
#include <unordered_map>

namespace elder_terms {

struct LinkRule {
  HyperlinkActionRule rule;
  std::shared_ptr<RegularExpressionState> program;
  std::uint64_t recognizer = 0;
  std::uint64_t rescan_revision = 0;
  std::uint64_t generation = 0;
};

struct Osc8CacheKey {
  boxrig_terminal *terminal;
  std::uint64_t rule_generation;
  const RegularExpressionState *program;
  std::string_view uri;

  bool operator==(const Osc8CacheKey &other) const {
    return terminal == other.terminal && rule_generation == other.rule_generation &&
        program == other.program && uri == other.uri;
  }
};

struct Osc8CacheHash {
  std::size_t operator()(const Osc8CacheKey &key) const {
#if defined(ELDER_TERMS_CACHE_TESTING)
    // All keys collide in the functional fixture, so URI equality is tested.
    (void)key;
    return 0u;
#else
    std::size_t value = std::hash<std::string_view>{}(key.uri);
    value ^= std::hash<boxrig_terminal *>{}(key.terminal) + 0x9e3779b9u + (value << 6) + (value >> 2);
    value ^= std::hash<std::uint64_t>{}(key.rule_generation) + 0x9e3779b9u + (value << 6) + (value >> 2);
    value ^= std::hash<const RegularExpressionState *>{}(key.program) + 0x9e3779b9u + (value << 6) + (value >> 2);
    return value;
#endif
  }
};

struct Osc8CacheEntry {
  std::string uri;
  Osc8CacheKey key{};
  std::shared_ptr<boxrig_regex_result> result;
};

struct TerminalLinks {
  boxrig_terminal *terminal = nullptr;
  boxrig_ui_view *view = nullptr;
  std::vector<LinkRule> rules;
  TerminalLinksCacheStatistics cache_statistics;
  std::list<Osc8CacheEntry> osc8_cache;
  std::unordered_map<Osc8CacheKey, std::list<Osc8CacheEntry>::iterator, Osc8CacheHash> osc8_index;
  std::size_t osc8_cache_bytes = 0;
#if defined(ELDER_TERMS_CACHE_TESTING)
  bool fail_next_cache_insert = false;
#endif
};

TerminalLinksCacheStatistics terminal_links_cache_statistics(const TerminalLinks *links) {
  auto result = links->cache_statistics;
  result.cache_entries = links->osc8_cache.size();
  result.cache_uri_bytes = links->osc8_cache_bytes;
  return result;
}

#if defined(ELDER_TERMS_CACHE_TESTING)
void terminal_links_fail_next_cache_insert_for_test(TerminalLinks *links) {
  links->fail_next_cache_insert = true;
}
#endif

static void clear_annotations(TerminalLinks *links, std::uint64_t source) {
  if (links->view) boxrig_ui_view_clear_text_annotations(links->view, source);
}

static bool restart_recognizer(TerminalLinks *links, LinkRule &rule) {
  if (rule.recognizer) {
    clear_annotations(links, rule.recognizer);
    boxrig_terminal_unregister_text_recognizer(links->terminal, rule.recognizer);
  }
  rule.recognizer = 0;
  rule.rescan_revision = 0;
  const auto limits = boxrig_text_recognizer_limits_default();
  const auto status = boxrig_terminal_register_compiled_text_recognizer(links->terminal,
      rule.program.get(), BOXRIG_TEXT_RECOGNIZER_SCOPE_BOTH, &limits, &rule.recognizer);
  if (status != BOXRIG_STATUS_OK) g_warning("Link rule %s: registration status %d", rule.rule.id.c_str(), status);
  return status == BOXRIG_STATUS_OK;
}

TerminalLinks *create_terminal_links(boxrig_ui_view *view, std::vector<HyperlinkActionRule> rules) {
  auto *links = new TerminalLinks;
  links->terminal = boxrig_ui_view_terminal(view);
  links->view = view;
  const auto limits = boxrig_text_recognizer_limits_default();
  for (auto &rule : rules) {
    std::string reason;
    std::shared_ptr<RegularExpressionState> program(create_regular_expression(rule.pattern, &reason), destroy_regular_expression);
    if (!program) {
      g_warning("Link rule %s: %s", rule.id.c_str(), reason.c_str());
      continue;
    }
    std::uint64_t recognizer = 0;
    if (rule.recognition_source == HyperlinkRecognitionSource::terminal_text) {
      const auto status = boxrig_terminal_register_compiled_text_recognizer(links->terminal,
          program.get(), BOXRIG_TEXT_RECOGNIZER_SCOPE_BOTH, &limits, &recognizer);
      if (status != BOXRIG_STATUS_OK) {
        g_warning("Link rule %s: recognizer status %d", rule.id.c_str(), status);
        continue;
      }
    }
    links->rules.push_back({std::move(rule), std::move(program), recognizer, 0,
        static_cast<std::uint64_t>(links->rules.size() + 1)});
  }
  return links;
}

void terminal_links_attach_view(TerminalLinks *links, boxrig_ui_view *view) {
  for (const auto &rule : links->rules) {
    if (rule.recognizer) clear_annotations(links, rule.recognizer);
  }
  links->view = view;
  if (view) {
    for (auto &rule : links->rules) {
      if (rule.rule.recognition_source == HyperlinkRecognitionSource::terminal_text) {
        // The rescan API only accepts sticky event loss. A new view instead
        // needs a fresh baseline while retaining the immutable compiled program.
        restart_recognizer(links, rule);
      }
    }
  }
}

static void update_annotations(TerminalLinks *links, const boxrig_text_recognizer_event &event) {
  if (!links->view) return;
  std::size_t required = 0;
  auto status = boxrig_terminal_query_text_recognizer_line_results(links->terminal,
      event.recognizer_id, event.screen, event.line_id, event.line_revision,
      event.result_set_revision, nullptr, 0, &required);
  // A later input revision can supersede queued line events. The view omits
  // obsolete ranges; current recognizer events will replace the changed line.
  if (status == BOXRIG_STATUS_STALE) return;
  if ((status != BOXRIG_STATUS_OK && status != BOXRIG_STATUS_LIMIT_EXCEEDED) || required > 4096) {
    clear_annotations(links, event.recognizer_id);
    return;
  }
  std::vector<boxrig_text_recognition_result> results(required);
  status = boxrig_terminal_query_text_recognizer_line_results(links->terminal,
      event.recognizer_id, event.screen, event.line_id, event.line_revision,
      event.result_set_revision, results.data(), results.size(), &required);
  if (status != BOXRIG_STATUS_OK) {
    clear_annotations(links, event.recognizer_id);
    return;
  }
  std::vector<boxrig_ui_text_annotation> annotations;
  annotations.reserve(results.size());
  for (const auto &result : results) {
    boxrig_ui_text_annotation annotation{};
    annotation.source_id = result.recognizer_id;
    annotation.item_id = result.match_id;
    annotation.range = result.range;
    annotation.style.field_mask = BOXRIG_UI_ANNOTATION_UNDERLINE_STYLE;
    annotation.style.underline_style = BOXRIG_UNDERLINE_SINGLE;
    annotations.push_back(annotation);
  }
  if (boxrig_ui_view_replace_text_annotations(links->view, event.recognizer_id, event.screen,
      event.line_id, event.line_revision, annotations.data(), annotations.size()) != BOXRIG_UI_OK)
    clear_annotations(links, event.recognizer_id);
}

bool terminal_links_dispatch(TerminalLinks *links, std::size_t budget) {
  // Sticky loss must be handled even when the event queue itself overflowed.
  bool rescanning = false;
  for (auto &rule : links->rules) {
    if (rule.recognizer && boxrig_terminal_text_recognizer_needs_rescan(links->terminal, rule.recognizer)) {
      clear_annotations(links, rule.recognizer);
      const auto revision = boxrig_terminal_text_revision(links->terminal);
      // Publishing a complete result set may itself exceed event capacity.
      // Retry that snapshot once, then wait for changed input instead of spinning.
      if (rule.rescan_revision != revision) {
        rule.rescan_revision = revision;
        rescanning = boxrig_terminal_rescan_text_recognizer(links->terminal, rule.recognizer) == BOXRIG_STATUS_OK || rescanning;
      }
    }
  }
  std::size_t used = 0;
  bool more = false;
  const bool queued = boxrig_terminal_text_recognizer_event_peek(links->terminal, nullptr);
  if (!queued && boxrig_terminal_resume_text_recognizers(links->terminal, budget, &used, &more) != BOXRIG_STATUS_OK) {
    for (const auto &rule : links->rules) if (rule.recognizer) clear_annotations(links, rule.recognizer);
    return false;
  }
  boxrig_text_recognizer_event event{};
  for (std::size_t count = 0; count < 64 && boxrig_terminal_text_recognizer_event_peek(links->terminal, &event); ++count) {
    boxrig_terminal_text_recognizer_event_consume(links->terminal);
    if (event.kind == BOXRIG_TEXT_RECOGNIZER_EVENT_LINE_REPLACED) {
      update_annotations(links, event);
    } else if (event.kind == BOXRIG_TEXT_RECOGNIZER_EVENT_LINE_EVICTED) {
      // Retired lines can be cleared without replacing surviving matches.
      if (links->view && boxrig_ui_view_clear_text_annotation_line(links->view,
          event.recognizer_id, event.screen, event.line_id) != BOXRIG_UI_OK)
        clear_annotations(links, event.recognizer_id);
    } else {
      clear_annotations(links, event.recognizer_id);
      // Loss is retried at the next dispatch through the revision guard above.
      if (event.kind == BOXRIG_TEXT_RECOGNIZER_EVENT_RESCAN_REQUIRED) more = true;
    }
  }
  return more || queued || rescanning || boxrig_terminal_text_recognizer_event_peek(links->terminal, nullptr);
}

struct Osc8RegexOutcome {
  bool definite = false;
  std::shared_ptr<boxrig_regex_result> result;
};

static Osc8RegexOutcome match_osc8(TerminalLinks *links,
    const LinkRule &rule, const boxrig_ui_hit_test_result &hit) {
  if (!hit.hyperlink_uri || hit.hyperlink_uri_length == 0 || hit.hyperlink_uri_length > 8192) return {};
  boxrig_regex_matcher *matcher = nullptr;
  std::size_t error = 0;
  links->cache_statistics.matcher_creations++;
  if (boxrig_regex_matcher_create(rule.program.get(), reinterpret_cast<const uint8_t *>(hit.hyperlink_uri),
      hit.hyperlink_uri_length, 0, &error, &matcher) != BOXRIG_STATUS_OK) return {};
  std::unique_ptr<boxrig_regex_matcher, decltype(&boxrig_regex_matcher_destroy)> owner(matcher, boxrig_regex_matcher_destroy);
  boxrig_regex_progress progress = BOXRIG_REGEX_IN_PROGRESS;
  boxrig_regex_match match{};
  // Pointer handling has a finite work ceiling as well as a URI byte bound.
  for (std::size_t remaining = 1000000; remaining && progress == BOXRIG_REGEX_IN_PROGRESS;) {
    std::size_t used = 0;
    // Completing after an exact budget boundary may consume no further work.
    if (boxrig_regex_matcher_resume(matcher, std::min(remaining, std::size_t{4096}), &used,
        &progress, &match) != BOXRIG_STATUS_OK || (used == 0 && progress == BOXRIG_REGEX_IN_PROGRESS)) return {};
    remaining -= used;
  }
  if (progress == BOXRIG_REGEX_NOT_FOUND ||
      (progress == BOXRIG_REGEX_MATCH && (match.start != 0 || match.end != hit.hyperlink_uri_length)))
    return {true, {}};
  if (progress == BOXRIG_REGEX_IN_PROGRESS) {
    links->cache_statistics.work_ceiling_exhaustions++;
    return {};
  }
  if (progress != BOXRIG_REGEX_MATCH) return {};
  boxrig_regex_result *result = nullptr;
  if (boxrig_regex_matcher_result(matcher, &result) != BOXRIG_STATUS_OK) return {};
  return {true, {result, boxrig_regex_result_destroy}};
}

static std::shared_ptr<boxrig_regex_result> cached_match_osc8(TerminalLinks *links,
    const LinkRule &rule, const boxrig_ui_hit_test_result &hit) {
  if (!hit.hyperlink_uri || hit.hyperlink_uri_length == 0 || hit.hyperlink_uri_length > 8192) return {};
  const Osc8CacheKey key{links->terminal, rule.generation, rule.program.get(),
      {hit.hyperlink_uri, hit.hyperlink_uri_length}};
  const auto cached = links->osc8_index.find(key);
  if (cached != links->osc8_index.end()) {
    links->cache_statistics.cache_hits++;
    links->osc8_cache.splice(links->osc8_cache.begin(), links->osc8_cache, cached->second);
    return cached->second->result;
  }
  const auto outcome = match_osc8(links, rule, hit);
  if (!outcome.definite) return outcome.result;
  try {
#if defined(ELDER_TERMS_CACHE_TESTING)
    if (links->fail_next_cache_insert) {
      links->fail_next_cache_insert = false;
      throw std::bad_alloc{};
    }
#endif
    links->osc8_cache.emplace_front();
    auto entry = links->osc8_cache.begin();
    try {
      entry->uri.assign(hit.hyperlink_uri, hit.hyperlink_uri_length);
      entry->key = {links->terminal, rule.generation, rule.program.get(), entry->uri};
      entry->result = outcome.result;
      links->osc8_index.emplace(entry->key, entry);
      links->osc8_cache_bytes += entry->uri.size();
    } catch (...) {
      links->osc8_cache.erase(entry);
      throw;
    }
    while (links->osc8_cache.size() > 128 || links->osc8_cache_bytes > 256 * 1024) {
      auto last = std::prev(links->osc8_cache.end());
      links->osc8_cache_bytes -= last->uri.size();
      links->osc8_index.erase(last->key);
      links->osc8_cache.erase(last);
      links->cache_statistics.cache_evictions++;
    }
  } catch (const std::bad_alloc &) {
    // Cache allocation is optional; the completed regex outcome remains valid.
  }
  return outcome.result;
}

std::optional<TerminalHyperlinkResolution> terminal_links_resolve(
    TerminalLinks *links, const boxrig_ui_hit_test_result &hit) {
  if (!links->view || !hit.inside_grid || !hit.text_position_valid ||
      (hit.overlay_roles & (BOXRIG_UI_OVERLAY_IME | BOXRIG_UI_OVERLAY_CANDIDATES))) return std::nullopt;
  // This also validates layout, text and annotation authority before reading a
  // borrowed OSC 8 pointer from the hit. Never dereference stale URI storage.
  std::array<boxrig_ui_text_annotation, 32> annotations{};
  std::size_t count = 0;
  if (boxrig_ui_view_query_text_annotation_hits(links->view, &hit, BOXRIG_UI_ANNOTATION_HIT_ALL,
      annotations.data(), annotations.size(), &count, nullptr) != BOXRIG_UI_OK) return std::nullopt;
  std::optional<TerminalHyperlinkResolution> rejection;
  for (const auto &rule : links->rules) {
    std::shared_ptr<boxrig_regex_result> result;
    std::uint64_t match_id = 0;
    if (rule.rule.recognition_source == HyperlinkRecognitionSource::osc8) {
      result = cached_match_osc8(links, rule, hit);
    } else if (rule.recognizer) {
      const auto annotation = std::find_if(annotations.begin(), annotations.begin() + count,
          [&rule](const auto &item) { return item.source_id == rule.recognizer; });
      if (annotation == annotations.begin() + count) continue;
      match_id = annotation->item_id;
      const boxrig_text_range grapheme{hit.leading_position, hit.trailing_position};
      boxrig_text_recognition_result current{};
      std::size_t required = 0;
      if (boxrig_terminal_query_recognition_hits(links->terminal, &grapheme, rule.recognizer,
          &current, 1, &required) != BOXRIG_STATUS_OK || required != 1 || current.match_id != annotation->item_id) continue;
      boxrig_regex_result *captured = nullptr;
      if (boxrig_terminal_text_recognizer_result(links->terminal, current.recognizer_id,
          current.match_id, current.result_set_revision, &captured) != BOXRIG_STATUS_OK) continue;
      result = {captured, boxrig_regex_result_destroy};
    }
    if (!result) continue;
    std::string reason;
    auto action = expand_terminal_hyperlink_action(rule.rule,
        [&result](const std::string &capture) { return regular_expression_capture(result.get(), capture); }, &reason);
    const auto hyperlink_handle = rule.recognizer ? 0 : hit.hyperlink_handle;
    if (action) return TerminalHyperlinkResolution{std::move(action), {}, rule.recognizer, match_id, hyperlink_handle};
    if (!rejection) rejection = TerminalHyperlinkResolution{std::nullopt, "rule " + rule.rule.id + ": " + reason,
        rule.recognizer, match_id, hyperlink_handle};
  }
  return rejection;
}

void destroy_terminal_links(TerminalLinks *links) {
  if (!links) return;
  for (const auto &rule : links->rules) {
    if (rule.recognizer) {
      clear_annotations(links, rule.recognizer);
      boxrig_terminal_unregister_text_recognizer(links->terminal, rule.recognizer);
    }
  }
  delete links;
}

}
