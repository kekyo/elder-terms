#include "../../src/terminal-macro-runner.h"

#include <cstddef>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace elder_terms_terminal_macro_runner_test {

struct RecordedCommand {
  std::string command;
  std::vector<std::string> arguments;
};

static void expect_true(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

static void feed(elder_terms::TerminalMacroRunnerState *runner,
                 const std::string &text) {
  elder_terms::feed_terminal_macro_runner(
      runner,
      std::span<const unsigned char>(
          reinterpret_cast<const unsigned char *>(text.data()), text.size()));
}

static void test_matches_incremental_lines_in_priority_order() {
  std::vector<std::string> sent;
  std::vector<RecordedCommand> commands;
  elder_terms::TerminalMacroRunnerState *runner =
      elder_terms::create_terminal_macro_runner(
          {
              elder_terms::MacroRule{
                  .id = "first",
                  .pattern = R"(READY (?<name>[A-Za-z]+))",
                  .action = elder_terms::MacroSendAction{
                      .text = "first:${1}:${name}:$$:${0}",
                  },
              },
              elder_terms::MacroRule{
                  .id = "second",
                  .pattern = R"(READY (?<name>[A-Za-z]+))",
                  .action = elder_terms::MacroCommandAction{
                      .command = "should-not-run",
                      .arguments = {},
                  },
              },
          },
          {
              .send = [&sent](std::string text) {
                sent.push_back(std::move(text));
              },
              .command = [&commands](std::string command,
                                     std::vector<std::string> arguments) {
                commands.push_back({
                    .command = std::move(command),
                    .arguments = std::move(arguments),
                });
              },
          });

  feed(runner, "prefix REA");
  expect_true(sent.empty(), "an incomplete match must not execute");
  feed(runner, "DY Bob");
  expect_true(sent ==
                  std::vector<std::string>{"first:Bob:Bob:$:READY Bob"},
              "a complete match should expand captures before LF");
  expect_true(commands.empty(),
              "only the first matching rule should execute");
  feed(runner, " and READY Eve");
  expect_true(sent.size() == 1,
              "a logical line should execute at most one action");
  feed(runner, "\nREADY Ana\r\n");
  expect_true(sent ==
                  std::vector<std::string>{
                      "first:Bob:Bob:$:READY Bob",
                      "first:Ana:Ana:$:READY Ana",
                  },
              "LF should reset matching for the next logical line");

  elder_terms::destroy_terminal_macro_runner(runner);
}

static void test_expands_commands_and_preserves_control_sequences() {
  std::vector<std::string> sent;
  std::vector<RecordedCommand> commands;
  elder_terms::TerminalMacroRunnerState *runner =
      elder_terms::create_terminal_macro_runner(
          {
              elder_terms::MacroRule{
                  .id = "initial",
                  .pattern = "INITIAL",
                  .action = elder_terms::MacroSendAction{.text = "initial"},
              },
          },
          {
              .send = [&sent](std::string text) {
                sent.push_back(std::move(text));
              },
              .command = [&commands](std::string command,
                                     std::vector<std::string> arguments) {
                commands.push_back({
                    .command = std::move(command),
                    .arguments = std::move(arguments),
                });
              },
          });

  feed(runner, "INITIAL");
  elder_terms::replace_terminal_macro_runner_rules(
      runner,
      {
          elder_terms::MacroRule{
              .id = "ansi_error",
              .pattern = R"(^\x1b\[31mERROR (?<code>[0-9]+)$)",
              .action = elder_terms::MacroCommandAction{
                  .command = "tool-${code}",
                  .arguments = {"${0}", "$$${code}"},
              },
          },
      });
  feed(runner, "\x1b[31mERROR 42");

  expect_true(sent == std::vector<std::string>{"initial"},
              "the initial send action should execute once");
  expect_true(
      commands.size() == 1 && commands[0].command == "tool-42" &&
          commands[0].arguments ==
              std::vector<std::string>{"\x1b[31mERROR 42", "$42"},
      "command and argument templates should preserve controls and expand "
      "captures");

  elder_terms::destroy_terminal_macro_runner(runner);
}

static void test_strips_crlf_and_caps_the_current_line() {
  std::vector<std::string> sent;
  elder_terms::TerminalMacroRunnerState *runner =
      elder_terms::create_terminal_macro_runner(
          {
              elder_terms::MacroRule{
                  .id = "carriage_return",
                  .pattern = R"(END\r$)",
                  .action = elder_terms::MacroSendAction{.text = "bad"},
              },
          },
          {
              .send = [&sent](std::string text) {
                sent.push_back(std::move(text));
              },
              .command = {},
          });

  feed(runner, "END\r");
  feed(runner, "\n");
  expect_true(sent.empty(), "CR immediately before LF must not be matched");

  elder_terms::replace_terminal_macro_runner_rules(
      runner,
      {
          elder_terms::MacroRule{
              .id = "bounded",
              .pattern = R"(^A.*Z$)",
              .action = elder_terms::MacroSendAction{.text = "overflow"},
          },
      });
  constexpr std::size_t maximum_line_bytes = 1024 * 1024;
  std::string oversized = "A";
  oversized.append(maximum_line_bytes, 'B');
  oversized.push_back('Z');
  feed(runner, oversized);
  expect_true(sent.empty(),
              "discarding the oldest bytes should remove an over-limit "
              "line prefix");

  elder_terms::destroy_terminal_macro_runner(runner);
}

} // namespace elder_terms_terminal_macro_runner_test

int main() {
  elder_terms_terminal_macro_runner_test::
      test_matches_incremental_lines_in_priority_order();
  elder_terms_terminal_macro_runner_test::
      test_expands_commands_and_preserves_control_sequences();
  elder_terms_terminal_macro_runner_test::
      test_strips_crlf_and_caps_the_current_line();
  return 0;
}
