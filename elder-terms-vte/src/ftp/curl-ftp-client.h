#pragma once

#include "ftp-client.h"

namespace elder_terms {

/**
 * Opens the libcurl implementation used by the migration driver.
 * @param options Endpoint settings and runtime credentials.
 * @param cancellation Cancellation for initialization.
 * @returns Authenticated remote file client.
 */
cardio::promise<std::shared_ptr<RemoteFileClient>>
open_libcurl_ftp_client_async(FtpClientOpenOptions options,
                               cardio::cancellation cancellation);

/**
 * Stops a libcurl FTP client before its caller's dispatcher is destroyed.
 * @param client Client returned by open_libcurl_ftp_client_async().
 * @returns Completion after all worker resources have been released.
 */
cardio::promise<void>
stop_libcurl_ftp_client_async(std::shared_ptr<RemoteFileClient> client);

} // namespace elder_terms
