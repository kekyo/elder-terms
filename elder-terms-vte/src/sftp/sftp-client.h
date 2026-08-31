#pragma once

#include <memory>

#include <cardio.h>

#include "../file-transfer/remote-file-client.h"

namespace elder_terms {

class AuthenticatedSshTransport;

/**
 * Opens an SFTP subsystem over an already authenticated SSH transport.
 *
 * @param transport Shared authenticated SSH transport.
 * @param cancellation Operation cancellation signal.
 * @returns Initialized SFTP client.
 */
cardio::promise<std::shared_ptr<RemoteFileClient>>
open_sftp_client_async(
    std::shared_ptr<AuthenticatedSshTransport> transport,
    cardio::cancellation cancellation);

} // namespace elder_terms
