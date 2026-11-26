// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.

#pragma once

#include <gio/gio.h>
#include <functional>
#include <string>

namespace elder_terms {

/** One origin-owned SFTP presentation endpoint for a transferred SSH session. */
struct SftpOriginService;

/**
 * Publishes a Show method on the original SSH process's session bus name.
 * @param connection Borrowed session-bus connection.
 * @param session_id Stable UUID of the transferred SSH session.
 * @param show Presents or creates the origin's shared SFTP window.
 * @return Owned endpoint, or nullptr if registration failed.
 * @remarks The callback may retain application state; destroy the service
 * before releasing that state. The origin keeps its bus connection alive.
 */
SftpOriginService *create_sftp_origin_service(GDBusConnection *connection,
    std::string session_id, std::function<void()> show);

/** Returns the origin's unique bus name to carry with subsequent handoffs. */
std::string sftp_origin_service_bus_name(const SftpOriginService *service);

/** Removes the Show endpoint before its callback's owner is destroyed. */
void destroy_sftp_origin_service(SftpOriginService *service);

/**
 * Asynchronously asks the original SSH process to present shared SFTP.
 * @param connection Borrowed session-bus connection.
 * @param origin_bus_name Original process's unique D-Bus name.
 * @param session_id Stable UUID retained through all later tab moves.
 * @param complete Reports whether the origin accepted the request.
 */
void request_sftp_origin(GDBusConnection *connection,
    std::string origin_bus_name, std::string session_id,
    std::function<void(bool)> complete);

}
