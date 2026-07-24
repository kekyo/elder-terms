#pragma once

#include <string>

#include <elder-terms/settings.h>

namespace elder_terms {

/**
 * Resolves the native local directory initially shown by an SFTP window.
 *
 * @param store Effective application settings.
 * @param settings Effective SFTP connection settings.
 * @returns Configured SFTP directory, transfer base path, or Downloads
 * fallback.
 */
std::string resolve_sftp_local_directory(
    const SettingsStore &store,
    const SftpConnectionSettings &settings);

} // namespace elder_terms
