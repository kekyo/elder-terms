#pragma once

#include "authenticated-ssh-transport.h"

namespace elder_terms {

/** Owned connected socket and the asynchronous route that transports its bytes. */
struct SshProxyConnection;

/**
 * Authenticates one gateway without recursively applying a proxy.
 * @param proxy Enabled gateway configuration.
 * @param callbacks Gateway prompts, decorated with the SSH proxy label.
 * @param options SSH path overrides.
 * @param cancellation Connection cancellation signal.
 * @returns Shared authenticated gateway.
 */
cardio::promise<std::shared_ptr<AuthenticatedSshTransport>> connect_ssh_gateway_async(
    SshProxySettings proxy, TerminalSessionCallbacks callbacks,
    AuthenticatedSshTransportOptions options, cardio::cancellation cancellation);

/**
 * Bridges a connected socket to an already opened forwarding channel.
 * @param channel Channel owned until both stream directions stop.
 * @param fd Borrowed socket; retain it until this operation finishes.
 * @param cancellation Cancels both directions and joins their work.
 */
cardio::promise<void> bridge_ssh_channel_async(
    std::unique_ptr<SshChannelConnection> channel, int fd, cardio::cancellation cancellation);

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
