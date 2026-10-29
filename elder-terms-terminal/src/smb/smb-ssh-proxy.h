// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once

#include "../terminal-sessions/ssh-session/authenticated-ssh-transport.h"

namespace elder_terms {

/** Owns one experimental SMB route through an SSH gateway. */
struct SmbSshProxy;

/**
 * Authenticates one SSH gateway and forwards multiple loopback TCP connections.
 * @param proxy Gateway settings; disabled settings return null.
 * @param host SMB server name, resolved by the gateway.
 * @param port SMB server TCP port.
 * @param callbacks Gateway authentication and host-key prompts.
 * @param options SSH configuration and known-host overrides.
 * @param cancellation Cancels gateway authentication and channel creation.
 * @returns Route retained until stop_smb_ssh_proxy_async joins all channels.
 */
cardio::promise<std::shared_ptr<SmbSshProxy>> open_smb_ssh_proxy_async(
    SshProxySettings proxy, std::string host, std::uint16_t port,
    TerminalSessionCallbacks callbacks, AuthenticatedSshTransportOptions options,
    cardio::cancellation cancellation);

/**
 * Gets the local connection endpoint for libsmb2.
 * @param proxy Open route, retained throughout SMB use.
 * @returns Port bound exclusively to 127.0.0.1; zero for a disabled route.
 */
std::uint16_t smb_ssh_proxy_port(const std::shared_ptr<SmbSshProxy> &proxy);

/**
 * Cancels and joins forwarding before releasing sockets and the gateway.
 * @param proxy Route; null and repeated calls are accepted.
 */
cardio::promise<void> stop_smb_ssh_proxy_async(std::shared_ptr<SmbSshProxy> proxy);

} // namespace elder_terms
