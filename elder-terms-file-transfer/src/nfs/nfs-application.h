// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once
#include <elder-terms/settings.h>

namespace elder_terms {
/**
 * Runs the common file browser with an NFS server connection.
 * @param settings Loaded global and connection settings.
 * @param config_path Writable connection file, when supplied at launch.
 * @returns Exit code after all asynchronous operations have stopped.
 */
int run_nfs_application(const SettingsLoadResult &settings, std::optional<std::filesystem::path> config_path);
} // namespace elder_terms
