// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "terminal-links.h"
#include <elder-terms/settings/regular-expression.h>
#include <elder-terms/terminal-color-filter.h>

#include <algorithm>
#include <array>
#include <deque>
#include <glib.h>
#include <list>
#include <map>
#include <new>
#include <set>
#include <string_view>
#include <tuple>
#include <unordered_map>

namespace elder_terms {

using LinkLineKey = std::tuple<boxrig_text_screen, std::uint64_t, std::uint64_t>;

struct PathCheck {
  std::string path;
  std::optional<std::string> error;
  std::map<LinkLineKey, std::size_t> subscribers;
  std::uint64_t generation = 0;
};

struct LinkAnnotation {
  boxrig_ui_text_annotation annotation{};
  std::optional<TerminalHyperlinkAction> candidate;
  std::shared_ptr<PathCheck> path_check;
  std::uint32_t hyperlink_handle = 0;
  std::string recognized_text{};
  std::uint64_t match_id = 0;
};

struct LinkLine {
  boxrig_text_recognizer_event event{};
  std::vector<LinkAnnotation> items;
  std::vector<boxrig_ui_text_annotation> published{};
  std::uint64_t published_revision = 0;
};

struct LinkRule {
  HyperlinkActionRule rule;
  std::shared_ptr<RegularExpressionState> program;
  std::uint64_t recognizer = 0;
  std::uint64_t rescan_revision = 0;
  std::uint64_t generation = 0;
  std::map<LinkLineKey, LinkLine> lines;
  bool url_boundaries = false;
};

struct LinkValidationState {
  TerminalLinks *owner = nullptr;
  TerminalHyperlinkPathValidator validate;
  cardio::cancellation_source stop;
  cardio::cancellation_source activation_stop;
  cardio::primitives::semaphore slots{8};
  std::unordered_map<std::string, std::shared_ptr<PathCheck>> cache;
  std::deque<std::weak_ptr<PathCheck>> queue;
  std::size_t insertions_since_prune = 0;
  std::set<LinkLineKey> dirty;
  guint presentation_source = 0;
  std::uint64_t activation_generation = 0;
  std::size_t queries = 0;
  std::size_t pending = 0;
  std::size_t workers = 0;
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
  std::shared_ptr<LinkValidationState> validation;
  TerminalLinkDecoration decoration = TerminalLinkDecoration::underline;
  bool osc8_dirty = true;
  std::uint64_t next_annotation_id = 1;
#if defined(ELDER_TERMS_CACHE_TESTING)
  bool fail_next_cache_insert = false;
#endif
};

TerminalLinksCacheStatistics terminal_links_cache_statistics(const TerminalLinks *links) {
  auto result = links->cache_statistics;
  result.cache_entries = links->osc8_cache.size();
  result.cache_uri_bytes = links->osc8_cache_bytes;
  result.path_queries = links->validation->queries;
  result.path_queries_pending = links->validation->pending;
  return result;
}

#if defined(ELDER_TERMS_CACHE_TESTING)
void terminal_links_fail_next_cache_insert_for_test(TerminalLinks *links) {
  links->fail_next_cache_insert = true;
}
#endif

static LinkLineKey line_key(boxrig_text_screen screen, boxrig_text_line_id line) {
  return {screen, line.epoch, line.serial};
}

static std::uint64_t annotation_source(const LinkRule &rule) {
  return rule.recognizer ? rule.recognizer : UINT64_MAX - rule.generation;
}

static void unsubscribe_line(const LinkLineKey &key, LinkLine &line) {
  for (auto &item : line.items) {
    if (!item.path_check) continue;
    const auto found = item.path_check->subscribers.find(key);
    if (found != item.path_check->subscribers.end() && --found->second == 0)
      item.path_check->subscribers.erase(found);
  }
}

static void clear_annotations(TerminalLinks *links, std::uint64_t source) {
  for (auto &rule : links->rules) if (annotation_source(rule) == source) {
    for (auto &[key, line] : rule.lines) unsubscribe_line(key, line);
    rule.lines.clear();
  }
  if (links->view) boxrig_ui_view_clear_text_annotations(links->view, source);
}

static void publish_line_annotations(TerminalLinks *links, const LinkLineKey &key);

static void schedule_validation_presentation(const std::shared_ptr<LinkValidationState> &state) {
  if (!state->owner || state->presentation_source || state->dirty.empty()) return;
  state->presentation_source = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE,
      +[](gpointer data) -> gboolean {
        const auto state = *static_cast<std::shared_ptr<LinkValidationState> *>(data);
        state->presentation_source = 0;
        auto dirty = std::move(state->dirty);
        state->dirty.clear();
        if (state->owner) for (const auto &key : dirty) publish_line_annotations(state->owner, key);
        return G_SOURCE_REMOVE;
      }, new std::shared_ptr<LinkValidationState>(state),
      +[](gpointer data) { delete static_cast<std::shared_ptr<LinkValidationState> *>(data); });
}

