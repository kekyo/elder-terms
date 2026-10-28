// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "../../src/terminal-links.h"

#include <chrono>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace elder_terms;

static void expect(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

static void measure(std::size_t uri_length, bool matching) {
  auto config = boxrig_terminal_config_default();
  config.rows = 3;
  config.columns = 40;
  boxrig_terminal *terminal = nullptr;
  expect(boxrig_terminal_create(&config, &terminal) == BOXRIG_STATUS_OK, "terminal create");
  std::unique_ptr<boxrig_terminal, decltype(&boxrig_terminal_destroy)> terminal_owner(terminal, boxrig_terminal_destroy);

  boxrig_ui_model *model = nullptr;
  expect(boxrig_ui_model_create(terminal, &model) == BOXRIG_UI_OK, "model create");
  std::unique_ptr<boxrig_ui_model, decltype(&boxrig_ui_model_destroy)> model_owner(model, boxrig_ui_model_destroy);

  const auto view_config = boxrig_ui_view_config_default();
  boxrig_ui_view *view = nullptr;
  expect(boxrig_ui_view_create(model, &view_config, &view) == BOXRIG_UI_OK, "view create");
  std::unique_ptr<boxrig_ui_view, decltype(&boxrig_ui_view_destroy)> view_owner(view, boxrig_ui_view_destroy);
  expect(boxrig_ui_view_acquire_layout_authority(view) == BOXRIG_UI_OK, "layout authority");
  boxrig_ui_allocation_input allocation{};
  allocation.allocation_width = 320 * BOXRIG_UI_FIXED_ONE;
  allocation.allocation_height = 30 * BOXRIG_UI_FIXED_ONE;
  allocation.cell_width = 8 * BOXRIG_UI_FIXED_ONE;
  allocation.cell_height = 10 * BOXRIG_UI_FIXED_ONE;
  allocation.cell_metrics_revision = 1;
  allocation.window_state = BOXRIG_UI_WINDOW_NORMAL;
  boxrig_ui_layout_result layout{};
  expect(boxrig_ui_view_accept_allocation(view, &allocation, &layout) == BOXRIG_UI_OK, "view allocation");

  const std::string prefix = matching ? "https://" : "other://";
  const std::string uri = prefix + std::string(uri_length - prefix.size(), 'a');
  const std::string input = "\x1b]8;;" + uri + "\x1b\\link\x1b]8;;\x1b\\";
  std::size_t consumed = 0;
  expect(boxrig_terminal_feed(terminal, reinterpret_cast<const uint8_t *>(input.data()),
      input.size(), &consumed) == BOXRIG_STATUS_OK && consumed == input.size(), "feed OSC 8");
  expect(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK, "synchronize OSC 8");
  boxrig_ui_hit_test_result hit{};
  expect(boxrig_ui_view_hit_test(view, BOXRIG_UI_FIXED_ONE, BOXRIG_UI_FIXED_ONE, &hit) == BOXRIG_UI_OK &&
      hit.text_position_valid && hit.hyperlink_uri_length == uri_length, "retained URI hit");

  auto rules = default_hyperlink_action_rules();
  rules.resize(1);
  std::unique_ptr<TerminalLinks, decltype(&destroy_terminal_links)> links(
      create_terminal_links(view, std::move(rules)), destroy_terminal_links);
  const std::size_t repetitions = uri_length == 8192 ? 200 : 2000;
  for (int trial = 0; trial < 3; ++trial) {
    const auto before = terminal_links_cache_statistics(links.get());
    const auto begin = std::chrono::steady_clock::now();
    for (std::size_t index = 0; index < repetitions; ++index) {
      const auto result = terminal_links_resolve(links.get(), hit);
      expect(static_cast<bool>(result && result->action) == matching, "resolve result");
    }
    const auto end = std::chrono::steady_clock::now();
    const auto after = terminal_links_cache_statistics(links.get());
    const auto total = std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count();
    std::printf("%zu,%s,%d,%zu,%.3f,%llu,%llu,%llu\n", uri_length, matching ? "match" : "miss", trial,
        repetitions, static_cast<double>(total) / repetitions,
        static_cast<unsigned long long>(after.matcher_creations - before.matcher_creations),
        static_cast<unsigned long long>(after.cache_hits - before.cache_hits),
        static_cast<unsigned long long>(after.cache_evictions - before.cache_evictions));
  }
}

int main() {
  try {
    std::puts("uri_bytes,result,trial,repetitions,mean_ns,matcher_creations,cache_hits,cache_evictions");
    for (const auto length : {std::size_t{64}, std::size_t{512}, std::size_t{8192}}) {
      measure(length, true);
      measure(length, false);
    }
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
