#pragma once

#include <string>
#include <variant>
#include <vector>

#include <elder-terms/export.h>

namespace elder_terms {

/**
 * Sends expanded text to the active terminal session.
 */
struct MacroSendAction {
  /** Text template expanded from the regular-expression match. */
  std::string text;

  bool operator==(const MacroSendAction &) const = default;
};

/**
 * Spawns a process after expanding its argument templates.
 */
struct MacroCommandAction {
  /** Executable name or path. */
  std::string command;
  /** Ordered argument templates, excluding argv[0]. */
  std::vector<std::string> arguments;

  bool operator==(const MacroCommandAction &) const = default;
};

/**
 * Action performed by a matching macro rule.
 */
using MacroAction = std::variant<MacroSendAction, MacroCommandAction>;

/**
 * Describes one ordered connection macro rule.
 */
struct MacroRule {
  /** Identifier used by the macro.<id> INI section. */
  std::string id;
  /** GLib regular expression matched against one received line. */
  std::string pattern;
  /** Action performed after a successful match. */
  MacroAction action = MacroSendAction{};

  bool operator==(const MacroRule &) const = default;
};

/**
 * Validates a macro identifier.
 *
 * @param id Identifier to validate.
 * @param reason Receives a human-readable reason when validation fails.
 * @returns True when id contains only ASCII letters, digits, '-' or '_'.
 */
ELDER_TERMS_API bool macro_rule_id_is_valid(const std::string &id,
                                            std::string *reason);

/**
 * Validates a complete macro rule, including capture placeholders.
 *
 * @param rule Rule to validate.
 * @param reason Receives a human-readable reason when validation fails.
 * @returns True when the rule can be compiled and executed.
 */
ELDER_TERMS_API bool macro_rule_is_valid(const MacroRule &rule,
                                         std::string *reason);

} // namespace elder_terms
