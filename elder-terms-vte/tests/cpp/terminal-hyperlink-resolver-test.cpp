#include "../../src/terminal-hyperlink-resolver.h"

#include <chrono>
#include <filesystem>
#include <fstream>
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
          elder_terms::TerminalHyperlinkCandidates{
              .osc8_target =
                  "vscode://file/tmp/source%20file.cpp:42:7",
              .terminal_text = {},
          })
          ->action;
  expect_true(
      line_column.has_value() && line_column->command == "code" &&
          line_column->arguments ==
              std::vector<std::string>{"--reuse-window", "--goto",
                                       "/tmp/source file.cpp:42:7"},
      "the built-in rule should decode the path and preserve line and column");

  const std::optional<elder_terms::TerminalHyperlinkAction> line =
      elder_terms::resolve_terminal_hyperlink(
          resolver,
          elder_terms::TerminalHyperlinkCandidates{
              .osc8_target = "vscode://file/tmp/source.cpp:11",
              .terminal_text = {},
          })
          ->action;
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
              .recognition_source =
                  elder_terms::HyperlinkRecognitionSource::osc8,
              .pattern =
                  R"(^tool:(?<path>/[^:]+):(?<line>[0-9]+)$)",
              .command = "literal-${line}",
              .arguments = {"${0}", "${path|uri-decode}", "$$${line}"},
          },
          elder_terms::HyperlinkActionRule{
              .id = "second",
              .recognition_source =
                  elder_terms::HyperlinkRecognitionSource::osc8,
              .pattern = R"(^fallback:.*$)",
              .command = "second-tool",
              .arguments = {},
          },
          elder_terms::HyperlinkActionRule{
              .id = "substring",
              .recognition_source =
                  elder_terms::HyperlinkRecognitionSource::osc8,
              .pattern = "OPEN",
              .command = "substring-tool",
              .arguments = {},
          },
      });

  const std::optional<elder_terms::TerminalHyperlinkAction> resolved =
      elder_terms::resolve_terminal_hyperlink(
          resolver,
          elder_terms::TerminalHyperlinkCandidates{
              .osc8_target = "tool:/tmp/A%20B.cpp:9",
              .terminal_text = {},
          })
          ->action;
  expect_true(
      resolved.has_value() && resolved->command == "literal-${line}" &&
          resolved->arguments ==
              std::vector<std::string>{"tool:/tmp/A%20B.cpp:9",
                                       "/tmp/A B.cpp", "$9"},
      "only argument templates should expand and the first full match should "
      "win");
  expect_true(!elder_terms::resolve_terminal_hyperlink(
                   resolver,
                   elder_terms::TerminalHyperlinkCandidates{
                       .osc8_target = "prefix OPEN suffix",
                       .terminal_text = {},
                   })
                   .has_value(),
              "a substring match must not activate a hyperlink action");
  const std::optional<elder_terms::TerminalHyperlinkResolution> rejected =
      elder_terms::resolve_terminal_hyperlink(
          resolver,
          elder_terms::TerminalHyperlinkCandidates{
              .osc8_target = "tool:/tmp/%ZZ.cpp:9",
              .terminal_text = {},
          });
  expect_true(rejected.has_value() && !rejected->action.has_value() &&
                  !rejected->error.empty(),
              "invalid URI escapes must not produce a command");

  elder_terms::destroy_terminal_hyperlink_resolver(resolver);
}

static void test_uses_rule_priority_across_recognition_sources() {
  elder_terms::TerminalHyperlinkResolverState *resolver =
      elder_terms::create_terminal_hyperlink_resolver({
          elder_terms::HyperlinkActionRule{
              .id = "text-first",
              .recognition_source =
                  elder_terms::HyperlinkRecognitionSource::terminal_text,
              .pattern = R"(^https?://[^\s]+$)",
              .command = "text-tool",
              .arguments = {"${0}"},
          },
          elder_terms::HyperlinkActionRule{
              .id = "osc-second",
              .recognition_source =
                  elder_terms::HyperlinkRecognitionSource::osc8,
              .pattern = R"(^https?://[^\s]+$)",
              .command = "osc-tool",
              .arguments = {"${0}"},
          },
      });

  const std::optional<elder_terms::TerminalHyperlinkResolution> resolution =
      elder_terms::resolve_terminal_hyperlink(
          resolver,
          elder_terms::TerminalHyperlinkCandidates{
              .osc8_target = "https://osc.example/",
              .terminal_text = {"https://text.example/"},
          });
  expect_true(resolution.has_value() && resolution->action.has_value() &&
                  resolution->action->command == "text-tool" &&
                  resolution->action->arguments ==
                      std::vector<std::string>{"https://text.example/"},
              "the first valid rule should win across OSC and text sources");
  elder_terms::destroy_terminal_hyperlink_resolver(resolver);
}

