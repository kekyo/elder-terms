#pragma once

#include <string>

#include <elder-terms/settings.h>

namespace elder_terms {

/**
 * Resolves the native local directory initially shown by a file-transfer
 * window.
 *
 * @param store Effective application settings.
 * @param configured_directory Protocol-specific configured local directory.
 * @returns Configured directory, transfer base path, or Downloads fallback.
 */
std::string resolve_file_transfer_local_directory(
    const SettingsStore &store, const std::string &configured_directory);

} // namespace elder_terms
