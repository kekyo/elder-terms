// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "../../src/terminal-links.h"

#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>

using namespace elder_terms;

static void expect(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

int main() {
  try {
    auto config = boxrig_terminal_config_default();
    config.rows = 3;
    config.columns = 40;
    boxrig_terminal *terminal = nullptr;
    expect(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK, "create terminal");
    std::unique_ptr<boxrig_terminal, decltype(&boxrig_terminal_destroy)> terminal_owner(terminal, boxrig_terminal_destroy);
    boxrig_ui_model *model = nullptr;
    expect(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK, "create model");
    std::unique_ptr<boxrig_ui_model, decltype(&boxrig_ui_model_destroy)> model_owner(model, boxrig_ui_model_destroy);
    const auto view_config = boxrig_ui_view_config_default();
    boxrig_ui_view *view = nullptr;
    expect(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK, "create view");
    std::unique_ptr<boxrig_ui_view, decltype(&boxrig_ui_view_destroy)> view_owner(view, boxrig_ui_view_destroy);
    expect(boxrig_ui_view_acquire_layout_authority(view) == BOXRIG_UI_OK, "acquire layout");
    boxrig_ui_allocation_input allocation{};
    allocation.allocation_width = 320 * BOXRIG_UI_FIXED_ONE;
    allocation.allocation_height = 30 * BOXRIG_UI_FIXED_ONE;
    allocation.cell_width = 8 * BOXRIG_UI_FIXED_ONE;
    allocation.cell_height = 10 * BOXRIG_UI_FIXED_ONE;
    allocation.cell_metrics_revision = 1;
    allocation.window_state = BOXRIG_UI_WINDOW_NORMAL;
    boxrig_ui_layout_result layout{};
    expect(boxrig_ui_view_accept_allocation(view, &allocation, &layout) == BOXRIG_UI_OK, "accept allocation");

    auto rules = default_hyperlink_action_rules();
    rules.resize(1);
    std::unique_ptr<TerminalLinks, decltype(&destroy_terminal_links)> links(
        create_terminal_links(view, rules), destroy_terminal_links);
    const auto feed = [&](const std::string &uri) {
      const auto input = "\x1b[1;1H\x1b[2K\x1b]8;;" + uri + "\x1b\\X\x1b]8;;\x1b\\";
      std::size_t consumed = 0;
      expect(boxrig_terminal_feed(terminal, reinterpret_cast<const uint8_t *>(input.data()),
          input.size(), &consumed) == BOXRIG_STATUS_OK && consumed == input.size(), "feed OSC 8");
      expect(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK, "synchronize OSC 8");
    };
    const auto hit = [&] {
      boxrig_ui_hit_test_result result{};
      expect(boxrig_ui_view_hit_test(view, BOXRIG_UI_FIXED_ONE, BOXRIG_UI_FIXED_ONE, &result) == BOXRIG_UI_OK &&
          result.text_position_valid, "current hit");
      return result;
    };
    const auto resolve = [&](const boxrig_ui_hit_test_result &candidate) {
      return terminal_links_resolve(links.get(), candidate);
    };

    feed("https://a.test/one");
    const auto first_hit = hit();
    expect(resolve(first_hit)->action->arguments == std::vector<std::string>{"https://a.test/one"}, "initial match");
    const auto first_stats = terminal_links_cache_statistics(links.get());
    expect(resolve(first_hit)->action->arguments == std::vector<std::string>{"https://a.test/one"}, "repeated match");
    auto stats = terminal_links_cache_statistics(links.get());
    expect(stats.matcher_creations == first_stats.matcher_creations && stats.cache_hits > first_stats.cache_hits,
        "repeated match avoids matcher creation");

    const std::string unrelated = "\x1b[2;1Hother";
    std::size_t consumed = 0;
    expect(boxrig_terminal_feed(terminal, reinterpret_cast<const uint8_t *>(unrelated.data()),
        unrelated.size(), &consumed) == BOXRIG_STATUS_OK && consumed == unrelated.size(), "feed unrelated text");
    expect(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK, "synchronize unrelated text");
    const auto after_revision = hit();
    expect(resolve(after_revision)->action->arguments == std::vector<std::string>{"https://a.test/one"},
        "unrelated text revision preserves URI cache");
    expect(terminal_links_cache_statistics(links.get()).matcher_creations == stats.matcher_creations,
        "text revision does not clear all URI hits");

    feed("https://a.test/two");
    expect(!resolve(first_hit), "stale borrowed hit cannot use cache");
    const auto second_hit = hit();
    expect(resolve(second_hit)->action->arguments == std::vector<std::string>{"https://a.test/two"},
        "equal length URI bytes remain distinct");
    feed("https://a.test/one");
    expect(resolve(hit())->action->arguments == std::vector<std::string>{"https://a.test/one"},
        "owned URI survives replacement and reuses its result");

    feed("other://a.test/one");
    expect(!resolve(hit()), "initial regex miss");
    stats = terminal_links_cache_statistics(links.get());
    expect(!resolve(hit()), "repeated regex miss");
    expect(terminal_links_cache_statistics(links.get()).matcher_creations == stats.matcher_creations,
        "negative result avoids matcher creation");

    feed("https://oom.test/new");
    terminal_links_fail_next_cache_insert_for_test(links.get());
    stats = terminal_links_cache_statistics(links.get());
    expect(resolve(hit())->action->arguments == std::vector<std::string>{"https://oom.test/new"},
        "cache allocation failure retains matching action");
    const auto after_oom = terminal_links_cache_statistics(links.get());
    expect(after_oom.cache_entries == stats.cache_entries, "allocation failure retains cache bound");
    expect(resolve(hit())->action->arguments == std::vector<std::string>{"https://oom.test/new"},
        "unretained outcome can be resolved again");
    expect(terminal_links_cache_statistics(links.get()).matcher_creations == after_oom.matcher_creations + 1,
        "allocation failure does not turn into a cached miss");

    for (int index = 0; index < 140; ++index) {
      feed("https://capacity.test/" + std::to_string(index));
      expect(resolve(hit())->action.has_value(), "capacity URI resolves");
    }
    stats = terminal_links_cache_statistics(links.get());
    expect(stats.cache_entries <= 128 && stats.cache_uri_bytes <= 256 * 1024 && stats.cache_evictions > 0,
        "cache remains bounded and evicts old URI results");
    for (int index = 0; index < 40; ++index) {
      const auto prefix = "https://long.test/" + std::to_string(index) + "/";
      feed(prefix + std::string(8192 - prefix.size(), 'x'));
      expect(resolve(hit())->action.has_value(), "maximum length URI resolves");
    }
    stats = terminal_links_cache_statistics(links.get());
    expect(stats.cache_entries <= 32 && stats.cache_uri_bytes <= 256 * 1024,
        "owned URI bytes remain bounded at maximum URI length");
    feed("https://a.test/one");
    const auto before_evicted = terminal_links_cache_statistics(links.get());
    expect(resolve(hit())->action.has_value(), "evicted URI resolves afresh");
    expect(terminal_links_cache_statistics(links.get()).matcher_creations == before_evicted.matcher_creations + 1,
        "old URI was evicted");

    auto first_rule = rules.front();
    first_rule.command = "first";
    auto second_rule = first_rule;
    second_rule.id = "second";
    second_rule.command = "second";
    links.reset(create_terminal_links(view, {first_rule, second_rule}));
    expect(terminal_links_cache_statistics(links.get()).cache_entries == 0, "new rule context starts empty");
    expect(resolve(hit())->action->command == "first", "first ordered rule wins");
    links.reset(create_terminal_links(view, {second_rule, first_rule}));
    expect(resolve(hit())->action->command == "second", "rule reorder invalidates old outcomes");
    auto edited_rule = first_rule;
    edited_rule.pattern = R"(^https://different\.test/$)";
    links.reset(create_terminal_links(view, {edited_rule}));
    expect(!resolve(hit()), "rule edit invalidates old positive outcome");
    links.reset(create_terminal_links(view, {}));
    expect(!resolve(hit()), "rule removal invalidates old outcomes");
    links.reset(create_terminal_links(view, {second_rule, first_rule}));
    terminal_links_attach_view(links.get(), nullptr);
    expect(!resolve(hit()), "detached context cannot return a cached action");
    terminal_links_attach_view(links.get(), view);
    expect(resolve(hit())->action->command == "second", "reattached context validates current hit");

    std::puts("OSC 8 cache: reuse, URI ownership, authority, OOM, bounds and rule lifetime passed");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
