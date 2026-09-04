#pragma once

#include <optional>
#include <string>
#include <vector>

#include <elder-terms/export.h>

namespace elder_terms {

/**
 * Selects the terminal data used to recognize a link.
 */
enum class HyperlinkRecognitionSource {
  /** Matches the complete target supplied by an OSC 8 hyperlink. */
  osc8,
  /** Searches visible terminal text for a matching substring. */
  terminal_text,
};

/**
 * Selects validation applied to a path expanded from a link match.
 */
enum class HyperlinkPathValidation {
  /** Does not interpret or validate an expanded path. */
  none,
  /** Requires an absolute path naming an existing file or directory. */
  existing_local_path,
};

/**
 * Describes one ordered link recognition and command rule.
 */
struct HyperlinkActionRule {
  /** Identifier used by the hyperlink.<id> INI section. */
  std::string id;
  /** Terminal data against which the regular expression is matched. */
  HyperlinkRecognitionSource recognition_source =
      HyperlinkRecognitionSource::osc8;
  /** PCRE2 regular expression used to recognize the link. */
  std::string pattern;
  /** Fixed executable name or path, without capture expansion. */
  std::string command;
  /** Ordered argument templates, excluding argv[0]. */
  std::vector<std::string> arguments;
  /** Validation applied to path_template after capture expansion. */
  HyperlinkPathValidation path_validation =
      HyperlinkPathValidation::none;
  /** Capture template expanded to the local path that is validated. */
  std::string path_template = {};

  bool operator==(const HyperlinkActionRule &) const = default;
};

/**
 * Returns the built-in URL and VS Code link actions.
 *
 * @returns Ordered URL rules followed by VS Code OSC 8 target rules.
 */
ELDER_TERMS_API std::vector<HyperlinkActionRule>
default_hyperlink_action_rules();

/**
 * Returns the INI value for a link recognition source.
 *
 * @param source Recognition source.
 * @returns Stable INI value.
 */
ELDER_TERMS_API const char *hyperlink_recognition_source_to_string(
    HyperlinkRecognitionSource source);

/**
 * Parses a link recognition source from an INI value.
 *
 * @param value INI value.
 * @returns Parsed source, or no value when unsupported.
 */
ELDER_TERMS_API std::optional<HyperlinkRecognitionSource>
hyperlink_recognition_source_from_string(const std::string &value);

/**
 * Returns the INI value for a link path validation mode.
 *
 * @param validation Validation mode.
 * @returns Stable INI value.
 */
ELDER_TERMS_API const char *hyperlink_path_validation_to_string(
    HyperlinkPathValidation validation);

/**
 * Parses a link path validation mode from an INI value.
 *
 * @param value INI value.
 * @returns Parsed mode, or no value when unsupported.
 */
ELDER_TERMS_API std::optional<HyperlinkPathValidation>
hyperlink_path_validation_from_string(const std::string &value);

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