static cardio::promise<void> check_paths_async(std::shared_ptr<LinkValidationState> state) {
  ++state->workers;
  while (state->owner && !state->queue.empty()) {
    const auto check = state->queue.front().lock();
    state->queue.pop_front();
    if (check) {
      const auto generation = check->generation;
      try {
        const auto cancellation = state->stop.get_cancellation();
        const auto slot = std::move(co_await state->slots.acquire(cancellation));
        cancellation.throw_if_cancellation_requested();
        ++state->queries;
        auto error = co_await state->validate(check->path, cancellation);
        if (state->owner && generation == check->generation) {
          check->error = std::move(error);
          for (const auto &[key, count] : check->subscribers) { (void)count; state->dirty.insert(key); }
          schedule_validation_presentation(state);
        }
      } catch (const cardio::canceled_exception &) {
        // Detached views and terminal destruction retire every pending delivery.
      } catch (const std::exception &error) {
        if (state->owner && generation == check->generation) {
          check->error = error.what();
          for (const auto &[key, count] : check->subscribers) { (void)count; state->dirty.insert(key); }
          schedule_validation_presentation(state);
        }
      }
    }
    --state->pending;
  }
  --state->workers;
}

static std::shared_ptr<PathCheck> path_check(TerminalLinks *links, const std::string &path) {
  auto &state = links->validation;
  if (const auto found = state->cache.find(path); found != state->cache.end()) return found->second;
  // Retained text owns its checks. A weak queue lets replaced text release even
  // unfinished checks, without allocating a suspended coroutine for every path.
  // Prune periodically so a large retained document is not scanned on every match.
  if (++state->insertions_since_prune >= 256) {
    state->insertions_since_prune = 0;
    std::erase_if(state->cache, [](const auto &entry) { return entry.second.use_count() == 1; });
    state->pending -= std::erase_if(state->queue, [](const auto &entry) { return entry.expired(); });
  }
  auto check = std::make_shared<PathCheck>();
  check->path = path;
  state->cache.emplace(path, check);
  state->queue.push_back(check);
  ++state->pending;
  if (state->workers < 8) cardio::fire_and_forget(check_paths_async(state));
  return check;
}

static bool annotation_action_valid(const LinkAnnotation &item) {
  return item.candidate && (!item.candidate->validation_path ||
      (item.path_check && item.path_check->error && item.path_check->error->empty()));
}

static bool annotation_action_rejected(const LinkAnnotation &item) {
  return !item.candidate || (item.path_check && item.path_check->error && !item.path_check->error->empty());
}

static bool same_published_annotation(const boxrig_ui_text_annotation &left,
    const boxrig_ui_text_annotation &right) {
  // All remaining fields are fixed by the owning rule/line or stay zero.
  return left.item_id == right.item_id && left.priority == right.priority &&
      left.range.start.column == right.range.start.column && left.range.end.column == right.range.end.column &&
      left.style.field_mask == right.style.field_mask && left.style.underline_style == right.style.underline_style &&
      left.style.color_tag == right.style.color_tag;
}