static void test_validates_expanded_local_paths() {
  const auto unique = std::chrono::steady_clock::now()
                          .time_since_epoch()
                          .count();
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() /
      ("elder-terms-link-path-" + std::to_string(unique));
  std::filesystem::create_directories(directory);
  const std::filesystem::path file = directory / "source file.cpp";
  std::ofstream(file) << "test\n";

  elder_terms::TerminalHyperlinkResolverState *resolver =
      elder_terms::create_terminal_hyperlink_resolver({
          elder_terms::HyperlinkActionRule{
              .id = "file",
              .recognition_source =
                  elder_terms::HyperlinkRecognitionSource::terminal_text,
              .pattern = R"(^file:(?<path>.+)$)",
              .command = "file-tool",
              .arguments = {"${path}"},
              .path_validation = elder_terms::HyperlinkPathValidation::
                  existing_local_path,
              .path_template = "${path}",
          },
      });

  const std::optional<elder_terms::TerminalHyperlinkResolution> existing =
      elder_terms::resolve_terminal_hyperlink(
          resolver,
          elder_terms::TerminalHyperlinkCandidates{
              .osc8_target = std::nullopt,
              .terminal_text = {"file:" + file.string()},
          });
  expect_true(existing.has_value() && existing->action.has_value() &&
                  existing->action->arguments ==
                      std::vector<std::string>{file.string()},
              "an existing absolute regular file should pass validation");

  const std::optional<elder_terms::TerminalHyperlinkResolution> relative =
      elder_terms::resolve_terminal_hyperlink(
          resolver,
          elder_terms::TerminalHyperlinkCandidates{
              .osc8_target = std::nullopt,
              .terminal_text = {"file:source.cpp"},
          });
  expect_true(relative.has_value() && !relative->action.has_value() &&
                  relative->error.find("absolute") != std::string::npos,
              "a relative path should be rejected explicitly");

  const std::optional<elder_terms::TerminalHyperlinkResolution> missing =
      elder_terms::resolve_terminal_hyperlink(
          resolver,
          elder_terms::TerminalHyperlinkCandidates{
              .osc8_target = std::nullopt,
              .terminal_text = {
                  "file:" + (directory / "missing").string()},
          });
  expect_true(missing.has_value() && !missing->action.has_value() &&
                  missing->error.find("does not exist") !=
                      std::string::npos,
              "a missing absolute path should be rejected explicitly");

  elder_terms::destroy_terminal_hyperlink_resolver(resolver);
  std::error_code remove_error;
  std::filesystem::remove_all(directory, remove_error);
}

static void test_evaluates_each_visible_text_rule_candidate() {
  elder_terms::TerminalHyperlinkResolverState *resolver =
      elder_terms::create_terminal_hyperlink_resolver({
          elder_terms::HyperlinkActionRule{
              .id = "specific",
              .recognition_source =
                  elder_terms::HyperlinkRecognitionSource::terminal_text,
              .pattern = R"(^first:.+$)",
              .command = "first-tool",
              .arguments = {"${0}"},
          },
          elder_terms::HyperlinkActionRule{
              .id = "fallback",
              .recognition_source =
                  elder_terms::HyperlinkRecognitionSource::terminal_text,
              .pattern = R"(^second:.+$)",
              .command = "second-tool",
              .arguments = {"${0}"},
          },
      });

  const std::optional<elder_terms::TerminalHyperlinkResolution> resolution =
      elder_terms::resolve_terminal_hyperlink(
          resolver,
          elder_terms::TerminalHyperlinkCandidates{
              .osc8_target = std::nullopt,
              .terminal_text = {"first:visible", "second:visible"},
          });
  expect_true(resolution.has_value() && resolution->action.has_value() &&
                  resolution->action->command == "first-tool" &&
                  resolution->action->arguments ==
                      std::vector<std::string>{"first:visible"},
              "each visible-text rule should evaluate its own candidate");
  elder_terms::destroy_terminal_hyperlink_resolver(resolver);
}

} // namespace elder_terms_terminal_hyperlink_resolver_test

int main() {
  elder_terms_terminal_hyperlink_resolver_test::
      test_resolves_built_in_vscode_targets();
  elder_terms_terminal_hyperlink_resolver_test::
      test_uses_full_matches_priority_and_safe_templates();
  elder_terms_terminal_hyperlink_resolver_test::
      test_uses_rule_priority_across_recognition_sources();
  elder_terms_terminal_hyperlink_resolver_test::
      test_validates_expanded_local_paths();
  elder_terms_terminal_hyperlink_resolver_test::
      test_evaluates_each_visible_text_rule_candidate();
  return 0;
}
