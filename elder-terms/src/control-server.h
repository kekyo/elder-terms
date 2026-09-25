#pragma once

#include <functional>
#include <optional>
#include <string>

namespace elder_terms {

/** Accepted control operations. */
enum class ControlCommand { open_application, open_connection, setup };

/** A transport-independent request to control the launcher. */
struct ControlRequest {
  /** Requested operation. */
  ControlCommand command;
  /** Saved connection name for open_connection, otherwise no value. */
  std::optional<std::string> connection;
  /** Desktop activation token, when supplied by the caller. */
  std::optional<std::string> activation_token;
};

/** Outcome of a control request. */
struct ControlReply {
  /** Whether the operation was accepted. */
  bool success;
  /** Whether setup should be queried again after initialization. */
  bool pending;
  /** Human-readable result or error. */
  std::string message;
};

/** Handles a control request on the dispatcher's thread. */
using ControlCallback = std::function<ControlReply(const ControlRequest &)>;

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
