#include <elder-terms/settings/regex-capture-template.h>

#include <optional>
#include <string>

#include <glib.h>

namespace elder_terms {

struct CapturePlaceholder {
  std::string capture;
  bool uri_decode = false;
};

static std::optional<CapturePlaceholder> parse_capture_placeholder(
    const std::string &placeholder, RegexCaptureTemplateOptions options,
    std::string *reason) {
  if (placeholder.empty()) {
    *reason = "contains an empty capture placeholder";
    return std::nullopt;
  }

  const std::size_t separator = placeholder.find('|');
  CapturePlaceholder result{
      .capture = placeholder.substr(0, separator),
      .uri_decode = false,
  };
  if (result.capture.empty()) {
    *reason = "contains an empty capture placeholder";
    return std::nullopt;
  }
  if (separator == std::string::npos) {
    return result;
  }
  if (placeholder.find('|', separator + 1) != std::string::npos) {
    *reason = "contains more than one capture transformation";
    return std::nullopt;
  }

  const std::string transform = placeholder.substr(separator + 1);
  if (transform != "uri-decode") {
    *reason = "references an unknown capture transformation: " + transform;
    return std::nullopt;
  }
  if (!options.allow_uri_decode) {
    *reason = "capture transformation is not allowed: " + transform;
    return std::nullopt;
  }
  result.uri_decode = true;
  return result;
}

bool regex_capture_template_is_valid(
    const std::string &value, const RegularExpressionState *regex,
    RegexCaptureTemplateOptions options, std::string *reason) {
  if (regex == nullptr) {
    *reason = "regular expression is unavailable";
    return false;
  }

  for (std::size_t index = 0; index < value.size();) {
    if (value[index] != '$') {
      ++index;
      continue;
    }
    if (index + 1 < value.size() && value[index + 1] == '$') {
      index += 2;
      continue;
    }
    if (index + 1 >= value.size() || value[index + 1] != '{') {
      *reason = "literal '$' must be written as '$$'";
      return false;
    }

    const std::size_t closing = value.find('}', index + 2);
    if (closing == std::string::npos) {
      *reason = "contains an unmatched capture placeholder";
      return false;
    }
    const std::optional<CapturePlaceholder> placeholder =
        parse_capture_placeholder(
            value.substr(index + 2, closing - index - 2), options, reason);
    if (!placeholder.has_value()) {
      return false;
    }
    if (!regular_expression_capture_exists(regex, placeholder->capture)) {
      *reason = "references an unknown capture: " + placeholder->capture;
      return false;
    }
    index = closing + 1;
  }
  return true;
}

static std::optional<std::string>
fetch_capture(const CapturePlaceholder &placeholder,
              const RegularExpressionMatch *match, std::string *reason) {
  const std::optional<std::string> matched =
      regular_expression_capture(*match, placeholder.capture);
  if (!matched.has_value()) {
    *reason = "failed to read capture: " + placeholder.capture;
    return std::nullopt;
  }

  std::string result;
  if (placeholder.uri_decode) {
    gchar *decoded = g_uri_unescape_string(matched->c_str(), nullptr);
    if (decoded == nullptr) {
      *reason = "capture contains invalid URI escaping: " +
                placeholder.capture;
      return std::nullopt;
    }
    result = decoded;
    g_free(decoded);
    return result;
  }

  result = *matched;
  return result;
}

std::optional<std::string>
expand_regex_capture_template(const std::string &value,
                              const RegularExpressionMatch *match,
                              RegexCaptureTemplateOptions options,
                              std::string *reason) {
  if (match == nullptr) {
    *reason = "regular-expression match is unavailable";
    return std::nullopt;
  }

  std::string expanded;
  for (std::size_t index = 0; index < value.size();) {
    if (value[index] != '$') {
      expanded.push_back(value[index]);
      ++index;
      continue;
    }
    if (index + 1 < value.size() && value[index + 1] == '$') {
      expanded.push_back('$');
      index += 2;
      continue;
    }
    if (index + 1 >= value.size() || value[index + 1] != '{') {
      *reason = "literal '$' must be written as '$$'";
      return std::nullopt;
    }

    const std::size_t closing = value.find('}', index + 2);
    if (closing == std::string::npos) {
      *reason = "contains an unmatched capture placeholder";
      return std::nullopt;
    }
    const std::optional<CapturePlaceholder> placeholder =
        parse_capture_placeholder(
            value.substr(index + 2, closing - index - 2), options, reason);
    if (!placeholder.has_value()) {
      return std::nullopt;
    }
    const std::optional<std::string> captured =
        fetch_capture(*placeholder, match, reason);
    if (!captured.has_value()) {
      return std::nullopt;
    }
    expanded.append(*captured);
    index = closing + 1;
  }
  return expanded;
}

} // namespace elder_terms
