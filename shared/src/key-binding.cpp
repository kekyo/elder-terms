#include <elder-terms/key-binding.h>

#include <algorithm>
#include <string>
#include <vector>

#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace elder_terms {

static constexpr auto supported_modifier_mask =
    static_cast<GdkModifierType>(GDK_SHIFT_MASK | GDK_CONTROL_MASK |
                                 GDK_MOD1_MASK | GDK_SUPER_MASK);

static std::string trim_ascii_whitespace(const std::string &text) {
  const auto first = std::find_if_not(text.begin(), text.end(), [](char value) {
    return g_ascii_isspace(static_cast<guchar>(value)) != FALSE;
  });
  const auto last = std::find_if_not(text.rbegin(), text.rend(), [](char value) {
                      return g_ascii_isspace(static_cast<guchar>(value)) !=
                             FALSE;
                    }).base();
  return first < last ? std::string(first, last) : std::string();
}

static std::string ascii_lower(const std::string &text) {
  std::string output = text;
  std::transform(output.begin(), output.end(), output.begin(), [](char value) {
    return static_cast<char>(g_ascii_tolower(static_cast<guchar>(value)));
  });
  return output;
}

static std::vector<std::string> split_binding_tokens(const std::string &text) {
  std::vector<std::string> tokens;
  std::size_t start = 0;
  for (std::size_t index = 0; index <= text.size(); ++index) {
    if (index != text.size() && text[index] != '+' && text[index] != '-') {
      continue;
    }
    tokens.push_back(trim_ascii_whitespace(text.substr(start, index - start)));
    start = index + 1;
  }
  return tokens;
}

static std::optional<GdkModifierType>
modifier_for_token(const std::string &token) {
  if (token == "ctrl") {
    return GDK_CONTROL_MASK;
  }
  if (token == "shift") {
    return GDK_SHIFT_MASK;
  }
  if (token == "alt") {
    return GDK_MOD1_MASK;
  }
  if (token == "super") {
    return GDK_SUPER_MASK;
  }
  return std::nullopt;
}

static guint keyval_from_case_insensitive_name(const std::string &name) {
  const xkb_keysym_t keysym = xkb_keysym_from_name(
      name.c_str(), XKB_KEYSYM_CASE_INSENSITIVE);
  return keysym == XKB_KEY_NoSymbol ? GDK_KEY_VoidSymbol
                                    : static_cast<guint>(keysym);
}

KeyBindingParseResult parse_key_binding(const std::string &text) {
  const std::string trimmed = trim_ascii_whitespace(text);
  if (trimmed.empty()) {
    return {};
  }

  const std::vector<std::string> tokens = split_binding_tokens(trimmed);
  if (std::any_of(tokens.begin(), tokens.end(),
                  [](const std::string &token) { return token.empty(); })) {
    return {.binding = std::nullopt,
            .error = "contains an empty token"};
  }

  GdkModifierType modifiers = static_cast<GdkModifierType>(0);
  for (std::size_t index = 0; index + 1 < tokens.size(); ++index) {
    const std::string token = ascii_lower(tokens[index]);
    const auto modifier = modifier_for_token(token);
    if (!modifier.has_value()) {
      return {.binding = std::nullopt,
              .error = "contains an unknown modifier: " + tokens[index]};
    }
    if ((modifiers & *modifier) != 0) {
      return {.binding = std::nullopt,
              .error = "contains a duplicate modifier: " + tokens[index]};
    }
    modifiers = static_cast<GdkModifierType>(modifiers | *modifier);
  }

  const std::string key_name = tokens.back();
  if (modifier_for_token(ascii_lower(key_name)).has_value()) {
    return {.binding = std::nullopt, .error = "does not contain a key"};
  }
  const guint keyval = keyval_from_case_insensitive_name(key_name);
  if (keyval == GDK_KEY_VoidSymbol) {
    return {.binding = std::nullopt,
            .error = "contains an unknown key: " + key_name};
  }

  return {
      .binding = KeyBinding{.keyval = keyval, .modifiers = modifiers},
      .error = {},
  };
}

bool global_hotkey_text_is_valid(const std::string &text,
                                 std::string *reason) {
  const KeyBindingParseResult parsed = parse_key_binding(text);
  if (!parsed.error.empty()) {
    if (reason != nullptr) {
      *reason = parsed.error;
    }
    return false;
  }
  if (!parsed.binding.has_value()) {
    return true;
  }
  if (parsed.binding->modifiers == 0) {
    if (reason != nullptr) {
      *reason = "must include Ctrl, Shift, Alt, or Super";
    }
    return false;
  }
  return true;
}

bool key_bindings_equal(const KeyBinding &left, const KeyBinding &right) {
  return left.keyval == right.keyval && left.modifiers == right.modifiers;
}

bool key_binding_matches(const KeyBinding &binding, guint keyval,
                         GdkModifierType modifiers) {
  return gdk_keyval_to_lower(binding.keyval) ==
             gdk_keyval_to_lower(keyval) &&
         binding.modifiers ==
             static_cast<GdkModifierType>(modifiers & supported_modifier_mask);
}

} // namespace elder_terms
