// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once
#include "smb-client.h"

namespace elder_terms {

/**
 * Opens an SMB1 server browser through an isolated libdsm worker.
 * @param options Validated direct-connection settings and runtime credentials.
 * @param cancellation Cancels DNS, process startup and authentication.
 * @returns Client rooted at the server's share list.
 */
cardio::promise<std::shared_ptr<RemoteFileClient>> open_smb1_server_async(
    SmbClientOpenOptions options, cardio::cancellation cancellation);

/**
 * Stops and reaps the SMB1 worker after pending I/O has retired.
 * @param client SMB1 client, or another client type which is ignored.
 */
cardio::promise<void> stop_smb1_client_async(std::shared_ptr<RemoteFileClient> client);

} // namespace elder_terms
