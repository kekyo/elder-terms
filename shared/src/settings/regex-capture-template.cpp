#include <elder-terms/settings/regex-capture-template.h>

#include <algorithm>
#include <cctype>
#include <limits>
#include <optional>
#include <string>

namespace elder_terms {

struct CapturePlaceholder {
  std::string capture;
  bool uri_decode = false;
};

static std::optional<int>
decimal_capture_number(const std::string &capture) {
  if (capture.empty() ||
      !std::all_of(capture.begin(), capture.end(), [](unsigned char value) {
        return std::isdigit(value) != 0;
      })) {
    return std::nullopt;
  }

  unsigned int number = 0;
  for (unsigned char value : capture) {
    const unsigned int digit = static_cast<unsigned int>(value - '0');
    if (number >
        (static_cast<unsigned int>(std::numeric_limits<int>::max()) - digit) /
            10) {
      return std::nullopt;
    }
    number = number * 10 + digit;
  }
  return static_cast<int>(number);
}

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

static bool capture_exists(const CapturePlaceholder &placeholder,
                           GRegex *regex) {
  const std::optional<int> number =
      decimal_capture_number(placeholder.capture);
  if (number.has_value()) {
    return *number <= g_regex_get_capture_count(regex);
  }
  return g_regex_get_string_number(regex, placeholder.capture.c_str()) >= 0;
}

bool regex_capture_template_is_valid(
    const std::string &value, GRegex *regex,
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
    if (!capture_exists(*placeholder, regex)) {
      *reason = "references an unknown capture: " + placeholder->capture;
      return false;
    }
    index = closing + 1;
  }
  return true;
}

static std::optional<std::string>
fetch_capture(const CapturePlaceholder &placeholder, GMatchInfo *match,
              std::string *reason) {
  const std::optional<int> number =
      decimal_capture_number(placeholder.capture);
  gchar *matched = number.has_value()
                       ? g_match_info_fetch(match, *number)
                       : g_match_info_fetch_named(
                             match, placeholder.capture.c_str());
  if (matched == nullptr) {
    *reason = "failed to read capture: " + placeholder.capture;
    return std::nullopt;
  }

  std::string result;
  if (placeholder.uri_decode) {
    gchar *decoded = g_uri_unescape_string(matched, nullptr);
    g_free(matched);
    if (decoded == nullptr) {
      *reason = "capture contains invalid URI escaping: " +
                placeholder.capture;
      return std::nullopt;
    }
    result = decoded;
    g_free(decoded);
    return result;
  }

  result = matched;
  g_free(matched);
  return result;
}

std::optional<std::string>
expand_regex_capture_template(const std::string &value, GMatchInfo *match,
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
