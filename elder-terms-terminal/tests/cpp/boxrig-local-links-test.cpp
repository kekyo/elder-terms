// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "../../src/terminal-links.h"

#include <cstdio>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string_view>

using namespace elder_terms;

static void expect(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

int main() {
  try {
    cardio::dispatcher_group_glib group;
    cardio::dispatcher_host_glib_auto dispatcher(group);
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
    const auto wait_checks = [&] {
      while (terminal_links_cache_statistics(links.get()).path_queries_pending)
        g_main_context_iteration(nullptr, TRUE);
      while (g_main_context_pending(nullptr)) g_main_context_iteration(nullptr, FALSE);
    };
    const auto link_style = [&](std::size_t row) {
      const auto point = hit(row, 2);
      boxrig_ui_text_annotation_style style{};
      boxrig_ui_text_annotation annotations[8]{};
      std::size_t required = 0;
      expect(boxrig_ui_view_query_text_annotation_hits(view, &point, BOXRIG_UI_ANNOTATION_HIT_ALL,
          annotations, 8, &required, &style) == BOXRIG_UI_OK && required > 0,
          "read current link appearance");
      return style;
    };

    feed("/foo/bar/baz\r\n\"/foo/bar/baz\"\r\n/tmp\r\n/proc/self/exe\r\nhttps://example.test/tmp\r\n");
    wait_checks();
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
      boxrig_rgb foreground{};
      expect(boxrig_terminal_get_dynamic_color(terminal, BOXRIG_DYNAMIC_COLOR_FOREGROUND, &foreground) == BOXRIG_STATUS_OK,
          "read normal link foreground");
      expect((style.field_mask & BOXRIG_UI_ANNOTATION_FOREGROUND) != 0 &&
          style.foreground.kind == BOXRIG_COLOR_RGB && style.foreground.red < foreground.red &&
          style.foreground.green < foreground.green && style.foreground.blue < foreground.blue,
          "invalid paths have lower brightness while retaining their underline");
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
    wait_checks();
    expect(terminal_links_copy_target(links.get(), hit(5, 1)) == "/tmp" &&
        terminal_links_copy_target(links.get(), hit(5, 8)) == "/proc/self/exe",
        "Unicode whitespace separates absolute paths");
    feed("\x1b[2J\x1b[H~/\r\n\"~/Downloads/\"\r\nhttps://example.test/~/Downloads/");
    wait_checks();
    expect(terminal_links_copy_target(links.get(), hit(0, 0)) == "~/" &&
        terminal_links_copy_target(links.get(), hit(1, 1)) == "~/Downloads/",
        "home-relative links include the tilde and exclude surrounding quotes");
    expect(!terminal_links_copy_target(links.get(), hit(1, 0)) &&
        !terminal_links_copy_target(links.get(), hit(1, 13)), "home link quotes are outside the target");
    const auto home = terminal_links_resolve(links.get(), hit(0, 0));
    expect(home && home->action && std::filesystem::path(home->action->arguments[0]).is_absolute(),
        "a home-relative directory resolves to an existing absolute local path");
    expect(terminal_links_copy_target(links.get(), hit(2, 22)) == "https://example.test/~/Downloads/" &&
        terminal_links_resolve(links.get(), hit(2, 22))->action->arguments ==
            std::vector<std::string>{"https://example.test/~/Downloads/"},
        "a tilde inside an HTTP URL remains part of the unmodified URL");

    links.reset();
    std::vector<std::shared_ptr<cardio::promise_source<std::string>>> requests;
    std::vector<std::string> requested_paths;
    links.reset(create_terminal_links(view, default_hyperlink_action_rules(),
        [&requests, &requested_paths](std::string path, cardio::cancellation) {
          auto request = std::make_shared<cardio::promise_source<std::string>>();
          requests.push_back(request);
          requested_paths.push_back(std::move(path));
          return request->get_promise();
        }));
    feed("\x1b[2J\x1b[H/foo/bar/baz\r\n/foo/bar/baz\r\nhttps://example.test/");
    while (g_main_context_pending(nullptr)) g_main_context_iteration(nullptr, FALSE);
    expect(requested_paths == std::vector<std::string>{"/foo/bar/baz"},
        "duplicate paths share one pending asynchronous check");
    const auto pending = terminal_links_resolve(links.get(), hit(0, 2));
    expect(pending && pending->candidate && !pending->action && pending->error.empty(),
        "pending validation returns immediately with an owned clickable candidate");
    const auto pending_style = link_style(1);
    expect(pending_style.underline_style == BOXRIG_UNDERLINE_SINGLE &&
        (pending_style.field_mask & BOXRIG_UI_ANNOTATION_FOREGROUND) != 0 &&
        (pending_style.field_mask & BOXRIG_UI_ANNOTATION_UNDERLINE_COLOR) != 0 &&
        pending_style.foreground.kind == BOXRIG_COLOR_RGB && pending_style.foreground.red < 128,
        "a newly recognized path is dimmed and underlined while validation is pending");
    expect((link_style(2).field_mask & BOXRIG_UI_ANNOTATION_FOREGROUND) == 0,
        "URLs without path validation immediately retain their normal brightness");
    for (int index = 0; index < 10; ++index)
      expect(terminal_links_copy_target(links.get(), hit(0, 2)) == "/foo/bar/baz", "pending links remain copyable");
    expect(requests.size() == 1, "pointer movement and copying reuse pending validation");
    bool superseded_activated = false;
    terminal_links_activate(links.get(), *pending, [&](auto) { superseded_activated = true; });
    while (g_main_context_pending(nullptr)) g_main_context_iteration(nullptr, FALSE);
    expect(!superseded_activated && requests.size() == 2, "click validation waits asynchronously");
    std::optional<TerminalHyperlinkResolution> activated;
    terminal_links_activate(links.get(), *pending, [&](auto result) { activated = std::move(result); });
    while (g_main_context_pending(nullptr)) g_main_context_iteration(nullptr, FALSE);
    expect(!activated && requests.size() == 3, "a newer click can supersede a pending check");
    bool url_activated = false;
    terminal_links_activate(links.get(), *terminal_links_resolve(links.get(), hit(2, 2)),
        [&](auto result) { url_activated = result.action.has_value(); });
    expect(url_activated, "an unrelated URL activates while path checks remain pending");
    feed("\x1b[1;1H\x1b[2Kreplacement");
    expect(!terminal_links_resolve(links.get(), hit(0, 2)), "new terminal input replaces a pending link immediately");
    requests[2]->resolve(std::string{});
    while (g_main_context_pending(nullptr)) g_main_context_iteration(nullptr, FALSE);
    expect(activated && activated->action, "the newest click delivers its fresh validation");
    expect((link_style(1).field_mask & BOXRIG_UI_ANNOTATION_FOREGROUND) == 0 &&
        (link_style(1).field_mask & BOXRIG_UI_ANNOTATION_UNDERLINE_COLOR) == 0 &&
        link_style(1).underline_style == BOXRIG_UNDERLINE_SINGLE,
        "successful validation restores normal text and underline brightness");
    requests[0]->resolve("expanded path does not exist: /foo/bar/baz");
    requests[1]->resolve("expanded path does not exist: /foo/bar/baz");
    wait_checks();
    expect(!superseded_activated && terminal_links_resolve(links.get(), hit(1, 2))->action,
        "older background and click results cannot overwrite the newest validation");
    activated.reset();
    terminal_links_activate(links.get(), *terminal_links_resolve(links.get(), hit(1, 2)),
        [&](auto result) { activated = std::move(result); });
    while (g_main_context_pending(nullptr)) g_main_context_iteration(nullptr, FALSE);
    requests.back()->resolve("expanded path does not exist: /foo/bar/baz");
    wait_checks();
    expect(activated && !activated->action && activated->error.find("does not exist") != std::string::npos,
        "the completed click delivers the path error for its modal dialog");
    expect(link_style(1).foreground.red == pending_style.foreground.red &&
        link_style(1).underline_color.red == pending_style.underline_color.red,
        "rejected paths keep the same dimmed appearance as pending paths");
    expect(!terminal_links_resolve(links.get(), hit(0, 2)), "late validation does not restore replaced text");

    requests.clear();
    std::string many_paths = "\x1b[2J\x1b[H";
    for (int index = 0; index < 20; ++index) many_paths += "/pending/" + std::to_string(index) + " ";
    feed(many_paths);
    while (g_main_context_pending(nullptr)) g_main_context_iteration(nullptr, FALSE);
    expect(requests.size() == 8 && terminal_links_cache_statistics(links.get()).path_queries_pending == 20,
        "at most eight filesystem queries run while the rest wait without blocking");
    bool retired_activated = false;
    terminal_links_activate(links.get(), *terminal_links_resolve(links.get(), hit(0, 2)),
        [&](auto) { retired_activated = true; });
    links.reset();
    for (auto &request : requests) request->resolve(std::string{});
    while (g_main_context_pending(nullptr)) g_main_context_iteration(nullptr, FALSE);
    expect(requests.size() == 8 && !retired_activated,
        "destruction cancels queued checks and discards late activation callbacks");

    requests.clear();
    auto preferred = default_hyperlink_action_rules().back();
    preferred.command = "preferred";
    auto fallback = preferred;
    fallback.id = "fallback";
    fallback.command = "fallback";
    fallback.path_validation = HyperlinkPathValidation::none;
    links.reset(create_terminal_links(view, {preferred, fallback},
        [&](std::string, cardio::cancellation) {
          auto request = std::make_shared<cardio::promise_source<std::string>>();
          requests.push_back(request);
          return request->get_promise();
        }));
    feed("\x1b[2J\x1b[H/priority/path");
    while (g_main_context_pending(nullptr)) g_main_context_iteration(nullptr, FALSE);
    const auto preferred_pending = terminal_links_resolve(links.get(), hit(0, 2));
    expect(preferred_pending && !preferred_pending->action && preferred_pending->candidate &&
        preferred_pending->candidate->command == "preferred",
        "pending validation preserves rule priority instead of running a lower-priority command");
    requests.front()->resolve("expanded path does not exist: /priority/path");
    wait_checks();
    expect(terminal_links_resolve(links.get(), hit(0, 2))->action->command == "fallback",
        "a definite validation rejection still permits the next valid rule");

    links.reset();
    requests.clear();
    auto unchecked_rules = default_hyperlink_action_rules();
    unchecked_rules.back().path_validation = HyperlinkPathValidation::none;
    links.reset(create_terminal_links(view, std::move(unchecked_rules),
        [&](std::string, cardio::cancellation) -> cardio::promise<std::string> {
          throw std::runtime_error("disabled path validation must not query the filesystem");
          co_return std::string{};
        }));
    feed("\x1b[2J\x1b[H/foo/bar/baz");
    activated.reset();
    terminal_links_activate(links.get(), *terminal_links_resolve(links.get(), hit(0, 2)),
        [&](auto result) { activated = std::move(result); });
    expect(activated && activated->action && terminal_links_cache_statistics(links.get()).path_queries_pending == 0,
        "disabling validation per rule preserves immediate activation without filesystem I/O");

    links.reset();
    expect(boxrig_terminal_set_scrollback_capacity(terminal, 0) == BOXRIG_STATUS_OK, "limit retained text");
    links.reset(create_terminal_links(view, default_hyperlink_action_rules(),
        [&](std::string, cardio::cancellation) {
          auto request = std::make_shared<cardio::promise_source<std::string>>();
          requests.push_back(request);
          return request->get_promise();
        }));
    for (int index = 0; index < 600; ++index) {
      feed("\x1b[2J\x1b[H/stalled/" + std::to_string(index));
      while (g_main_context_pending(nullptr)) g_main_context_iteration(nullptr, FALSE);
    }
    expect(requests.size() == 8 && terminal_links_cache_statistics(links.get()).path_queries_pending <= 264,
        "replaced text cannot accumulate unbounded waiting filesystem checks");
    links.reset();
    for (auto &request : requests) request->resolve(std::string{});
    while (g_main_context_pending(nullptr)) g_main_context_iteration(nullptr, FALSE);
    std::puts("boxrig local links: absolute paths, validation, quoting and URL priority passed");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
