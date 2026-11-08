// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once
#include <elder-terms/settings.h>

namespace elder_terms {
/**
 * Runs the shared file browser with a SMB connection.
 * @param settings Loaded connection settings.
 * @param config_path Writable connection file, if supplied at launch.
 * @param ssh_known_hosts_file SSH test override; empty uses the user's known hosts.
 * @returns Process exit code after all asynchronous work has stopped.
 */
int run_smb_application(const SettingsLoadResult &settings,
    std::optional<std::filesystem::path> config_path, std::string ssh_known_hosts_file);
} // namespace elder_terms
