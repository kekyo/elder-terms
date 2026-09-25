#pragma once

#include <string>
#include <sys/un.h>
#include <unistd.h>

namespace elder_terms {

/** Maximum size of one control request, including its field separators. */
inline constexpr std::size_t control_request_limit = 16384;

/** Owns one descriptor, including during coroutine cancellation. */
struct ControlDescriptor {
  /** Owned descriptor, or -1 when not open. */
  int value = -1;
  /** Creates an empty owner. */
  ControlDescriptor() = default;
  ControlDescriptor(const ControlDescriptor &) = delete;
  ControlDescriptor &operator=(const ControlDescriptor &) = delete;
  /** Closes the descriptor when open. */
  ~ControlDescriptor() {
    if (value >= 0) {
      close(value);
    }
  }
};

/**
 * Resolves the control socket in the current user's runtime directory.
 * @returns Absolute socket path.
 * @throws std::runtime_error if XDG_RUNTIME_DIR is missing or unsafe.
 */
std::string control_socket_path();

/**
 * Builds a pathname socket address without truncation.
 * @param path Absolute socket path.
 * @returns Initialized Unix socket address.
 * @throws std::runtime_error if the path is too long.
 */
sockaddr_un control_socket_address(const std::string &path);

/**
 * Checks the peer's effective user identity.
 * @param descriptor Connected Unix socket.
 * @returns True when SO_PEERCRED reports the current effective UID.
 */
bool control_peer_is_owner(int descriptor);

} // namespace elder_terms
