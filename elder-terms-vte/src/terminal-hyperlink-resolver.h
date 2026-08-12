#pragma once

#include <optional>
#include <string>
#include <vector>

#include <elder-terms/settings/hyperlink-settings.h>

namespace elder_terms {

/**
 * External command resolved from one OSC 8 hyperlink target.
 */
struct TerminalHyperlinkAction {
  /** Fixed executable name or path from the matching rule. */
  std::string command;
  /** Expanded ordered arguments, excluding argv[0]. */
  std::vector<std::string> arguments;
};

/**
 * Opaque compiled OSC 8 hyperlink action resolver.
 */
struct TerminalHyperlinkResolverState;

/**
 * Creates a resolver for ordered OSC 8 hyperlink rules.
 *
 * @param rules Ordered rules, with the first rule having highest priority.
 * @returns New resolver state owned by the caller.
 */
TerminalHyperlinkResolverState *create_terminal_hyperlink_resolver(
    std::vector<HyperlinkActionRule> rules);

/**
 * Resolves a complete OSC 8 target into an external command.
 *
 * @param state Resolver created by create_terminal_hyperlink_resolver.
 * @param target Raw UTF-8 target returned by VTE.
 * @returns First fully matching action, or no value when none is safe.
 */
std::optional<TerminalHyperlinkAction>
resolve_terminal_hyperlink(TerminalHyperlinkResolverState *state,
                           const std::string &target);

/**
 * Releases a terminal hyperlink resolver.
 *
 * @param state Resolver state to destroy.
 */
void destroy_terminal_hyperlink_resolver(
    TerminalHyperlinkResolverState *state);

} // namespace elder_terms
