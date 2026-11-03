// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once

#include <cstdint>
#include <string>

#include <cardio.h>

namespace elder_terms {

/**
 * Resolves and connects a TCP socket without blocking the calling dispatcher.
 *
 * @param io io_uring instance used to create and connect sockets.
 * @param host Hostname or numeric address to resolve.
 * @param port TCP port in host byte order.
 * @param cancellation Cancellation requested by the owning session.
 * @returns Connected close-on-exec socket owned by the caller.
 */
cardio::promise<int>
connect_tcp_socket_async(cardio::io_uring &io, std::string host,
                         std::uint16_t port,
                         cardio::cancellation cancellation);

} // namespace elder_terms
