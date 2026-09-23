#include "control-socket.h"

#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/stat.h>

namespace elder_terms {

std::string control_socket_path() {
  const char *runtime = std::getenv("XDG_RUNTIME_DIR");
  struct stat status {};
  if (runtime == nullptr || runtime[0] != '/' ||
      lstat(runtime, &status) != 0 || !S_ISDIR(status.st_mode) ||
      status.st_uid != geteuid() || (status.st_mode & 0077) != 0) {
    throw std::runtime_error(
        "XDG_RUNTIME_DIR must name an existing private directory owned by you");
  }
  return std::string(runtime) + "/elder-terms/control.sock";
}

sockaddr_un control_socket_address(const std::string &path) {
  sockaddr_un address {};
  address.sun_family = AF_UNIX;
  if (path.size() >= sizeof(address.sun_path)) {
    throw std::runtime_error("The control socket path is too long");
  }
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
  return address;
}

bool control_peer_is_owner(int descriptor) {
  ucred credentials {};
  socklen_t size = sizeof(credentials);
  return getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED,
                    &credentials, &size) == 0 &&
         size == sizeof(credentials) && credentials.uid == geteuid();
}

} // namespace elder_terms
