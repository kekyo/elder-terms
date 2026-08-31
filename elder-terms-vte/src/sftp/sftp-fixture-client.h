#pragma once

#include <memory>

#include "sftp-client.h"

namespace elder_terms {

/**
 * Creates the deterministic in-memory SFTP backend used by GTK tests.
 *
 * @param pause_writes True to keep remote writes pending until cancellation.
 * @returns Initialized fixture client rooted at /remote.
 */
std::shared_ptr<RemoteFileClient>
create_sftp_fixture_client(bool pause_writes);

} // namespace elder_terms
