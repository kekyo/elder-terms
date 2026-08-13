#pragma once

#include <string>
#include <vector>

#include <elder-terms/export.h>

namespace elder_terms {

/**
 * Describes one ordered OSC 8 hyperlink command rule.
 */
struct HyperlinkActionRule {
  /** Identifier used by the hyperlink.<id> INI section. */
  std::string id;
  /** GLib regular expression matched against the complete OSC 8 target. */
  std::string pattern;
  /** Fixed executable name or path, without capture expansion. */
  std::string command;
  /** Ordered argument templates, excluding argv[0]. */
  std::vector<std::string> arguments;

  bool operator==(const HyperlinkActionRule &) const = default;
};

/**
 * Returns the built-in VS Code OSC 8 hyperlink actions.
 *
 * @returns Ordered rules for VS Code targets with line and optional column.
 */
ELDER_TERMS_API std::vector<HyperlinkActionRule>
default_hyperlink_action_rules();

/**
 * Validates a hyperlink rule identifier.
 *
 * @param id Identifier to validate.
 * @param reason Receives a human-readable reason when validation fails.
 * @returns True when id contains only ASCII letters, digits, '-' or '_'.
 */
ELDER_TERMS_API bool hyperlink_action_rule_id_is_valid(
    const std::string &id, std::string *reason);

/**
 * Validates one complete hyperlink action rule.
 *
 * @param rule Rule to validate.
 * @param reason Receives a human-readable reason when validation fails.
 * @returns True when the rule can be compiled and its arguments expanded.
 */
ELDER_TERMS_API bool hyperlink_action_rule_is_valid(
    const HyperlinkActionRule &rule, std::string *reason);

} // namespace elder_terms
