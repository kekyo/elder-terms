#include <elder-terms/settings/regular-expression.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <vector>

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

namespace elder_terms {

static constexpr uint32_t regular_expression_match_limit = 1000000;
static constexpr uint32_t regular_expression_depth_limit = 1000;

struct RegularExpressionState {
  pcre2_code *code = nullptr;
  pcre2_match_context *match_context = nullptr;
  std::size_t capture_count = 0;
  std::map<std::string, std::size_t> named_captures;
};

static std::optional<std::size_t>
decimal_capture_number(const std::string &capture) {
  if (capture.empty() ||
      !std::all_of(capture.begin(), capture.end(), [](unsigned char value) {
        return std::isdigit(value) != 0;
      })) {
    return std::nullopt;
  }

  std::size_t number = 0;
  for (unsigned char value : capture) {
    const std::size_t digit = static_cast<std::size_t>(value - '0');
    if (number >
        (std::numeric_limits<std::size_t>::max() - digit) / 10) {
      return std::nullopt;
    }
    number = number * 10 + digit;
  }
  return number;
}

static std::string pcre2_error_message(int error_code) {
  PCRE2_UCHAR buffer[256]{};
  const int length =
      pcre2_get_error_message(error_code, buffer, sizeof(buffer));
  if (length < 0) {
    return "PCRE2 error " + std::to_string(error_code);
  }
  return std::string(reinterpret_cast<const char *>(buffer),
                     static_cast<std::size_t>(length));
}

static bool load_expression_metadata(RegularExpressionState *state,
                                     std::string *reason) {
  uint32_t capture_count = 0;
  if (pcre2_pattern_info(state->code, PCRE2_INFO_CAPTURECOUNT,
                         &capture_count) != 0) {
    *reason = "failed to read regular-expression capture count";
    return false;
  }
  state->capture_count = capture_count;

  uint32_t name_count = 0;
  uint32_t name_entry_size = 0;
  PCRE2_SPTR name_table = nullptr;
  if (pcre2_pattern_info(state->code, PCRE2_INFO_NAMECOUNT, &name_count) !=
          0 ||
      pcre2_pattern_info(state->code, PCRE2_INFO_NAMEENTRYSIZE,
                         &name_entry_size) != 0 ||
      pcre2_pattern_info(state->code, PCRE2_INFO_NAMETABLE, &name_table) !=
          0) {
    *reason = "failed to read regular-expression capture names";
    return false;
  }

  for (uint32_t index = 0; index < name_count; ++index) {
    const PCRE2_SPTR entry = name_table + index * name_entry_size;
    const std::size_t number =
        (static_cast<std::size_t>(entry[0]) << 8) |
        static_cast<std::size_t>(entry[1]);
    state->named_captures.emplace(
        reinterpret_cast<const char *>(entry + 2), number);
  }
  return true;
}

RegularExpressionState *create_regular_expression(
    const std::string &pattern, bool multiline, std::string *reason) {
  if (reason != nullptr) {
    reason->clear();
  }

  pcre2_compile_context *compile_context =
      pcre2_compile_context_create(nullptr);
  if (compile_context == nullptr) {
    if (reason != nullptr) {
      *reason = "failed to allocate regular-expression compile context";
    }
    return nullptr;
  }
  if (pcre2_set_newline(compile_context, PCRE2_NEWLINE_LF) != 0) {
    pcre2_compile_context_free(compile_context);
    if (reason != nullptr) {
      *reason = "failed to configure regular-expression newline handling";
    }
    return nullptr;
  }

  int error_code = 0;
  PCRE2_SIZE error_offset = 0;
  uint32_t options = PCRE2_UTF | PCRE2_UCP | PCRE2_NEVER_BACKSLASH_C;
  if (multiline) {
    options |= PCRE2_MULTILINE;
  }
  pcre2_code *code = pcre2_compile(
      reinterpret_cast<PCRE2_SPTR>(pattern.data()), pattern.size(), options,
      &error_code, &error_offset, compile_context);
  pcre2_compile_context_free(compile_context);
  if (code == nullptr) {
    if (reason != nullptr) {
      *reason = "at byte " + std::to_string(error_offset) + ": " +
                pcre2_error_message(error_code);
    }
    return nullptr;
  }

  pcre2_match_context *match_context = pcre2_match_context_create(nullptr);
  if (match_context == nullptr) {
    pcre2_code_free(code);
    if (reason != nullptr) {
      *reason = "failed to allocate regular-expression match context";
    }
    return nullptr;
  }
  if (pcre2_set_match_limit(match_context,
                            regular_expression_match_limit) != 0 ||
      pcre2_set_depth_limit(match_context,
                            regular_expression_depth_limit) != 0) {
    pcre2_match_context_free(match_context);
    pcre2_code_free(code);
    if (reason != nullptr) {
      *reason = "failed to configure regular-expression resource limits";
    }
    return nullptr;
  }

  auto *state = new RegularExpressionState{
      .code = code,
      .match_context = match_context,
      .capture_count = 0,
      .named_captures = {},
  };
  std::string metadata_reason;
  if (!load_expression_metadata(state, &metadata_reason)) {
    destroy_regular_expression(state);
    if (reason != nullptr) {
      *reason = metadata_reason;
    }
    return nullptr;
  }
  return state;
}

bool regular_expression_capture_exists(
    const RegularExpressionState *state, const std::string &capture) {
  if (state == nullptr) {
    return false;
  }
  const std::optional<std::size_t> number =
      decimal_capture_number(capture);
  if (number.has_value()) {
    return *number <= state->capture_count;
  }
  return state->named_captures.contains(capture);
}

std::optional<RegularExpressionMatch>
search_regular_expression(const RegularExpressionState *state,
                          const std::string &subject, std::string *reason) {
  if (reason != nullptr) {
    reason->clear();
  }
  if (state == nullptr) {
    if (reason != nullptr) {
      *reason = "regular expression is unavailable";
    }
    return std::nullopt;
  }

  pcre2_match_data *match_data =
      pcre2_match_data_create_from_pattern(state->code, nullptr);
  if (match_data == nullptr) {
    if (reason != nullptr) {
      *reason = "failed to allocate regular-expression match data";
    }
    return std::nullopt;
  }
  const int result = pcre2_match(
      state->code, reinterpret_cast<PCRE2_SPTR>(subject.data()),
      subject.size(), 0, 0, match_data, state->match_context);
  if (result < 0) {
    pcre2_match_data_free(match_data);
    if (result != PCRE2_ERROR_NOMATCH && reason != nullptr) {
      *reason = pcre2_error_message(result);
    }
    return std::nullopt;
  }

  PCRE2_SIZE *offsets = pcre2_get_ovector_pointer(match_data);
  RegularExpressionMatch match{
      .start = static_cast<std::size_t>(offsets[0]),
      .end = static_cast<std::size_t>(offsets[1]),
      .captures = {},
      .named_captures = state->named_captures,
  };
  match.captures.reserve(state->capture_count + 1);
  for (std::size_t index = 0; index <= state->capture_count; ++index) {
    const PCRE2_SIZE start = offsets[index * 2];
    const PCRE2_SIZE end = offsets[index * 2 + 1];
    if (start == PCRE2_UNSET || end == PCRE2_UNSET) {
      match.captures.emplace_back();
    } else {
      match.captures.push_back(subject.substr(
          static_cast<std::size_t>(start),
          static_cast<std::size_t>(end - start)));
    }
  }
  pcre2_match_data_free(match_data);
  return match;
}

std::optional<std::string>
regular_expression_capture(const RegularExpressionMatch &match,
                           const std::string &capture) {
  std::optional<std::size_t> number = decimal_capture_number(capture);
  if (!number.has_value()) {
    const auto named = match.named_captures.find(capture);
    if (named == match.named_captures.end()) {
      return std::nullopt;
    }
    number = named->second;
  }
  if (*number >= match.captures.size()) {
    return std::nullopt;
  }
  return match.captures[*number];
}

void destroy_regular_expression(RegularExpressionState *state) {
  if (state == nullptr) {
    return;
  }
  pcre2_match_context_free(state->match_context);
  pcre2_code_free(state->code);
  delete state;
}

} // namespace elder_terms
