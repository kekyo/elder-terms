#pragma once
#include <elder-terms/settings.h>

namespace elder_terms {
/**
 * Runs the shared file browser with a WebDAV connection.
 * @param settings Loaded connection settings.
 * @returns Process exit code after all asynchronous work has stopped.
 */
int run_webdav_application(const SettingsLoadResult &settings);
} // namespace elder_terms
