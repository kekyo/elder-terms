// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "terminal-hyperlink-action.h"

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <elder-terms/settings/regex-capture-template.h>

namespace elder_terms {

static std::optional<std::string> expand_hyperlink_template(
    const std::string &value, const RegexCaptureLookup &capture,
    std::string *reason) {
  return expand_regex_capture_template(
      value, capture,
      RegexCaptureTemplateOptions{.allow_uri_decode = true}, reason);
}

static std::optional<std::string> expand_home_path(std::string value,
                                                std::string *reason) {
  if (!value.starts_with("~/")) return value;
  // Read HOME directly: passwd lookup and filesystem inspection must never
  // run in synchronous recognition, pointer hit testing or template expansion.
  const char *home = g_getenv("HOME");
  if (!home || !std::filesystem::path(home).is_absolute()) {
    *reason = "cannot expand home-relative path: HOME must be an absolute path";
    return std::nullopt;
  }
  std::string expanded(home);
  if (!expanded.ends_with('/')) expanded += '/';
  expanded += value.substr(2);
  return expanded;
}

static bool validate_hyperlink_path_syntax(const std::string &value,
                                         std::string *reason) {
  if (value.find('\0') != std::string::npos) {
    *reason = "expanded path contains a NUL byte";
    return false;
  }

  const std::filesystem::path path(value);
  if (!path.is_absolute()) {
    *reason = "expanded path must be absolute: " + value;
    return false;
  }

  return true;
}

cardio::promise<std::string> validate_terminal_hyperlink_path_async(
    std::string path, cardio::cancellation cancellation) {
  std::string reason;
  if (!validate_hyperlink_path_syntax(path, &reason)) co_return reason;
  const std::unique_ptr<GFile, decltype(&g_object_unref)> file(g_file_new_for_path(path.c_str()), g_object_unref);
  try {
    const std::unique_ptr<GFileInfo, decltype(&g_object_unref)> info(
        co_await cardio::gio::submit<GFileInfo *>(
            [file = file.get()](GCancellable *cancel, GAsyncReadyCallback callback, gpointer data) {
              g_file_query_info_async(file, G_FILE_ATTRIBUTE_STANDARD_TYPE,
                  G_FILE_QUERY_INFO_NONE, G_PRIORITY_DEFAULT, cancel, callback, data);
            },
            [](GObject *file, GAsyncResult *result, GError **error) {
              return g_file_query_info_finish(G_FILE(file), result, error);
            }, cancellation), g_object_unref);
    const auto type = g_file_info_get_file_type(info.get());
    if (type != G_FILE_TYPE_REGULAR && type != G_FILE_TYPE_DIRECTORY)
      co_return "expanded path is not a regular file or directory: " + path;
    co_return std::string{};
  } catch (const cardio::gio::gio_error &error) {
    if (error.domain() == G_IO_ERROR && error.code() == G_IO_ERROR_NOT_FOUND)
      co_return "expanded path does not exist: " + path;
    co_return "expanded path could not be inspected: " + path + ": " + error.what();
  }
}

std::optional<TerminalHyperlinkAction> expand_terminal_hyperlink_action(
    const HyperlinkActionRule &rule, const RegexCaptureLookup &capture,
    std::string *reason) {
  TerminalHyperlinkAction action{
      .command = rule.command,
      .arguments = {},
      .validation_path = {},
  };
  if (rule.command.empty() || rule.command.find('\0') != std::string::npos) {
    *reason = "command must be nonempty and must not contain a NUL byte";
    return std::nullopt;
  }
  action.arguments.reserve(rule.arguments.size());
  for (const std::string &argument : rule.arguments) {
    const std::optional<std::string> expanded =
        expand_hyperlink_template(argument, capture, reason);
    if (!expanded.has_value()) {
      return std::nullopt;
    }
    if (expanded->find('\0') != std::string::npos) {
      *reason = "expanded argument contains a NUL byte";
      return std::nullopt;
    }
    const auto local_argument = expand_home_path(*expanded, reason);
    if (!local_argument) return std::nullopt;
    action.arguments.push_back(*local_argument);
  }

  if (rule.path_validation ==
      HyperlinkPathValidation::existing_local_path) {
    const std::optional<std::string> path =
        expand_hyperlink_template(rule.path_template, capture, reason);
    if (!path) return std::nullopt;
    const auto local_path = expand_home_path(*path, reason);
    if (!local_path || !validate_hyperlink_path_syntax(*local_path, reason)) {
      return std::nullopt;
    }
    action.validation_path = *local_path;
  }
  return action;
}

} // namespace elder_terms
