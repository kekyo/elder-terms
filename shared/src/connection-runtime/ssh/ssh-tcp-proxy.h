// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once

#include "authenticated-ssh-transport.h"

namespace elder_terms {

/** Owns one authenticated SSH gateway and its loopback TCP forwarding endpoints. */
struct SshTcpProxy;

/**
 * Authenticates a gateway shared by any number of TCP forwarding endpoints.
 * @param proxy Gateway settings; disabled settings return null.
 * @param callbacks Gateway authentication and host-key prompts.
 * @param options SSH configuration and known-host overrides.
 * @param cancellation Cancels gateway authentication.
 * @returns Proxy retained until stop_ssh_tcp_proxy_async joins all channels.
 */
cardio::promise<std::shared_ptr<SshTcpProxy>> open_ssh_tcp_proxy_async(
    SshProxySettings proxy, TerminalSessionCallbacks callbacks,
    AuthenticatedSshTransportOptions options, cardio::cancellation cancellation);

/**
 * Forwards multiple local connections to one destination through the gateway.
 * @param proxy Open gateway, retained throughout client use.
 * @param host Destination hostname, resolved by the gateway.
 * @param port Destination TCP port.
 * @param cancellation Cancels creation of the endpoint and its first channel.
 * @returns Port bound exclusively to 127.0.0.1; repeated destinations reuse it.
 * @remarks Endpoint creation is serialized with other creations and shutdown.
 * Cancellation after creation does not stop the endpoint; stop the proxy instead.
 */
cardio::promise<std::uint16_t> forward_ssh_tcp_port_async(std::shared_ptr<SshTcpProxy> proxy,
    std::string host, std::uint16_t port, cardio::cancellation cancellation);

/**
 * Cancels and joins all forwarding before releasing sockets and the gateway.
 * @param proxy Proxy; null and repeated calls are accepted.
 */
cardio::promise<void> stop_ssh_tcp_proxy_async(std::shared_ptr<SshTcpProxy> proxy);

} // namespace elder_terms