static void publish_line_annotations(TerminalLinks *links, const LinkLineKey &key) {
  if (!links->view) return;
  for (auto &rule : links->rules) {
    const auto line = rule.lines.find(key);
    if (line == rule.lines.end()) continue;
    std::vector<boxrig_ui_text_annotation> annotations;
    for (auto &item : line->second.items) {
      bool covered = false;
      for (const auto &other : links->rules) {
        if (&other == &rule) continue;
        const auto previous = other.lines.find(key);
        if (previous == other.lines.end() || previous->second.event.line_revision != line->second.event.line_revision) continue;
        const auto &items = previous->second.items;
        auto candidate = std::upper_bound(items.begin(), items.end(), item.annotation.range.start.column,
            [](std::size_t column, const LinkAnnotation &value) { return column < value.annotation.range.start.column; });
        if (candidate == items.begin()) continue;
        --candidate;
        if (candidate->annotation.range.end.column >= item.annotation.range.end.column &&
            annotation_action_valid(*candidate) &&
            (other.generation < rule.generation || annotation_action_rejected(item))) { covered = true; break; }
      }
      if (covered) continue;
      if (item.candidate && item.candidate->validation_path && !item.path_check) {
        item.path_check = path_check(links, *item.candidate->validation_path);
        ++item.path_check->subscribers[key];
      }
      auto annotation = item.annotation;
      annotation.priority = -static_cast<std::int32_t>(rule.generation);
      annotation.style = {};
      if (links->decoration != TerminalLinkDecoration::none) {
        annotation.style.field_mask = BOXRIG_UI_ANNOTATION_UNDERLINE_STYLE | BOXRIG_UI_ANNOTATION_UNDERLINE_FALLBACK;
        annotation.style.underline_style = BOXRIG_UNDERLINE_SINGLE;
      }
      if (links->decoration == TerminalLinkDecoration::validated) {
        // A valid higher-priority match must also clear dimming contributed
        // by a partially overlapping lower-priority path annotation.
        annotation.style.field_mask |= BOXRIG_UI_ANNOTATION_COLOR_TAG;
        if (rule.rule.path_validation == HyperlinkPathValidation::existing_local_path && !annotation_action_valid(item))
          annotation.style.color_tag = terminal_dim_link_color_tag;
      }
      annotations.push_back(annotation);
    }
    auto &retained = line->second;
    const auto &event = retained.event;
    if (retained.published_revision == event.line_revision &&
        retained.published.size() == annotations.size() &&
        std::equal(retained.published.begin(), retained.published.end(), annotations.begin(), same_published_annotation)) continue;
    // The view rejects a completion after its logical text revision changed.
    if (boxrig_ui_view_replace_text_annotations(links->view, annotation_source(rule), event.screen,
        event.line_id, event.line_revision, annotations.data(), annotations.size()) == BOXRIG_UI_OK) {
      retained.published = std::move(annotations);
      retained.published_revision = event.line_revision;
    }
  }
}

void terminal_links_set_decoration(TerminalLinks *links, TerminalLinkDecoration decoration) {
  if (links->decoration == decoration) return;
  links->decoration = decoration;
  std::set<LinkLineKey> keys;
  for (const auto &rule : links->rules)
    for (const auto &[key, line] : rule.lines) { (void)line; keys.insert(key); }
  for (const auto &key : keys) publish_line_annotations(links, key);
}

static void stop_validation(TerminalLinks *links) {
  const auto state = links->validation;
  state->owner = nullptr;
  state->stop.cancel();
  state->activation_stop.cancel();
  state->pending -= state->queue.size();
  state->queue.clear();
  if (state->presentation_source) {
    g_source_remove(state->presentation_source);
    state->presentation_source = 0;
  }
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

TerminalLinks *create_terminal_links(boxrig_ui_view *view, std::vector<HyperlinkActionRule> rules,
    TerminalHyperlinkPathValidator validate) {
  auto *links = new TerminalLinks;
  links->validation = std::make_shared<LinkValidationState>();
  links->validation->owner = links;
  links->validation->validate = std::move(validate);
  links->terminal = boxrig_ui_view_terminal(view);
  links->view = view;
  const auto limits = boxrig_text_recognizer_limits_default();
  const auto default_url = default_hyperlink_action_rules()[1];
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
    // Boundary policy belongs to the built-in recognition expression, not its
    // editable identifier or launch command. Changed expressions keep their
    // exact captures, even when they reuse the built-in rule identifier.
    const bool url_boundaries = rule.recognition_source == HyperlinkRecognitionSource::terminal_text &&
        !rule.whole_token && rule.pattern == default_url.pattern;
    links->rules.push_back({std::move(rule), std::move(program), recognizer, 0,
        static_cast<std::uint64_t>(links->rules.size() + 1), {}, url_boundaries});
  }
  return links;
}

