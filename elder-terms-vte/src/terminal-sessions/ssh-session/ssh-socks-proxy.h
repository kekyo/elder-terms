#pragma once

#include "authenticated-ssh-transport.h"

namespace elder_terms {

/** Private SOCKS5 endpoint whose channels share one authenticated SSH gateway. */
struct SshSocksProxy;

/**
 * Opens a private Unix socket for a logical file-transfer connection.
 * @param proxy Validated gateway settings. Disabled settings return null.
 * @param destination Only this destination hostname may be forwarded; ports may vary.
 * @param callbacks Gateway host-key and authentication prompts.
 * @param options SSH path overrides.
 * @param cancellation Connection cancellation signal.
 * @returns Owned adapter. Retain until libcurl has stopped using it.
 */
cardio::promise<std::shared_ptr<SshSocksProxy>> open_ssh_socks_proxy_async(
    SshProxySettings proxy, std::string destination, TerminalSessionCallbacks callbacks,
    AuthenticatedSshTransportOptions options, cardio::cancellation cancellation);

/** @param proxy Adapter or null for direct access. @returns socks5h URL, or empty for direct access. */
std::string ssh_socks_proxy_url(const std::shared_ptr<SshSocksProxy> &proxy);

/**
 * Stops accepting connections and joins all forwarding operations.
 * @param proxy Adapter; null and repeated calls are accepted.
 */
cardio::promise<void> stop_ssh_socks_proxy_async(std::shared_ptr<SshSocksProxy> proxy);

} // namespace elder_terms
