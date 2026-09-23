#pragma once

#include <functional>
#include <optional>
#include <string>

namespace elder_terms {

/** A transport-independent request to show the launcher or open a connection. */
struct ControlRequest {
  /** Saved connection name, or no value to show the launcher. */
  std::optional<std::string> connection;
  /** Desktop activation token, when supplied by the caller. */
  std::optional<std::string> activation_token;
};

/** Handles a request, returning an empty string on acceptance or an error. */
using ControlCallback = std::function<std::string(const ControlRequest &)>;

/** Opaque asynchronous control server and its owned resources. */
struct ControlServerState;

/**
 * Starts a private Unix control socket on the current cardio dispatcher.
 * @param callback Runs accepted requests on the dispatcher's thread.
 * @returns Server owned by the caller.
 * @throws std::runtime_error on unsafe paths, an existing owner, or I/O errors.
 */
ControlServerState *create_control_server(ControlCallback callback);

/**
 * Cancels pending requests and releases the socket and process lock.
 * @param state Server to destroy, or null.
 */
void destroy_control_server(ControlServerState *state);

} // namespace elder_terms
