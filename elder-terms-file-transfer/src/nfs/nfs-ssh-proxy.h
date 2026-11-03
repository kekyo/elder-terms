// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once

#include "nfs-client.h"
#include "../../../shared/src/connection-runtime/ssh/ssh-tcp-proxy.h"

namespace elder_terms {

/**
 * Resolves and forwards NFS service ports through an authenticated gateway.
 * @param proxy Shared SSH TCP proxy, retained until after the NFS client stops.
 * @param options Original server endpoint, protocol and authentication settings.
 * @param cancellation Cancels service discovery and endpoint creation.
 * @returns Options using loopback endpoints with explicit service ports.
 * @throws RemoteFileError If reserved source ports are required or discovery fails.
 * @remarks NFS v4 uses its configured port or 2049. NFS v2/v3 discover only
 * unspecified ports through the gateway's PORTMAP service. No direct fallback
 * or local resolution of the destination hostname is performed.
 */
cardio::promise<NfsClientOpenOptions> forward_nfs_ssh_ports_async(
    std::shared_ptr<SshTcpProxy> proxy, NfsClientOpenOptions options, cardio::cancellation cancellation);

} // namespace elder_terms