void terminal_links_attach_view(TerminalLinks *links, boxrig_ui_view *view) {
  stop_validation(links);
  auto validation = std::make_shared<LinkValidationState>();
  validation->owner = links;
  validation->validate = links->validation->validate;
  links->validation = std::move(validation);
  for (const auto &rule : links->rules) {
    clear_annotations(links, annotation_source(rule));
  }
  links->view = view;
  links->osc8_dirty = true;
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

static bool token_boundary(boxrig_terminal *terminal,
    boxrig_text_position position, bool before, std::size_t *remaining = nullptr) {
  if (before && position.column == 0) return true;
  if (remaining && !*remaining) return false;
  std::size_t rows = 0, columns = 0;
  boxrig_terminal_size(terminal, &rows, &columns);
  if (!columns) return false;
  // Read at most one physical fragment. Fragment edges are grapheme boundaries,
  // including font-measured wide characters; soft wraps are not token breaks.
  boxrig_text_range range{position, position};
  if (before) range.start.column = ((position.column - 1) / columns) * columns;
  else range.end.column += columns - position.column % columns;
  const boxrig_text_snapshot_options options{position.screen, &range, remaining ? *remaining : 65536, 1};
  boxrig_text_snapshot *snapshot = nullptr;
  const auto status = boxrig_terminal_create_text_snapshot(terminal, &options, &snapshot);
  if (remaining && status == BOXRIG_STATUS_LIMIT_EXCEEDED) *remaining = 0;
  // Beyond the final physical fragment is a hard line end. Other failures,
  // including oversized projections and stale positions, fail closed.
  if (status != BOXRIG_STATUS_OK)
    return !before && position.column % columns == 0 && status == BOXRIG_STATUS_INVALID_ARGUMENT;
  const std::unique_ptr<boxrig_text_snapshot, decltype(&boxrig_text_snapshot_destroy)> owner(snapshot, boxrig_text_snapshot_destroy);
  std::size_t length = 0;
  const auto *bytes = reinterpret_cast<const char *>(boxrig_text_snapshot_data(snapshot, &length));
  if (remaining) *remaining -= length;
  if (!length) return true;
  const auto character = g_utf8_get_char(before ? g_utf8_find_prev_char(bytes, bytes + length) : bytes);
  return g_unichar_isspace(character) || character == 0x85 || character == 0x180e ||
      character == '\'' || character == '"';
}

static bool url_prefix(std::string_view text, std::size_t offset) {
  const auto rest = text.substr(offset);
  return rest.starts_with("https://") || rest.starts_with("http://");
}

static std::vector<std::pair<std::size_t, std::size_t>> prose_url_ranges(std::string_view text) {
  std::vector<std::pair<std::size_t, std::size_t>> ranges;
  // RFC 3986 permits balanced parentheses and query commas. Prose recognition
  // is intentionally narrower; explicit OSC 8 targets keep every URI byte.
  // Walk the candidate once, including discarded prose between adjacent URLs.
  for (std::size_t offset = 0; offset < text.size();) {
    if (!url_prefix(text, offset)) { ++offset; continue; }
    const auto start = offset;
    offset += text.substr(offset).starts_with("https://") ? 8 : 7;
    const auto authority = offset;
    std::vector<gunichar> closers;
    bool query = false;
    while (offset < text.size()) {
      const auto character = g_utf8_get_char(text.data() + offset);
      if (g_unichar_isspace(character) || character == 0x85 || character == 0x180e ||
          character == '"' || character == '\'' || character == '<' || character == '>' ||
          character == '`' || character == '{' || character == '}' ||
          character == 0x3001 || character == 0x3002 ||
          (character >= 0x3008 && character <= 0x3011) ||
          (character >= 0x2018 && character <= 0x201f) ||
          character == 0xff08 || character == 0xff09 || character == 0xff0c ||
          character == 0xff0e || character == 0xff1b || character == 0xff1a) break;
      if ((character == ',' || character == ';') && url_prefix(text, offset + 1)) break;
      if (character == '(' || character == '[') {
        if (closers.size() == 128) break;
        closers.push_back(character == '(' ? ')' : ']');
      } else if (character == ')' || character == ']') {
        if (closers.empty() || closers.back() != character) break;
        closers.pop_back();
      }
      if (character == '?' || character == '#') query = true;
      offset = static_cast<std::size_t>(g_utf8_next_char(text.data() + offset) - text.data());
    }
    auto end = offset;
    if (!query) while (end > authority && std::string_view(".,;:!").find(text[end - 1]) != std::string_view::npos) --end;
    // Oversized candidates are rejected as a whole, never activated as a prefix.
    if (end > authority && end - start <= 8192) ranges.emplace_back(start, end);
    if (offset < text.size()) ++offset;
  }
  return ranges;
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
  const auto rule = std::find_if(links->rules.begin(), links->rules.end(),
      [&event](const LinkRule &rule) { return rule.recognizer == event.recognizer_id; });
  if (rule == links->rules.end()) return;
  const auto key = line_key(event.screen, event.line_id);
  auto &line = rule->lines[key];
  unsubscribe_line(key, line);
  auto previous = std::move(line.items);
  line.event = event;
  line.items.clear();
  line.items.reserve(results.size());
  std::size_t projection_budget = 65536;
  for (const auto &result : results) {
    if (rule->rule.whole_token &&
        (!token_boundary(links->terminal, result.range.start, true) ||
         !token_boundary(links->terminal, result.range.end, false))) continue;
    boxrig_regex_result *captured = nullptr;
    if (boxrig_terminal_text_recognizer_result(links->terminal, result.recognizer_id,
        result.match_id, result.result_set_revision, &captured) != BOXRIG_STATUS_OK) continue;
    const std::unique_ptr<boxrig_regex_result, decltype(&boxrig_regex_result_destroy)> capture(captured, boxrig_regex_result_destroy);
    auto text = regular_expression_capture(captured, "0").value_or(std::string{});
    std::vector<std::pair<boxrig_text_range, std::string>> matches;
    if (rule->url_boundaries) {
      if (text.size() > projection_budget) continue;
      projection_budget -= text.size();
      const auto ranges = prose_url_ranges(text);
      if (ranges.size() == 1 && ranges[0].first == 0 && ranges[0].second == text.size()) {
        matches.emplace_back(result.range, text);
      } else if (!ranges.empty()) {
        const boxrig_text_snapshot_options options{event.screen, &result.range, text.size(), 1};
        boxrig_text_snapshot *snapshot = nullptr;
        if (boxrig_terminal_create_text_snapshot(links->terminal, &options, &snapshot) != BOXRIG_STATUS_OK) continue;
        const std::unique_ptr<boxrig_text_snapshot, decltype(&boxrig_text_snapshot_destroy)> owner(snapshot, boxrig_text_snapshot_destroy);
        for (const auto &[start, end] : ranges) {
          boxrig_text_range range{};
          if (boxrig_text_snapshot_position_at_byte_offset(snapshot, start, &range.start) == BOXRIG_STATUS_OK &&
              boxrig_text_snapshot_position_at_byte_offset(snapshot, end, &range.end) == BOXRIG_STATUS_OK)
            matches.emplace_back(range, text.substr(start, end - start));
        }
      }
    } else {
      // Very large custom matches continue through the bounded recognizer only.
      if (text.size() > 65536) text.clear();
      matches.emplace_back(result.range, std::move(text));
    }
    for (auto &[range, target] : matches) {
      boxrig_ui_text_annotation annotation{};
      annotation.source_id = result.recognizer_id;
      const auto old = std::lower_bound(previous.begin(), previous.end(), range.start.column,
          [](const LinkAnnotation &item, std::size_t column) { return item.annotation.range.start.column < column; });
      annotation.item_id = old != previous.end() && old->annotation.range.start.column == range.start.column &&
          old->annotation.range.end.column == range.end.column ? old->annotation.item_id : links->next_annotation_id++;
      annotation.range = range;
      std::string reason;
      auto candidate = expand_terminal_hyperlink_action(rule->rule,
          [&](const std::string &name) { return rule->url_boundaries && name == "0" ?
              std::optional{target} : regular_expression_capture(captured, name); }, &reason);
      line.items.push_back({annotation, std::move(candidate), {}, 0, std::move(target), result.match_id});
    }
  }
  publish_line_annotations(links, key);
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
      const auto key = line_key(event.screen, event.line_id);
      for (auto &rule : links->rules) if (rule.recognizer == event.recognizer_id) {
        if (const auto found = rule.lines.find(key); found != rule.lines.end()) {
          unsubscribe_line(key, found->second);
          rule.lines.erase(found);
        }
      }
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

static bool retained_text_unchanged(TerminalLinks *links, const LinkRule &rule,
    LinkAnnotation &item, const boxrig_text_position &position, std::size_t &remaining) {
  if (item.recognized_text.empty() || item.recognized_text.size() > remaining) return false;
  auto range = item.annotation.range;
  range.start.line_revision = range.end.line_revision = position.line_revision;
  range.start.text_revision = range.end.text_revision = position.text_revision;
  const boxrig_text_snapshot_options options{position.screen, &range, remaining, 1};
  boxrig_text_snapshot *snapshot = nullptr;
  const auto status = boxrig_terminal_create_text_snapshot(links->terminal, &options, &snapshot);
  if (status == BOXRIG_STATUS_LIMIT_EXCEEDED) remaining = 0;
  if (status != BOXRIG_STATUS_OK) return false;
  const std::unique_ptr<boxrig_text_snapshot, decltype(&boxrig_text_snapshot_destroy)> owner(snapshot, boxrig_text_snapshot_destroy);
  std::size_t length = 0;
  const auto *bytes = reinterpret_cast<const char *>(boxrig_text_snapshot_data(snapshot, &length));
  remaining -= length;
  if (std::string_view(bytes, length) != item.recognized_text ||
      (rule.rule.whole_token && (!token_boundary(links->terminal, range.start, true, &remaining) ||
          !token_boundary(links->terminal, range.end, false, &remaining)))) return false;
  item.annotation.range = range;
  return true;
}

static void refresh_visible_text_annotations(TerminalLinks *links, std::size_t rows) {
  // Bound the extra projection work independently of history and rule count.
  // Matches beyond this limit still finish through the incremental recognizer.
  std::size_t remaining = 65536;
  for (std::size_t row = 0; row < rows; ++row) {
    boxrig_viewport_cell_resolution position{};
    if (boxrig_terminal_resolve_viewport_cell(links->terminal, row, 0, &position) != BOXRIG_STATUS_OK) continue;
    const auto key = line_key(position.fragment.screen, position.fragment.line_id);
    bool changed = false;
    for (auto &rule : links->rules) {
      if (!rule.recognizer) continue;
      const auto found = rule.lines.find(key);
      if (found == rule.lines.end() || found->second.event.line_revision == position.fragment.line_revision) continue;
      auto &line = found->second;
      // Retain only the appearance of byte-identical text, with current token
      // boundaries. Activation still requires fresh recognizer captures below.
      std::erase_if(line.items, [&](LinkAnnotation &item) {
        if (retained_text_unchanged(links, rule, item, position.range.start, remaining)) return false;
        if (item.path_check) {
          auto subscriber = item.path_check->subscribers.find(key);
          if (subscriber != item.path_check->subscribers.end() && --subscriber->second == 0)
            item.path_check->subscribers.erase(subscriber);
        }
        return true;
      });
      line.event.line_revision = position.fragment.line_revision;
      changed = true;
    }
    if (changed) publish_line_annotations(links, key);
  }
}

void terminal_links_refresh_viewport(TerminalLinks *links) {
  if (!links->view) return;
  boxrig_ui_dirty dirty{};
  if (!links->osc8_dirty && !boxrig_ui_view_peek_dirty(links->view, BOXRIG_UI_DIRTY_MODEL, &dirty)) return;
  links->osc8_dirty = false;
  std::vector<std::map<LinkLineKey, LinkLine>> visible(links->rules.size());
  std::size_t rows = 0, columns = 0;
  boxrig_terminal_size(links->terminal, &rows, &columns);
  refresh_visible_text_annotations(links, rows);
  for (std::size_t row = 0; row < rows; ++row) {
    for (std::size_t column = 0; column < columns; ++column) {
      boxrig_cell_view cell{};
      if (boxrig_terminal_get_cell(links->terminal, row, column, &cell) != BOXRIG_STATUS_OK ||
          cell.continuation || !cell.hyperlink_handle) continue;
      boxrig_viewport_cell_resolution position{};
      if (boxrig_terminal_resolve_viewport_cell(links->terminal, row, column, &position) != BOXRIG_STATUS_OK) continue;
      const auto key = line_key(position.fragment.screen, position.fragment.line_id);
      for (std::size_t index = 0; index < links->rules.size(); ++index) {
        const auto &rule = links->rules[index];
        if (rule.recognizer) continue;
        auto &line = visible[index][key];
        if (!line.items.empty() && line.items.back().hyperlink_handle == cell.hyperlink_handle &&
            line.items.back().annotation.range.end.column == position.range.start.column) {
          line.items.back().annotation.range.end = position.range.end;
          continue;
        }
        boxrig_ui_hit_test_result target{};
        target.hyperlink_uri = cell.hyperlink_uri;
        target.hyperlink_uri_length = cell.hyperlink_uri_length;
        const auto match = cached_match_osc8(links, rule, target);
        if (!match) continue;
        std::string reason;
        auto action = expand_terminal_hyperlink_action(rule.rule,
            [&match](const std::string &name) { return regular_expression_capture(match.get(), name); }, &reason);
        line.event.screen = position.fragment.screen;
        line.event.line_id = position.fragment.line_id;
        line.event.line_revision = position.fragment.line_revision;
        boxrig_ui_text_annotation annotation{};
        annotation.source_id = annotation_source(rule);
        // Source/item identities are unique across the view, not just one line.
        annotation.item_id = links->next_annotation_id++;
        annotation.range = position.range;
        line.items.push_back({annotation, std::move(action), {}, cell.hyperlink_handle});
      }
    }
  }
  std::set<LinkLineKey> changed;
  for (std::size_t index = 0; index < links->rules.size(); ++index) {
    auto &rule = links->rules[index];
    if (rule.recognizer) continue;
    auto &current = visible[index];
    for (auto found = rule.lines.begin(); found != rule.lines.end();) {
      if (!current.contains(found->first) || current.at(found->first).items.empty()) {
        unsubscribe_line(found->first, found->second);
        boxrig_ui_view_clear_text_annotation_line(links->view, annotation_source(rule),
            found->second.event.screen, found->second.event.line_id);
        changed.insert(found->first);
        found = rule.lines.erase(found);
      } else ++found;
    }
    for (auto &[key, line] : current) {
      if (line.items.empty()) continue;
      auto &previous = rule.lines[key];
      const bool same = previous.event.line_revision == line.event.line_revision &&
          previous.items.size() == line.items.size() &&
          std::equal(previous.items.begin(), previous.items.end(), line.items.begin(),
              [](const LinkAnnotation &left, const LinkAnnotation &right) {
                return left.hyperlink_handle == right.hyperlink_handle && left.candidate == right.candidate &&
                    left.annotation.range.start.column == right.annotation.range.start.column &&
                    left.annotation.range.end.column == right.annotation.range.end.column;
              });
      if (same) continue;
      unsubscribe_line(key, previous);
      previous = std::move(line);
      changed.insert(key);
    }
  }
  for (const auto &key : changed) publish_line_annotations(links, key);
}

struct ResolvedLink {
  TerminalHyperlinkResolution resolution;
  std::shared_ptr<boxrig_regex_result> match;
  std::optional<std::string> normalized_target;
};

static std::optional<ResolvedLink> resolve_link(
    TerminalLinks *links, const boxrig_ui_hit_test_result &hit) {
  if (!links->view || !hit.inside_grid || !hit.text_position_valid ||
      (hit.overlay_roles & (BOXRIG_UI_OVERLAY_IME | BOXRIG_UI_OVERLAY_CANDIDATES))) return std::nullopt;
  // This also validates layout, text and annotation authority before reading a
  // borrowed OSC 8 pointer from the hit. Never dereference stale URI storage.
  std::array<boxrig_ui_text_annotation, 32> annotations{};
  std::size_t count = 0;
  if (boxrig_ui_view_query_text_annotation_hits(links->view, &hit, BOXRIG_UI_ANNOTATION_HIT_ALL,
      annotations.data(), annotations.size(), &count, nullptr) != BOXRIG_UI_OK) return std::nullopt;
  std::optional<ResolvedLink> rejection;
  for (const auto &rule : links->rules) {
    std::shared_ptr<boxrig_regex_result> result;
    std::optional<std::string> normalized_target;
    std::uint64_t match_id = 0;
    if (rule.rule.recognition_source == HyperlinkRecognitionSource::osc8) {
      result = cached_match_osc8(links, rule, hit);
    } else if (rule.recognizer) {
      const auto annotation = std::find_if(annotations.begin(), annotations.begin() + count,
          [&rule](const auto &item) { return item.source_id == rule.recognizer; });
      if (annotation == annotations.begin() + count) continue;
      const boxrig_text_range grapheme{hit.leading_position, hit.trailing_position};
      boxrig_text_recognition_result current{};
      std::size_t required = 0;
      if (boxrig_terminal_query_recognition_hits(links->terminal, &grapheme, rule.recognizer,
          &current, 1, &required) != BOXRIG_STATUS_OK || required != 1 ||
          current.range.start.column > annotation->range.start.column ||
          current.range.end.column < annotation->range.end.column) continue;
      const auto line = rule.lines.find(line_key(hit.position.screen, hit.position.line_id));
      if (line == rule.lines.end()) continue;
      const auto retained = std::find_if(line->second.items.begin(), line->second.items.end(),
          [&](const LinkAnnotation &item) { return item.annotation.item_id == annotation->item_id; });
      if (retained == line->second.items.end() || retained->match_id != current.match_id) continue;
      if (rule.url_boundaries) normalized_target = retained->recognized_text;
      match_id = current.match_id;
      boxrig_regex_result *captured = nullptr;
      if (boxrig_terminal_text_recognizer_result(links->terminal, current.recognizer_id,
          current.match_id, current.result_set_revision, &captured) != BOXRIG_STATUS_OK) continue;
      result = {captured, boxrig_regex_result_destroy};
    }
    if (!result) continue;
    std::string reason;
    auto action = expand_terminal_hyperlink_action(rule.rule,
        [&](const std::string &capture) { return normalized_target && capture == "0" ?
            normalized_target : regular_expression_capture(result.get(), capture); }, &reason);
    const auto hyperlink_handle = rule.recognizer ? 0 : hit.hyperlink_handle;
    const auto candidate = action;
    if (action && action->validation_path) {
      auto check = path_check(links, *action->validation_path);
      if (!check->error || !check->error->empty()) {
        reason = check->error.value_or(std::string{});
        action.reset();
      }
    }
    ResolvedLink resolved{{std::move(action), reason.empty() ? std::string{} : "rule " + rule.rule.id + ": " + reason,
        rule.recognizer, match_id, hyperlink_handle, candidate}, std::move(result), std::move(normalized_target)};
    // Pending validation must retain priority until its outcome is known.
    if (resolved.resolution.action || (candidate && reason.empty())) return resolved;
    if (!rejection) rejection = std::move(resolved);
  }
  return rejection;
}

std::optional<TerminalHyperlinkResolution> terminal_links_resolve(
    TerminalLinks *links, const boxrig_ui_hit_test_result &hit) {
  auto resolved = resolve_link(links, hit);
  return resolved ? std::optional{std::move(resolved->resolution)} : std::nullopt;
}

static cardio::promise<void> activate_path_async(std::shared_ptr<LinkValidationState> state,
    TerminalHyperlinkResolution resolution, std::function<void(TerminalHyperlinkResolution)> activate,
    std::uint64_t generation, cardio::cancellation cancellation) {
  ++state->pending;
  try {
    const auto slot = std::move(co_await state->slots.acquire(cancellation));
    cancellation.throw_if_cancellation_requested();
    ++state->queries;
    const auto path = *resolution.candidate->validation_path;
    auto error = co_await state->validate(path, cancellation);
    if (state->owner && generation == state->activation_generation && !cancellation.is_cancellation_requested()) {
      if (const auto found = state->cache.find(path); found != state->cache.end()) {
        ++found->second->generation;
        found->second->error = error;
        for (const auto &[key, count] : found->second->subscribers) { (void)count; state->dirty.insert(key); }
        schedule_validation_presentation(state);
      }
      resolution.action = error.empty() ? resolution.candidate : std::nullopt;
      resolution.error = std::move(error);
      activate(std::move(resolution));
    }
  } catch (const cardio::canceled_exception &) {
    // A newer click, detached view or destroyed terminal owns subsequent UI.
  } catch (const std::exception &error) {
    if (state->owner && generation == state->activation_generation) {
      resolution.action.reset();
      resolution.error = error.what();
      activate(std::move(resolution));
    }
  }
  --state->pending;
}

void terminal_links_activate(TerminalLinks *links, TerminalHyperlinkResolution resolution,
    std::function<void(TerminalHyperlinkResolution)> activate) {
  if (!links || !links->view) return;
  if (!resolution.candidate || !resolution.candidate->validation_path) {
    activate(std::move(resolution));
    return;
  }
  auto state = links->validation;
  state->activation_stop.cancel();
  state->activation_stop = cardio::cancellation_source{};
  cardio::fire_and_forget(activate_path_async(state, std::move(resolution), std::move(activate),
      ++state->activation_generation, state->activation_stop.get_cancellation()));
}

std::optional<std::string> terminal_links_copy_target(
    TerminalLinks *links, const boxrig_ui_hit_test_result &hit) {
  const auto resolved = resolve_link(links, hit);
  if (!resolved) return std::nullopt;
  // Group zero owns the full text match or OSC 8 URI. Extract it only for
  // copying, rather than allocating another string on every hover query.
  auto target = resolved->normalized_target ? resolved->normalized_target : regular_expression_capture(resolved->match.get(), "0");
  if (!target || target->empty() || target->find('\0') != std::string::npos) return std::nullopt;
  return target;
}

void destroy_terminal_links(TerminalLinks *links) {
  if (!links) return;
  stop_validation(links);
  for (const auto &rule : links->rules) {
    clear_annotations(links, annotation_source(rule));
    if (rule.recognizer) {
      boxrig_terminal_unregister_text_recognizer(links->terminal, rule.recognizer);
    }
  }
  delete links;
}

}
