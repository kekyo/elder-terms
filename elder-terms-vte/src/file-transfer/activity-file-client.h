#pragma once

#include <functional>
#include "remote-file-client.h"
#include "../activity-indicator-id.h"

namespace elder_terms {
/**
 * Wraps a remote client to report requests, replies and transferred chunks.
 * @param client Remote service whose capabilities and ownership are preserved.
 * @param activity Callback executed on the caller dispatcher for SD or RD.
 * @returns Delegating client, including activity-aware readers and writers.
 */
std::shared_ptr<RemoteFileClient> create_activity_file_client(
    std::shared_ptr<RemoteFileClient> client,
    std::function<void(ActivityIndicatorId)> activity);
} // namespace elder_terms
