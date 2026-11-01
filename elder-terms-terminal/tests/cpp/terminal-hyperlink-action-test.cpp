// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "../../src/terminal-hyperlink-action.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <unistd.h>

using namespace elder_terms;

int main() {
  cardio::dispatcher_group_glib group;
  cardio::dispatcher_host_glib_auto dispatcher(group);
  const auto validate = [](std::string path) {
    auto result = validate_terminal_hyperlink_path_async(std::move(path), {});
    while (!result.is_ready()) g_main_context_iteration(nullptr, TRUE);
    return *result.try_result();
  };
  HyperlinkActionRule rule{
      .id = "file",
      .recognition_source = HyperlinkRecognitionSource::osc8,
      .pattern = R"(^file:(?<path>.+)$)",
      .command = "literal-${path}",
      .arguments = {"${path|uri-decode}", "$$${line}"},
      .path_validation = HyperlinkPathValidation::existing_local_path,
      .path_template = "${path|uri-decode}",
  };
  const auto directory = std::filesystem::temp_directory_path() /
      ("elder-terms-link-" + std::to_string(getpid()));
  std::filesystem::create_directories(directory);
  std::ofstream(directory / "source file.cpp") << "test\n";
  std::string path = directory.string() + "/source%20file.cpp";
  const RegexCaptureLookup capture = [&](const std::string &name) -> std::optional<std::string> {
    if (name == "path") return path;
    if (name == "line") return "42";
    return std::nullopt;
  };
  std::string reason;
  const auto action = expand_terminal_hyperlink_action(rule, capture, &reason);
  if (!action || action->command != "literal-${path}" || action->arguments !=
      std::vector<std::string>{(directory / "source file.cpp").string(), "$42"} ||
      !action->validation_path || !validate(*action->validation_path).empty())
    throw std::runtime_error("only argument templates should expand; an existing absolute file is accepted");
  path = directory.string();
  if (!expand_terminal_hyperlink_action(rule, capture, &reason) || !validate(path).empty())
    throw std::runtime_error("an existing directory should be accepted");
  path = "relative.cpp";
  if (expand_terminal_hyperlink_action(rule, capture, &reason) || reason.find("absolute") == std::string::npos)
    throw std::runtime_error("a relative path must be rejected");
  path = (directory / "missing").string();
  if (!expand_terminal_hyperlink_action(rule, capture, &reason) || validate(path).find("does not exist") == std::string::npos)
    throw std::runtime_error("a missing path must be rejected");
  if (validate("/dev/null").find("not a regular file or directory") == std::string::npos)
    throw std::runtime_error("a device must not be accepted as a regular file");
  path = directory.string() + "/%00.cpp";
  if (expand_terminal_hyperlink_action(rule, capture, &reason))
    throw std::runtime_error("a decoded NUL must be rejected");
  path = directory.string() + "/%ZZ.cpp";
  if (expand_terminal_hyperlink_action(rule, capture, &reason))
    throw std::runtime_error("an invalid URI escape must be rejected");
  std::filesystem::remove_all(directory);
}
