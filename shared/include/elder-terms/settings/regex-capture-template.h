#pragma once

#include <optional>
#include <string>

#include <elder-terms/export.h>
#include <elder-terms/settings/regular-expression.h>

namespace elder_terms {

/**
 * Selects transformations accepted by a regular-expression capture template.
 */
struct RegexCaptureTemplateOptions {
  /** Allows the `uri-decode` transformation on captured values. */
  bool allow_uri_decode = false;
};

/**
 * Validates capture placeholders in a template.
 *
 * @param value Template containing `${0}`, `${name}`, and `$$` references.
 * @param regex Compiled regular expression defining available captures.
 * @param options Transformations accepted in the template.
 * @param reason Receives a human-readable reason when validation fails.
 * @returns True when every placeholder is valid for regex.
 */
ELDER_TERMS_API bool regex_capture_template_is_valid(
    const std::string &value, const RegularExpressionState *regex,
    RegexCaptureTemplateOptions options, std::string *reason);

/**
 * Expands a validated capture template from one regular-expression match.
 *
 * @param value Template containing capture placeholders.
 * @param match Successful project-owned regular-expression match.
 * @param options Transformations accepted in the template.
 * @param reason Receives a human-readable reason when expansion fails.
 * @returns Expanded value, or no value for invalid syntax or transformation.
 *
 * @remarks `uri-decode` performs URI percent decoding and rejects malformed
 * escapes and escaped NUL bytes. It does not interpret `+` as a space.
 */
ELDER_TERMS_API std::optional<std::string>
expand_regex_capture_template(const std::string &value,
                              const RegularExpressionMatch *match,
                              RegexCaptureTemplateOptions options,
                              std::string *reason);

} // namespace elder_terms
