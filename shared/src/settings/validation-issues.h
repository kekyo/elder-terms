// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once
#include <elder-terms/settings/settings-store.h>
#include <utility>

namespace elder_terms {

/**
 * Associates one existing validation failure with its editable settings.
 * @tparam Settings Connection settings exposing validation_errors and validation_issues.
 * @param settings Result receiving both the diagnostic and field association.
 * @param fields Settings that participate in the failure.
 * @param message Human-readable reason.
 */
template<typename Settings>
void add_validation_issue(Settings &settings, std::vector<SettingKey> fields, std::string message) {
  settings.validation_errors.push_back(message);
  settings.validation_issues.push_back({std::move(fields), std::move(message)});
}

} // namespace elder_terms
