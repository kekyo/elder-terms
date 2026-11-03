// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once

#include "terminal-transport.h"
#include "../../shared/src/connection-runtime/ssh/authenticated-ssh-transport.h"

namespace elder_terms {

/**
 * Creates a TELNET channel factory with protocol negotiation and NAWS.
 * @param settings Destination and reported terminal type.
 * @param encoding Peer character encoding.
 * @param callbacks Connection and SSH proxy authentication callbacks.
 * @param options SSH proxy and known-host overrides.
 * @return Reusable factory; each open owns a new parser and connection route.
 */
TerminalTransportRequest make_telnet_transport_request(TelnetConnectionSettings settings,
    std::string encoding, TerminalSessionCallbacks callbacks, AuthenticatedSshTransportOptions options);

/**
 * Creates an SSH shell factory using the shared authentication implementation.
 * @param settings Endpoint, login and requested terminal type.
 * @param encoding Peer character encoding.
 * @param callbacks Authentication prompts and connection notifications.
 * @param options SSH proxy, client configuration and known-host overrides.
 * @return Reusable factory with asynchronous shell I/O and resize completion.
 */
TerminalTransportRequest make_ssh_transport_request(SshConnectionSettings settings,
    std::string encoding, TerminalSessionCallbacks callbacks, AuthenticatedSshTransportOptions options);

/**
 * Creates a serial byte-stream factory using the saved device identity.
 * @param settings Device identity, framing, baud rate, flow control and carrier.
 * @param encoding Peer character encoding.
 * @param callbacks Receive/send activity and modem-line indicator notifications.
 * @return Reusable factory; each open resolves and configures a fresh descriptor.
 * @remarks Device notifications permit runtime recovery after a failed open or
 * unplug. The monitor is joined when the user disconnects or replaces the target.
 * Carrier drops suspend sending while retaining the open port and its reader.
 * Copies share configure_serial updates, including during open and recovery.
 * Only one channel may be open per factory. A new target needs a new factory.
 */
TerminalTransportRequest make_serial_transport_request(SerialConnectionSettings settings,
    std::string encoding, TerminalSessionCallbacks callbacks);

}
