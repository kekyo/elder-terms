// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <cardio.h>

#include <elder-terms/settings/hyperlink-settings.h>
#include <elder-terms/settings/regex-capture-template.h>

namespace elder_terms {

/**
 * External command resolved from one terminal link candidate.
 */
struct TerminalHyperlinkAction {
  /** Fixed executable name or path from the matching rule. */
  std::string command;
  /** Expanded ordered arguments, excluding argv[0]. */
  std::vector<std::string> arguments;
  /** Absolute local path to validate asynchronously before execution, if any. */
  std::optional<std::string> validation_path{};

  /** Compares the expanded command, arguments and validation target. */
  bool operator==(const TerminalHyperlinkAction &) const = default;
};

/**
 * Result of evaluating terminal link candidates against ordered rules.
 */
struct TerminalHyperlinkResolution {
  /** Expanded action, absent while validation is pending or rejected. */
  std::optional<TerminalHyperlinkAction> action;
  /** Rejection reason, empty while validation is pending or accepted. */
  std::string error;
  /** Recognizer identity for a text link, or zero for OSC 8. */
  std::uint64_t source_id = 0;
  /** Match identity within source_id, or zero for OSC 8. */
  std::uint64_t match_id = 0;
  /** Terminal-lifetime OSC 8 identity, or zero for a text link. */
  std::uint32_t hyperlink_handle = 0;
  /** Expanded candidate retained while path validation is pending or rejected. */
  std::optional<TerminalHyperlinkAction> candidate{};
};

/**
 * Expands an action and validates its syntax without filesystem I/O.
 * @param rule Rule whose command and path policy are applied.
 * @param capture Borrowed lookup into an immutable match.
 * @param reason Receives a template or path rejection reason.
 * @return Command, arguments and optional validation path, or no value on rejection.
 */
std::optional<TerminalHyperlinkAction> expand_terminal_hyperlink_action(
    const HyperlinkActionRule &rule, const RegexCaptureLookup &capture, std::string *reason);

/**
 * Checks an absolute local path without blocking the caller's UI thread.
 * @param path Owned absolute path; symlinks are followed.
 * @param cancellation Cancels the asynchronous query.
 * @return Empty error for a regular file or directory, otherwise a diagnostic.
 * @remarks Requires a cardio dispatcher with GIO support. Cancellation propagates.
 */
cardio::promise<std::string> validate_terminal_hyperlink_path_async(
    std::string path, cardio::cancellation cancellation);

/** Asynchronous path check; an empty result accepts the path, cancellation propagates. */
using TerminalHyperlinkPathValidator = std::function<cardio::promise<std::string>(
    std::string, cardio::cancellation)>;

} // namespace elder_terms
