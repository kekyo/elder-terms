#include "../../src/terminal-hyperlink-resolver.h"

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <elder-terms/settings/hyperlink-settings.h>

namespace elder_terms_terminal_hyperlink_resolver_test {

static void expect_true(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

static void test_resolves_built_in_vscode_targets() {
  elder_terms::TerminalHyperlinkResolverState *resolver =
      elder_terms::create_terminal_hyperlink_resolver(
          elder_terms::default_hyperlink_action_rules());

  const std::optional<elder_terms::TerminalHyperlinkAction> line_column =
      elder_terms::resolve_terminal_hyperlink(
          resolver,
          "vscode://file/tmp/source%20file.cpp:42:7");
  expect_true(
      line_column.has_value() && line_column->command == "code" &&
          line_column->arguments ==
              std::vector<std::string>{"--reuse-window", "--goto",
                                       "/tmp/source file.cpp:42:7"},
      "the built-in rule should decode the path and preserve line and column");

  const std::optional<elder_terms::TerminalHyperlinkAction> line =
      elder_terms::resolve_terminal_hyperlink(
          resolver, "vscode://file/tmp/source.cpp:11");
  expect_true(line.has_value() && line->command == "code" &&
                  line->arguments ==
                      std::vector<std::string>{"--reuse-window", "--goto",
                                               "/tmp/source.cpp:11"},
              "the built-in fallback should support a line without a column");

  elder_terms::destroy_terminal_hyperlink_resolver(resolver);
}

static void test_uses_full_matches_priority_and_safe_templates() {
  elder_terms::TerminalHyperlinkResolverState *resolver =
      elder_terms::create_terminal_hyperlink_resolver({
          elder_terms::HyperlinkActionRule{
              .id = "first",
              .pattern =
                  R"(^tool:(?<path>/[^:]+):(?<line>[0-9]+)$)",
              .command = "literal-${line}",
              .arguments = {"${0}", "${path|uri-decode}", "$$${line}"},
          },
          elder_terms::HyperlinkActionRule{
              .id = "second",
              .pattern = R"(^tool:.*$)",
              .command = "second-tool",
              .arguments = {},
          },
          elder_terms::HyperlinkActionRule{
              .id = "substring",
              .pattern = "OPEN",
              .command = "substring-tool",
              .arguments = {},
          },
      });

  const std::optional<elder_terms::TerminalHyperlinkAction> resolved =
      elder_terms::resolve_terminal_hyperlink(
          resolver, "tool:/tmp/A%20B.cpp:9");
  expect_true(
      resolved.has_value() && resolved->command == "literal-${line}" &&
          resolved->arguments ==
              std::vector<std::string>{"tool:/tmp/A%20B.cpp:9",
                                       "/tmp/A B.cpp", "$9"},
      "only argument templates should expand and the first full match should "
      "win");
  expect_true(!elder_terms::resolve_terminal_hyperlink(
                   resolver, "prefix OPEN suffix")
                   .has_value(),
              "a substring match must not activate a hyperlink action");
  expect_true(!elder_terms::resolve_terminal_hyperlink(
                   resolver, "tool:/tmp/%ZZ.cpp:9")
                   .has_value(),
              "invalid URI escapes must not produce a command");

  elder_terms::destroy_terminal_hyperlink_resolver(resolver);
}

} // namespace elder_terms_terminal_hyperlink_resolver_test

int main() {
  elder_terms_terminal_hyperlink_resolver_test::
      test_resolves_built_in_vscode_targets();
  elder_terms_terminal_hyperlink_resolver_test::
      test_uses_full_matches_priority_and_safe_templates();
  return 0;
}
