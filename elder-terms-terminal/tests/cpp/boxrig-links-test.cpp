// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "../../src/terminal-links.h"
#include <elder-terms/settings/regular-expression.h>

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
    cardio::dispatcher_group_glib group;
    cardio::dispatcher_host_glib_auto dispatcher(group);
    auto config = boxrig_terminal_config_default();
    config.rows = 8;
    config.columns = 100;
    config.text_recognizer_event_capacity = 2;
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
    };
    std::size_t scan_number = 0;
    const auto scan = [&] {
      ++scan_number;
      std::size_t dispatches = 0;
      while (terminal_links_dispatch(links.get(), 1)) {
        if (++dispatches >= 500000) throw std::runtime_error("finite recognition, scan " + std::to_string(scan_number));
      }
    };
    const auto hit = [&](std::size_t row, std::size_t column) {
      boxrig_ui_hit_test_result result{};
      expect(boxrig_ui_view_hit_test(view, (column * 8 + 1) * BOXRIG_UI_FIXED_ONE,
          (row * 10 + 1) * BOXRIG_UI_FIXED_ONE, &result) == BOXRIG_UI_OK && result.text_position_valid, "current pointer hit");
      return result;
    };
    const auto action = [&](std::size_t row, std::size_t column) {
      const auto resolved = terminal_links_resolve(links.get(), hit(row, column));
      if (!resolved || !resolved->action) throw std::runtime_error("resolve current link at row " +
          std::to_string(row) + ", column " + std::to_string(column) + ", scan " + std::to_string(scan_number));
      return *resolved->action;
    };

    const std::string pipewire_docs = "https://docs.pipewire.org/page_module_combine_stream.html";
    const std::string pipewire_code = "https://github.com/PipeWire/pipewire/blob/1.0.5/src/modules/module-combine-stream.c";
    const std::vector<std::pair<std::string, std::vector<std::string>>> prose_cases = {
        {"参照: https://host/a、詳細は後述", {"https://host/a"}},
        {"url=\"https://host/a\" status=200", {"https://host/a"}},
        {"(https://host/a) [label](https://host/b)", {"https://host/a", "https://host/b"}},
        {"https://host/a_(b) https://host/a_(b_(c))", {"https://host/a_(b)", "https://host/a_(b_(c))"}},
        {"https://host/a)、次の文章", {"https://host/a"}},
        {"https://[2001:db8::1]/a", {"https://[2001:db8::1]/a"}},
        {"https://host/a%28b%29?q=a,b&x=%5D#part", {"https://host/a%28b%29?q=a,b&x=%5D#part"}},
        {"https://host/a,https://host/b;https://host/c.", {"https://host/a", "https://host/b", "https://host/c"}},
        {"https://host/a?q=a,b,", {"https://host/a?q=a,b,"}},
        {"https://例え.test/日本語_(説明)。後続", {"https://例え.test/日本語_(説明)"}},
        {"\"[Combine Streamの仕様](" + pipewire_docs + ")、[PipeWire 1.0.5のcombine-stream実装](" + pipewire_code + ")\"",
            {pipewire_docs, pipewire_code}},
        {std::string(90, ' ') + "https://host/" + std::string(120, 'a') + ") end",
            {"https://host/" + std::string(120, 'a')}},
    };
    for (const auto &[input, targets] : prose_cases) {
      feed("\x1b[2J\x1b[H" + input);
      scan();
      const boxrig_text_snapshot_options options{BOXRIG_TEXT_SCREEN_PRIMARY, nullptr, 8192, 8};
      boxrig_text_snapshot *snapshot = nullptr;
      expect(boxrig_terminal_create_text_snapshot(terminal, &options, &snapshot) == BOXRIG_STATUS_OK, "snapshot prose");
      const std::unique_ptr<boxrig_text_snapshot, decltype(&boxrig_text_snapshot_destroy)> owner(snapshot, boxrig_text_snapshot_destroy);
      std::size_t length = 0;
      const auto *bytes = reinterpret_cast<const char *>(boxrig_text_snapshot_data(snapshot, &length));
      const std::string_view displayed(bytes, length);
      std::vector<std::pair<std::size_t, std::string>> expected;
      std::size_t offset = 0;
      for (const auto &target : targets) {
        offset = displayed.find(target, offset);
        expect(offset != std::string::npos, "fixture target exists");
        expected.emplace_back(offset, target);
        offset += target.size();
      }
      for (std::size_t row = 0; row < 3; ++row) for (std::size_t column = 0; column < 100; ++column) {
        const auto current = hit(row, column);
        std::size_t byte = 0;
        const bool projected = boxrig_text_snapshot_byte_offset_at_position(snapshot, &current.leading_position, &byte) == BOXRIG_STATUS_OK;
        std::optional<std::string> target;
        for (const auto &[start, value] : expected)
          if (projected && byte >= start && byte < start + value.size()) target = value;
        const auto copied = terminal_links_copy_target(links.get(), current);
        if (copied != target) throw std::runtime_error("prose target at " + std::to_string(byte) + ": expected " +
            target.value_or("<none>") + ", got " + copied.value_or("<none>") + "; " + input);
        const auto resolved = terminal_links_resolve(links.get(), current);
        expect(target ? resolved && resolved->action && resolved->action->arguments == std::vector<std::string>{*target} : !resolved,
            "decoration, launch and copy share the exact prose URL range");
      }
    }
    feed("\x1b[2J\x1b[H");

    feed("https://host/a\r\nb\r\n\x1b]8;;https://host/a)、https://host/b\x1b\\explicit\x1b]8;;\x1b\\");
    scan();
    expect(action(0, 2).arguments == std::vector<std::string>{"https://host/a"}, "hard newline ends a URL");
    expect(!terminal_links_copy_target(links.get(), hit(1, 0)), "hard newline continuation is not a link");
    expect(action(2, 2).arguments == std::vector<std::string>{"https://host/a)、https://host/b"} &&
        terminal_links_copy_target(links.get(), hit(2, 2)) == "https://host/a)、https://host/b", "OSC 8 retains its complete explicit URI");
    feed("\x1b[2J\x1b[H");

    feed("前 https://example.みんな/　https://second.test/\r\n");
    expect(!terminal_links_resolve(links.get(), hit(0, 4)), "unscanned text is not a complete link");
    scan();
    expect(action(0, 4).arguments == std::vector<std::string>{"https://example.みんな/"}, "Unicode space ends the first screen URL");
    expect(terminal_links_copy_target(links.get(), hit(0, 4)) == "https://example.みんな/",
        "copy the complete recognized UTF-8 URL");
    expect(action(0, 32).arguments == std::vector<std::string>{"https://second.test/"}, "multiple links on one logical line");
    std::size_t annotations = 0;
    expect(boxrig_ui_view_query_text_annotation_visual_ranges(view, 0, nullptr, 0, &annotations) == BOXRIG_UI_LIMIT_EXCEEDED && annotations == 2, "both links have annotations");
    terminal_links_attach_view(links.get(), nullptr);
    terminal_links_attach_view(links.get(), view);
    scan();
    expect(action(0, 4).arguments == std::vector<std::string>{"https://example.みんな/"}, "reattachment restores screen annotations");
    const auto old_hit = hit(0, 4);
    feed("\x1b[1;1H\x1b[2Kreplacement");
    expect(!terminal_links_resolve(links.get(), old_hit), "old text revision cannot launch a command");
    expect(!terminal_links_copy_target(links.get(), old_hit), "stale text cannot be offered for link copying");
    scan();
    expect(!terminal_links_resolve(links.get(), hit(0, 4)), "replaced text removes the action");

    feed("\x1b[2;1H\x1b]8;;vscode://file/tmp/a%20b.cpp:12:3\x1b\\open file\x1b]8;;\x1b\\");
    scan();
    const auto vscode = action(1, 2);
    expect(terminal_links_copy_target(links.get(), hit(1, 2)) == "vscode://file/tmp/a%20b.cpp:12:3",
        "copy the original OSC 8 URI without decoding or command expansion");
    expect(vscode.command == "code" && vscode.arguments == std::vector<std::string>{"--reuse-window", "--goto", "/tmp/a b.cpp:12:3"}, "default VS Code captures are decoded once");
    const auto vscode_counters = terminal_links_cache_statistics(links.get());
    expect(action(1, 2).arguments == vscode.arguments, "repeated OSC 8 hit keeps the action");
    const auto repeated_counters = terminal_links_cache_statistics(links.get());
    expect(repeated_counters.matcher_creations == vscode_counters.matcher_creations &&
        repeated_counters.cache_hits > vscode_counters.cache_hits,
        "repeated OSC 8 hit reuses completed regex outcomes");
    feed("\x1b[3;1H\x1b]8;;https://good.test/　bad\x1b\\invalid\x1b]8;;\x1b\\");
    scan();
    expect(!terminal_links_resolve(links.get(), hit(2, 2)), "OSC 8 requires full target matching");
    feed("\x1b[4;1H\x1b]8;;vscode://file/tmp/a%00b.cpp:12\x1b\\bad path\x1b]8;;\x1b\\");
    scan();
    const auto rejected = terminal_links_resolve(links.get(), hit(3, 2));
    expect(rejected && !rejected->action && !rejected->error.empty(), "invalid decoded paths are rejected");
    const auto rejected_counters = terminal_links_cache_statistics(links.get());
    const auto rejected_again = terminal_links_resolve(links.get(), hit(3, 2));
    expect(terminal_links_copy_target(links.get(), hit(3, 2)) == "vscode://file/tmp/a%00b.cpp:12",
        "copying a detected target does not require permission to execute its action");
    expect(rejected_again && !rejected_again->action && rejected_again->error == rejected->error,
        "cached regex match still validates the action");
    expect(terminal_links_cache_statistics(links.get()).matcher_creations == rejected_counters.matcher_creations,
        "action rejection is not mistaken for a regex miss");

    terminal_links_attach_view(links.get(), nullptr);
    expect(!terminal_links_resolve(links.get(), hit(1, 2)), "detached view cannot resolve actions");
    terminal_links_attach_view(links.get(), view);
    scan();
    expect(action(1, 2).arguments == vscode.arguments, "reattachment preserves OSC 8 and rules");

    // The event queue either retains a complete pending result set or requests
    // a rescan. Neither path may expose only some of the three links.
    feed("\x1b[5;1Hhttps://one.test/\r\nhttps://two.test/\r\nhttps://three.test/");
    std::size_t used = 0;
    bool more = false;
    expect(boxrig_terminal_resume_text_recognizers(terminal, 100000, &used, &more) == BOXRIG_STATUS_OK, "force event overflow");
    scan();
    const auto first_overflow = terminal_links_resolve(links.get(), hit(4, 3));
    const auto second_overflow = terminal_links_resolve(links.get(), hit(5, 3));
    const auto third_overflow = terminal_links_resolve(links.get(), hit(6, 3));
    const bool all_overflow = first_overflow && first_overflow->action &&
        second_overflow && second_overflow->action && third_overflow && third_overflow->action;
    const bool none_overflow = !first_overflow && !second_overflow && !third_overflow;
    expect(all_overflow || none_overflow, "event capacity exhaustion publishes no partial link set");
    if (all_overflow)
      expect(third_overflow->action->arguments == std::vector<std::string>{"https://three.test/"},
          "pending notifications retain the third link");
    feed("\x1b[7;1H\x1b[2K");
    scan();
    expect(action(4, 3).arguments == std::vector<std::string>{"https://one.test/"}, "rescan restores the complete current annotation set");

    const std::vector<std::string> separators = {"\t", "\n", "\v", "\f", "\r", " ", "\u0085", "\u00a0", "\u1680", "\u180e",
        "\u2000", "\u2001", "\u2002", "\u2003", "\u2004", "\u2005", "\u2006", "\u2007", "\u2008", "\u2009", "\u200a",
        "\u2028", "\u2029", "\u202f", "\u205f", "\u3000"};
    std::string reason;
    const auto defaults = default_hyperlink_action_rules();
    const std::shared_ptr<RegularExpressionState> http_text(
        create_regular_expression(defaults[1].pattern, &reason), destroy_regular_expression);
    const std::shared_ptr<RegularExpressionState> http_osc8(
        create_regular_expression(defaults[0].pattern, &reason), destroy_regular_expression);
    expect(static_cast<bool>(http_text), "compile default HTTP pattern");
    for (const auto &separator : separators) {
      const std::string input = "https://good.test/" + separator + "bad";
      boxrig_regex_matcher *matcher = nullptr;
      std::size_t error = 0;
      expect(boxrig_regex_matcher_create(http_text.get(), reinterpret_cast<const uint8_t *>(input.data()),
          input.size(), 0, &error, &matcher) == BOXRIG_STATUS_OK, "create boundary matcher");
      std::unique_ptr<boxrig_regex_matcher, decltype(&boxrig_regex_matcher_destroy)> matcher_owner(matcher, boxrig_regex_matcher_destroy);
      boxrig_regex_progress progress = BOXRIG_REGEX_IN_PROGRESS;
      boxrig_regex_match matched{};
      while (progress == BOXRIG_REGEX_IN_PROGRESS)
        expect(boxrig_regex_matcher_resume(matcher, 1, &used, &progress, &matched) == BOXRIG_STATUS_OK, "resume boundary matcher");
      expect(progress == BOXRIG_REGEX_MATCH && matched.start == 0 && matched.end == 18, "all 26 separators terminate the default HTTP match");
      matcher_owner.reset();
      expect(boxrig_regex_matcher_create(http_osc8.get(), reinterpret_cast<const uint8_t *>(input.data()),
          input.size(), 0, &error, &matcher) == BOXRIG_STATUS_OK, "create whole-URI matcher");
      matcher_owner.reset(matcher);
      progress = BOXRIG_REGEX_IN_PROGRESS;
      while (progress == BOXRIG_REGEX_IN_PROGRESS)
        expect(boxrig_regex_matcher_resume(matcher, 1, &used, &progress, &matched) == BOXRIG_STATUS_OK, "resume whole-URI matcher");
      expect(progress == BOXRIG_REGEX_NOT_FOUND, "all 26 separators reject the whole URI");
      // C0 controls and C1 NEL are not retained as URI bytes by the OSC parser;
      // those input strings are covered directly by the matcher above.
      if (static_cast<unsigned char>(separator.front()) >= 0x20 && separator != "\u0085") {
        feed("\x1b[8;1H\x1b[2K\x1b]8;;https://good.test/" + separator + "bad\x1b\\invalid\x1b]8;;\x1b\\");
        scan();
        const auto target_hit = hit(7, 2);
        const auto resolved = terminal_links_resolve(links.get(), target_hit);
        expect(!resolved, "Unicode separator rejects the complete retained OSC 8 target");
      }
    }

    links.reset();
    feed("\x1b[2J\x1b[H");
    auto custom = default_hyperlink_action_rules()[1];
    custom.pattern = R"((?<scheme>https?)://(?<host>[\p{L}0-9.-]+))";
    custom.command = "first";
    custom.arguments = {"${scheme}", "${host}", "${2}", "$$${0}"};
    auto second = custom;
    second.id = "second";
    second.command = "second";
    links.reset(create_terminal_links(view, {custom, second}));
    feed("\x1b[1;1H\x1b[2K前 https://example.みんな/😀");
    scan();
    const auto first = action(0, 4);
    expect(first.command == "first" && first.arguments == std::vector<std::string>{"https", "example.みんな", "example.みんな", "$https://example.みんな"}, "screen captures retain UTF-8 offsets and rule priority");
    expect(terminal_links_copy_target(links.get(), hit(0, 4)) == "https://example.みんな",
        "copy the full rule match rather than any expanded action argument");

    links.reset();
    custom.pattern = R"(https://[^ ]+)";
    custom.arguments = {"${0}"};
    links.reset(create_terminal_links(view, {custom}));
    feed("\x1b[2J\x1b[Hhttps://host/a)、後続");
    scan();
    expect(action(0, 3).arguments == std::vector<std::string>{"https://host/a)、後続"} &&
        terminal_links_copy_target(links.get(), hit(0, 3)) == "https://host/a)、後続",
        "an edited rule keeps exact captures even with the built-in identifier");

    links.reset();
    custom.pattern = "x";
    custom.arguments = {"${0}"};
    links.reset(create_terminal_links(view, {custom}));
    feed("\x1b[2J\x1b[H" + std::string(4097, 'x'));
    scan();
    expect(!terminal_links_resolve(links.get(), hit(0, 2)), "incomplete scope never publishes a partial link set");
    expect(!terminal_links_copy_target(links.get(), hit(0, 2)), "incomplete recognition cannot be copied as a link");
    annotations = 0;
    expect(boxrig_ui_view_query_text_annotation_visual_ranges(view, 0, nullptr, 0, &annotations) == BOXRIG_UI_OK && annotations == 0, "incomplete recognition clears annotations");

    links.reset();
    expect(boxrig_terminal_set_scrollback_capacity(terminal, 0) == BOXRIG_STATUS_OK, "disable retained history");
    feed("\x1b[2J\x1b[Hhttps://evicted.test/\r\nhttps://retained.test/\x1b[8;1H");
    links.reset(create_terminal_links(view, default_hyperlink_action_rules()));
    scan();
    terminal_links_set_decorations(links.get(), {TerminalLinkDecoration::underline, TerminalLinkDecoration::underline});
    const auto annotation_at = [&](std::size_t row) {
      boxrig_ui_text_annotation_visual_range ranges[2]{};
      std::size_t count = 0;
      expect(boxrig_ui_view_query_text_annotation_visual_ranges(view, row, ranges, 2, &count) == BOXRIG_UI_OK && count == 1,
          "one visible link annotation");
      expect((ranges[0].annotation.style.field_mask & BOXRIG_UI_ANNOTATION_UNDERLINE_STYLE) != 0 &&
          ranges[0].annotation.style.underline_style == BOXRIG_UNDERLINE_SINGLE,
          "visible link keeps its underline");
      return ranges[0].annotation;
    };
    const auto evicted_annotation = annotation_at(0);
    const auto retained_annotation = annotation_at(1);
    expect(evicted_annotation.source_id == retained_annotation.source_id &&
        evicted_annotation.item_id != retained_annotation.item_id,
        "different links have stable item identities within one recognizer");
    expect(action(1, 3).arguments == std::vector<std::string>{"https://retained.test/"}, "recognize links before eviction");
    feed("\n");
    scan();
    const auto surviving_annotation = annotation_at(0);
    expect(surviving_annotation.source_id == retained_annotation.source_id &&
        surviving_annotation.item_id == retained_annotation.item_id,
        "eviction keeps the surviving recognizer and match identities");
    for (std::size_t row = 0; row < config.rows; ++row) {
      boxrig_ui_text_annotation_visual_range ranges[2]{};
      std::size_t count = 0;
      expect(boxrig_ui_view_query_text_annotation_visual_ranges(view, row, ranges, 2, &count) == BOXRIG_UI_OK,
          "query current link annotations after eviction");
      for (std::size_t index = 0; index < count; ++index)
        expect(ranges[index].annotation.item_id != evicted_annotation.item_id ||
            ranges[index].annotation.source_id != evicted_annotation.source_id,
            "evicted link annotation is absent");
    }
    const auto empty_hit = hit(1, 3);
    boxrig_ui_text_annotation empty_annotations[2]{};
    std::size_t empty_count = 0;
    expect(boxrig_ui_view_query_text_annotation_hits(view, &empty_hit,
        BOXRIG_UI_ANNOTATION_HIT_ALL, empty_annotations, 2, &empty_count, nullptr) == BOXRIG_UI_OK && empty_count == 0,
        "evicted link has no hit on the newly empty row");
    expect(!terminal_links_resolve(links.get(), empty_hit), "evicted link has no action");
    expect(action(0, 3).arguments == std::vector<std::string>{"https://retained.test/"}, "eviction preserves surviving link annotations");
    const auto wrapped_url = "https://wrapped.test/" + std::string(130, 'a');
    feed("\x1b[2J\x1b[H" + wrapped_url);
    scan();
    expect(terminal_links_copy_target(links.get(), hit(1, 2)) == wrapped_url,
        "a hit on a wrapped URL copies the whole logical match");

    feed("\x1b[2J\x1b[H00000 https://retained.test/");
    scan();
    const auto before_counter = annotation_at(0);
    const auto before_counter_hit = hit(0, 10);
    feed("\x1b[H00001");
    terminal_links_refresh_viewport(links.get());
    expect(annotation_at(0).item_id == before_counter.item_id,
        "unrelated text updates retain the visible link identity before recognition completes");
    expect(!terminal_links_resolve(links.get(), before_counter_hit) && !terminal_links_resolve(links.get(), hit(0, 10)),
        "retaining decoration never authorizes an obsolete capture or pointer hit");
    scan();
    expect(annotation_at(0).item_id == before_counter.item_id &&
        action(0, 10).arguments == std::vector<std::string>{"https://retained.test/"},
        "fresh recognition restores activation without replacing identical decoration");
    feed("\x1b[1;7H                     ");
    terminal_links_refresh_viewport(links.get());
    annotations = 0;
    expect(boxrig_ui_view_query_text_annotation_visual_ranges(view, 0, nullptr, 0, &annotations) == BOXRIG_UI_OK && annotations == 0,
        "changed link text loses its decoration before recognition completes");
    links.reset();
    links.reset(create_terminal_links(view, default_hyperlink_action_rules()));
    feed("\x1b[2J\x1b[Hhttps://hover.test/\r\n\x1b]8;;custom://unhandled\x1b\\explicit\x1b]8;;\x1b\\");
    scan();
    terminal_links_refresh_viewport(links.get());
    const auto style_at = [&](std::size_t row, std::size_t column) {
      const auto point = hit(row, column);
      boxrig_ui_text_annotation found[32]{};
      std::size_t count = 0;
      boxrig_ui_text_annotation_style style{};
      expect(boxrig_ui_view_query_text_annotation_hits(view, &point, BOXRIG_UI_ANNOTATION_HIT_ALL,
          found, 32, &count, &style) == BOXRIG_UI_OK, "read link presentation");
      return style;
    };
    expect(style_at(0, 2).underline_style == BOXRIG_UNDERLINE_NONE,
        "default regex decoration waits for hover");
    expect(style_at(1, 2).underline_style == BOXRIG_UNDERLINE_SINGLE &&
        !terminal_links_resolve(links.get(), hit(1, 2)), "explicit OSC 8 is decorated without granting an unsupported action");

    feed("\x1b[2J\x1b[Hhttps://same.test/ https://same.test/\r\n"
        "\x1b]8;;https://osc.test/)\x1b\\https://same.test/\x1b]8;;\x1b\\\r\n"
        "\x1b]8;;custom://unhandled\x1b\\explicit\x1b]8;;\x1b\\\r\n"
        "\x1b[4:2mhttps://double.test/\x1b[0m\r\n" + std::string(90, ' ') + wrapped_url);
    scan();
    terminal_links_refresh_viewport(links.get());
    const auto cache_before_hover = terminal_links_cache_statistics(links.get());
    const std::vector modes{TerminalLinkDecoration::none, TerminalLinkDecoration::underline,
        TerminalLinkDecoration::hover, TerminalLinkDecoration::validated};
    const auto expected_underline = [](TerminalLinkDecoration mode, bool hovered) {
      return mode == TerminalLinkDecoration::underline || mode == TerminalLinkDecoration::validated ||
          (mode == TerminalLinkDecoration::hover && hovered) ? BOXRIG_UNDERLINE_SINGLE : BOXRIG_UNDERLINE_NONE;
    };
    for (const auto regex : modes) for (const auto osc8 : modes) {
      terminal_links_set_decorations(links.get(), {regex, osc8});
      terminal_links_set_hover(links.get(), {});
      expect(style_at(0, 2).underline_style == expected_underline(regex, false) &&
          style_at(1, 2).underline_style == expected_underline(osc8, false), "independent nonhovered modes");
      terminal_links_set_hover(links.get(), hit(0, 2));
      expect(style_at(0, 2).underline_style == expected_underline(regex, true) &&
          style_at(0, 22).underline_style == expected_underline(regex, false) &&
          style_at(1, 2).underline_style == expected_underline(osc8, false), "only the pointed occurrence is hovered");
      terminal_links_set_hover(links.get(), hit(1, 2));
      expect(style_at(0, 2).underline_style == expected_underline(regex, false) &&
          style_at(1, 2).underline_style == expected_underline(osc8, true), "OSC 8 display wins over regex in every combination");
      terminal_links_set_hover(links.get(), hit(2, 2));
      expect(style_at(2, 2).underline_style == expected_underline(osc8, true) &&
          !terminal_links_resolve(links.get(), hit(2, 2)), "hover never grants an unsupported action");
      boxrig_cell_view cell{};
      expect(boxrig_terminal_get_cell(terminal, 3, 2, &cell) == BOXRIG_STATUS_OK, "read terminal double underline");
      for (const bool hovered : {true, false}) {
        terminal_links_set_hover(links.get(), hovered ? hit(3, 2) : boxrig_ui_hit_test_result{});
        const auto style = style_at(3, 2);
        expect(boxrig_ui_annotation_underline_style(&cell, &style) == BOXRIG_UNDERLINE_DOUBLE,
            "hover preserves the terminal's double underline");
      }
    }
    expect(terminal_links_cache_statistics(links.get()).matcher_creations == cache_before_hover.matcher_creations,
        "hover changes reuse recognition and URI matching");
    terminal_links_set_decorations(links.get(), {});
    terminal_links_set_hover(links.get(), hit(5, 5));
    expect(style_at(4, 95).underline_style == BOXRIG_UNDERLINE_SINGLE &&
        style_at(5, 5).underline_style == BOXRIG_UNDERLINE_SINGLE, "hover highlights every wrapped fragment");
    const auto stale_hover = hit(0, 2);
    feed("\x1b[1;1H\x1b[2Kremoved");
    terminal_links_refresh_viewport(links.get());
    terminal_links_set_hover(links.get(), stale_hover);
    expect(style_at(5, 5).underline_style == BOXRIG_UNDERLINE_NONE, "stale hover clears the previous occurrence");
    terminal_links_attach_view(links.get(), nullptr);
    terminal_links_attach_view(links.get(), view);
    scan();
    terminal_links_refresh_viewport(links.get());
    expect(style_at(5, 5).underline_style == BOXRIG_UNDERLINE_NONE &&
        style_at(1, 2).underline_style == BOXRIG_UNDERLINE_SINGLE, "reattachment restores preferences without stale hover");

    links.reset();
    links.reset(create_terminal_links(view, {}));
    terminal_links_refresh_viewport(links.get());
    expect(style_at(1, 2).underline_style == BOXRIG_UNDERLINE_SINGLE &&
        !terminal_links_resolve(links.get(), hit(1, 2)), "an empty rule list still displays explicit OSC 8 labels");
    std::puts("boxrig links: Unicode boundaries, captures, annotations and stale hits passed");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
