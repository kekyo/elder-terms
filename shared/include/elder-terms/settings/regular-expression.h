#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <elder-terms/export.h>

namespace elder_terms {

/**
 * Opaque compiled PCRE2 regular expression.
 */
struct RegularExpressionState;

/**
 * Project-owned result of one regular-expression match.
 */
struct RegularExpressionMatch {
  /** UTF-8 byte offset at which the complete match starts. */
  std::size_t start = 0;
  /** UTF-8 byte offset immediately after the complete match. */
  std::size_t end = 0;
  /** Captures indexed by their numeric group, including group zero. */
  std::vector<std::string> captures;
  /** Mapping from capture names to numeric capture groups. */
  std::map<std::string, std::size_t> named_captures;
};

/**
 * Compiles a UTF-8 PCRE2 regular expression.
 *
 * @param pattern Pattern to compile.
 * @param multiline Enables multiline handling for `^` and `$`.
 * @param reason Receives a human-readable reason when compilation fails.
 * @returns Opaque compiled expression, or nullptr when invalid.
 */
ELDER_TERMS_API RegularExpressionState *create_regular_expression(
    const std::string &pattern, bool multiline, std::string *reason);

/**
 * Reports whether a numbered or named capture exists in an expression.
 *
 * @param state Compiled expression.
 * @param capture Decimal capture number or capture name.
 * @returns True when the capture is defined by the expression.
 */
ELDER_TERMS_API bool regular_expression_capture_exists(
    const RegularExpressionState *state, const std::string &capture);

/**
 * Searches a UTF-8 string for the first regular-expression match.
 *
 * @param state Compiled expression.
 * @param subject UTF-8 subject to search.
 * @param reason Receives a human-readable reason for matching errors.
 * @returns Project-owned match data, or no value when no match is found or an
 * error occurs.
 */
ELDER_TERMS_API std::optional<RegularExpressionMatch>
search_regular_expression(const RegularExpressionState *state,
                          const std::string &subject, std::string *reason);

/**
 * Fetches a numbered or named capture from a match.
 *
 * @param match Match returned by search_regular_expression().
 * @param capture Decimal capture number or capture name.
 * @returns Captured text, or no value when the capture is unknown.
 */
ELDER_TERMS_API std::optional<std::string>
regular_expression_capture(const RegularExpressionMatch &match,
                           const std::string &capture);

/**
 * Releases a compiled regular expression.
 *
 * @param state Expression returned by create_regular_expression().
 */
ELDER_TERMS_API void
destroy_regular_expression(RegularExpressionState *state);

} // namespace elder_terms
