// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "../../src/terminal-links.h"

#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string_view>

using namespace elder_terms;

static void expect(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

int main() {
  try {
    auto config = boxrig_terminal_config_default();
    config.rows = 8;
    config.columns = 100;
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
    allocation.allocation_width = 800 * BOXRIG_UI_FIXED_ONE;
    allocation.allocation_height = 80 * BOXRIG_UI_FIXED_ONE;
    allocation.cell_width = 8 * BOXRIG_UI_FIXED_ONE;
    allocation.cell_height = 10 * BOXRIG_UI_FIXED_ONE;
    allocation.cell_metrics_revision = 1;
    allocation.window_state = BOXRIG_UI_WINDOW_NORMAL;
    boxrig_ui_layout_result layout{};
    expect(boxrig_ui_view_accept_allocation(view, &allocation, &layout) == BOXRIG_UI_OK, "allocate view");
    std::unique_ptr<TerminalLinks, decltype(&destroy_terminal_links)> links(
        create_terminal_links(view, default_hyperlink_action_rules()), destroy_terminal_links);
    const auto feed = [&](std::string_view text) {
      std::size_t consumed = 0;
      expect(boxrig_terminal_feed(terminal, reinterpret_cast<const uint8_t *>(text.data()), text.size(), &consumed) == BOXRIG_STATUS_OK && consumed == text.size(), "feed all text");
      expect(boxrig_ui_model_synchronize(model) == BOXRIG_UI_OK, "synchronize text");
      std::size_t dispatches = 0;
      while (terminal_links_dispatch(links.get(), 4096))
        expect(++dispatches < 10000, "recognition completes");
    };
    const auto hit = [&](std::size_t row, std::size_t column) {
      boxrig_ui_hit_test_result result{};
      expect(boxrig_ui_view_hit_test(view, (column * 8 + 1) * BOXRIG_UI_FIXED_ONE,
          (row * 10 + 1) * BOXRIG_UI_FIXED_ONE, &result) == BOXRIG_UI_OK && result.text_position_valid, "current pointer hit");
      return result;
    };

    feed("/foo/bar/baz\r\n\"/foo/bar/baz\"\r\n/tmp\r\n/proc/self/exe\r\nhttps://example.test/tmp\r\n");
    for (const std::size_t row : {0u, 1u}) {
      const auto point = hit(row, 2);
      expect(terminal_links_copy_target(links.get(), point) == "/foo/bar/baz",
          "default rules recognize an absolute path without surrounding quotes");
      const auto resolved = terminal_links_resolve(links.get(), point);
      expect(resolved && !resolved->action && resolved->error.find("does not exist") != std::string::npos,
          "a missing absolute path retains the rejection reason for its error dialog");
      boxrig_ui_text_annotation_style style{};
      std::size_t required = 0;
      boxrig_ui_text_annotation annotations[8]{};
      expect(boxrig_ui_view_query_text_annotation_hits(view, &point, BOXRIG_UI_ANNOTATION_HIT_ALL,
          annotations, 8, &required, &style) == BOXRIG_UI_OK && required > 0 &&
          style.underline_style == BOXRIG_UNDERLINE_SINGLE,
          "missing paths remain underlined");
    }
    expect(!terminal_links_copy_target(links.get(), hit(1, 0)) && !terminal_links_copy_target(links.get(), hit(1, 13)),
        "quotation marks are outside the link");
    for (const auto &[row, path] : {std::pair{2u, "/tmp"}, std::pair{3u, "/proc/self/exe"},
        std::pair{4u, "https://example.test/tmp"}}) {
      const auto point = hit(row, 2);
      const auto resolved = terminal_links_resolve(links.get(), point);
      expect(resolved && resolved->action && resolved->action->command == "xdg-open" &&
          resolved->action->arguments == std::vector<std::string>{path},
          "existing files, directories and HTTP URLs keep their complete xdg-open target");
      expect(terminal_links_copy_target(links.get(), point) == path, "copy the complete target");
    }
    expect(terminal_links_copy_target(links.get(), hit(4, 21)) == "https://example.test/tmp",
        "the HTTP rule wins over an embedded absolute path");
    feed("\x1b[6;1H/tmp\u3000/proc/self/exe");
    expect(terminal_links_copy_target(links.get(), hit(5, 1)) == "/tmp" &&
        terminal_links_copy_target(links.get(), hit(5, 8)) == "/proc/self/exe",
        "Unicode whitespace separates absolute paths");
    std::puts("boxrig local links: absolute paths, validation, quoting and URL priority passed");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
