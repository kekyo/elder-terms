#pragma once

#include "authenticated-ssh-transport.h"

namespace elder_terms {

/** Owned connected socket and the asynchronous route that transports its bytes. */
struct SshProxyConnection;

/**
 * Connects to a destination through a gateway, or directly when disabled.
 * @param proxy Gateway configuration; invalid values always fail before connecting.
 * @param host Final destination hostname, resolved by the gateway when enabled.
 * @param port Final destination port.
 * @param callbacks Gateway authentication prompts and connection notifications.
 * @param options SSH configuration and known_hosts overrides.
 * @param cancellation Connection cancellation signal.
 * @returns Connection retaining its socket and gateway until explicitly stopped or released.
 */
cardio::promise<std::shared_ptr<SshProxyConnection>> connect_ssh_proxy_async(
    SshProxySettings proxy, std::string host, std::uint16_t port,
    TerminalSessionCallbacks callbacks, AuthenticatedSshTransportOptions options,
    cardio::cancellation cancellation);

/**
 * Borrows the connection's descriptor.
 * @param connection Owning connection, retained while the descriptor is in use.
 * @returns Connected descriptor; callers must not close it.
 */
int ssh_proxy_connection_fd(const std::shared_ptr<SshProxyConnection> &connection);

/**
 * Cancels and joins the route's asynchronous operations before releasing resources.
 * @param connection Connection to stop; null and repeated calls are accepted.
 */
cardio::promise<void> stop_ssh_proxy_connection_async(std::shared_ptr<SshProxyConnection> connection);

} // namespace elder_terms
