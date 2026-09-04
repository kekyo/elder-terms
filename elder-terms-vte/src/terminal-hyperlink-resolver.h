#pragma once

#include <optional>
#include <string>
#include <vector>

#include <elder-terms/settings/hyperlink-settings.h>

namespace elder_terms {

/**
 * External command resolved from one terminal link candidate.
 */
struct TerminalHyperlinkAction {
  /** Fixed executable name or path from the matching rule. */
  std::string command;
  /** Expanded ordered arguments, excluding argv[0]. */
  std::vector<std::string> arguments;
};

/**
 * Candidate values found at one terminal pointer position.
 */
struct TerminalHyperlinkCandidates {
  /** Complete OSC 8 target at the pointer, when present. */
  std::optional<std::string> osc8_target;
  /**
   * Visible substrings matched by VTE, in terminal-text rule order. An empty
   * element means the corresponding rule did not match at the pointer.
   */
  std::vector<std::optional<std::string>> terminal_text;
};

/**
 * Result of evaluating terminal link candidates against ordered rules.
 */
struct TerminalHyperlinkResolution {
  /** Safe expanded action, or no value when validation rejected the match. */
  std::optional<TerminalHyperlinkAction> action;
  /** Human-readable rejection reason when action has no value. */
  std::string error;
};

/**
 * Opaque compiled terminal link action resolver.
 */
struct TerminalHyperlinkResolverState;

/**
 * Creates a resolver for ordered terminal link rules.
 *
 * @param rules Ordered rules, with the first rule having highest priority.
 * @returns New resolver state owned by the caller.
 */
TerminalHyperlinkResolverState *create_terminal_hyperlink_resolver(
    std::vector<HyperlinkActionRule> rules);

/**
 * Resolves OSC 8 and visible-text candidates into an external command.
 *
 * @param state Resolver created by create_terminal_hyperlink_resolver.
 * @param candidates Candidate values returned by the terminal adapter.
 * @returns First fully matching and validated result, an error result when all
 * matching rules are rejected, or no value when no rule matches.
 */
std::optional<TerminalHyperlinkResolution>
resolve_terminal_hyperlink(TerminalHyperlinkResolverState *state,
                           const TerminalHyperlinkCandidates &candidates);

/**
 * Releases a terminal hyperlink resolver.
 *
 * @param state Resolver state to destroy.
 */
void destroy_terminal_hyperlink_resolver(
    TerminalHyperlinkResolverState *state);

} // namespace elder_terms
