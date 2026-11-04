// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once
#include <cstddef>

namespace elder_terms {

/** Identifies the part of a macro or link rule requiring correction. */
enum class RuleValidationField {
  /** Rule identifier. */
  identifier,
  /** Recognition regular expression. */
  pattern,
  /** Executable or command template. */
  command,
  /** One argument template. */
  argument,
  /** Text sent to the terminal. */
  send_text,
  /** Local path template used by link validation. */
  path_template,
};

/** Location of the first validation failure in a rule. */
struct RuleValidationLocation {
  /** Editable field. Meaningful only when validation fails. */
  RuleValidationField field = RuleValidationField::identifier;
  /** Zero-based argument index, used only for the argument field. */
  std::size_t argument_index = 0;
};

} // namespace elder_terms
