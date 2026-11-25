// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "../../src/terminal-received-macros.h"

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
    std::string sent;
    std::vector<std::vector<std::string>> commands;
    std::vector<std::string> errors;
    bool blocked = false;
    const TerminalReceivedMacroCallbacks callbacks{
      [](std::string text) { return text; },
      [&](std::span<const unsigned char> bytes) {
        if (blocked) return false;
        expect(!bytes.empty() && bytes.size() <= 4096 && (bytes.front() & 0xc0) != 0x80,
            "send chunks are bounded and start at UTF-8 scalar boundaries");
        sent.append(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        return true;
      },
      [&](std::string command, std::vector<std::string> args) {
        args.insert(args.begin(), std::move(command));
        commands.push_back(std::move(args));
      },
      [&](std::string message) { errors.push_back(std::move(message)); },
    };
    const std::vector<MacroRule> rules{
      {"ready", R"(READY (?<name>[\p{L}]+))", MacroSendAction{"${1}:${name}:$$:${0};"}},
      {"lower-priority", R"(READY (?<name>[\p{L}]+))", MacroSendAction{"wrong"}},
      {"early", "^GO$", MacroSendAction{"early;"}},
      {"control", R"(^\u{1b}\[31mERROR (?<code>[0-9]+)$)",
        MacroCommandAction{"tool-${code}", {"${0}", "$$${code}"}}},
    };
    std::unique_ptr<TerminalReceivedMacros, decltype(&destroy_terminal_received_macros)> macros(
        create_terminal_received_macros(rules, callbacks), destroy_terminal_received_macros);
    std::uint64_t generation = 1;
    const auto feed = [&](std::string_view text) {
      expect(terminal_received_macros_feed(macros.get(), generation,
          {reinterpret_cast<const unsigned char *>(text.data()), text.size()}), "accept bounded receive notification");
    };
    const auto drain = [&](std::size_t budget) {
      TerminalReceivedMacroProgress progress;
      std::size_t steps = 0;
      do {
        progress = terminal_received_macros_dispatch(macros.get(), budget);
        expect(progress.queued_bytes <= 2 * 1024 * 1024 && progress.line_bytes <= 1024 * 1024, "bounded stream storage");
        expect(++steps < 1000000, "finite macro dispatch");
      } while (progress.pending && !progress.blocked);
      return progress;
    };

    feed("prefix REA");
    feed("DY 記者");
    feed(" and READY Later");
    expect(sent.empty(), "receive callback only retains data");
    drain(1);
    expect(sent == "記者:記者:$:READY 記者;", "ordered snapshots preserve captures and one action per line before LF");
    feed("\r");
    feed("\nGO");
    feed("NEVER");
    drain(1);
    expect(sent.ends_with("early;"), "intermediate exact match is not lost to a later notification");
    feed("\n\x1b[31mERROR 42\r");
    feed("\n");
    drain(1);
    expect(commands == std::vector<std::vector<std::string>>{{"tool-42", "\x1b[31mERROR 42", "$42"}}, "control sequences and CRLF boundaries reach command captures");

    feed("READY Discarded");
    terminal_received_macros_dispatch(macros.get(), 1);
    terminal_received_macros_reset(macros.get(), ++generation);
    const std::string stale = "READY Old\n";
    expect(terminal_received_macros_feed(macros.get(), generation - 1,
        {reinterpret_cast<const unsigned char *>(stale.data()), stale.size()}), "ignore a late old-generation notification");
    const auto before = sent;
    blocked = true;
    feed("READY New\n");
    expect(drain(1).blocked && sent == before, "blocked sending retains exactly one action");
    blocked = false;
    drain(1);
    expect(sent == before + "New:New:$:READY New;", "resume only the new generation's action");

    feed(std::string(1024 * 1024 + 200, 'x'));
    drain(4096);
    feed("\nREADY End\n");
    drain(1);
    expect(sent.ends_with("End:End:$:READY End;"), "oversized lines remain bounded and the next line still matches");
    const std::string overflow(2 * 1024 * 1024 + 1, 'x');
    expect(!terminal_received_macros_feed(macros.get(), generation,
        {reinterpret_cast<const unsigned char *>(overflow.data()), overflow.size()}), "queue overflow accepts no partial notification");
    expect(!drain(1).pending, "rejected input creates no matching work");

    const auto payload = std::string(4095, 'x') + "日本語" + std::string(70000, 'y');
    macros.reset(create_terminal_received_macros({{"long", "LARGE", MacroSendAction{payload}}}, callbacks));
    sent.clear();
    feed("LARGE\nLARGE\n");
    drain(1);
    expect(sent == payload + payload, "large sends use finite complete UTF-8 chunks without loss");
    macros.reset(create_terminal_received_macros({
        {"cr", R"(^A\r$)", MacroSendAction{"CR"}},
        {"nul", R"(B(?<nul>\u{0})C)", MacroSendAction{"${nul}done"}},
        {"absent", "(x)?Y", MacroSendAction{"[${1}]${0}"}},
    }, callbacks));
    sent.clear();
    feed("A\r");
    feed("Later\n");
    feed(std::string_view("B\0C\nY\n", 6));
    drain(1);
    expect(sent == std::string("CR\0done[]Y", 10), "a lone CR, embedded NUL, and absent capture preserve received bytes");
    expect(errors.empty(), "valid macros produce no diagnostics");
    macros.reset(create_terminal_received_macros(rules, callbacks));
    sent.clear();
    feed("RE");
    drain(1);
    auto saved = terminal_received_macros_snapshot(macros.get());
    expect(saved.has_value(), "an idle partial line can move to another runner");
    macros.reset(create_terminal_received_macros(rules, callbacks));
    terminal_received_macros_restore(macros.get(), ++generation, *saved);
    feed("ADY Dock\n");
    drain(1);
    expect(sent == "Dock:Dock:$:READY Dock;", "a pattern spanning a terminal handoff executes once");
    std::puts("boxrig macros: snapshots, captures, controls, generations and backpressure passed");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
