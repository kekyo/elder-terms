#include "tcp-connector.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <exception>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

namespace elder_terms {

struct ResolvedSocketAddress {
  sockaddr_storage storage{};
  socklen_t length = 0;
  int family = AF_UNSPEC;
};

static ResolvedSocketAddress make_socket_address(GInetAddress *address,
                                                 std::uint16_t port) {
  if (address == nullptr) {
    throw std::runtime_error("resolver returned an empty address");
  }

  const GSocketFamily family = g_inet_address_get_family(address);
  const guint8 *bytes = g_inet_address_to_bytes(address);
  ResolvedSocketAddress result;
  if (family == G_SOCKET_FAMILY_IPV4) {
    sockaddr_in native_address{};
    native_address.sin_family = AF_INET;
    native_address.sin_port = htons(port);
    std::memcpy(&native_address.sin_addr, bytes, sizeof(native_address.sin_addr));
    std::memcpy(&result.storage, &native_address, sizeof(native_address));
    result.length = sizeof(native_address);
    result.family = AF_INET;
    return result;
  }

  if (family == G_SOCKET_FAMILY_IPV6) {
    sockaddr_in6 native_address{};
    native_address.sin6_family = AF_INET6;
    native_address.sin6_port = htons(port);
    std::memcpy(&native_address.sin6_addr, bytes,
                sizeof(native_address.sin6_addr));
    std::memcpy(&result.storage, &native_address, sizeof(native_address));
    result.length = sizeof(native_address);
    result.family = AF_INET6;
    return result;
  }

  throw std::runtime_error("resolver returned an unsupported address");
}

static cardio::promise<std::vector<ResolvedSocketAddress>>
resolve_socket_addresses_async(std::string host, std::uint16_t port,
                               cardio::cancellation cancellation) {
  auto resolver =
      std::shared_ptr<GResolver>(g_resolver_get_default(), g_object_unref);
  auto host_holder = std::make_shared<std::string>(std::move(host));
  GList *raw_addresses = co_await cardio::gio::submit<GList *>(
      [resolver, host_holder](GCancellable *cancellable,
                              GAsyncReadyCallback callback,
                              gpointer user_data) {
        g_resolver_lookup_by_name_async(resolver.get(), host_holder->c_str(),
                                        cancellable, callback, user_data);
      },
      [resolver](GObject *object, GAsyncResult *result, GError **error) {
        (void)resolver;
        return g_resolver_lookup_by_name_finish(G_RESOLVER(object), result,
                                                error);
      },
      std::move(cancellation));

  auto addresses = std::unique_ptr<GList, decltype(&g_resolver_free_addresses)>(
      raw_addresses, g_resolver_free_addresses);
  if (addresses == nullptr) {
    throw std::runtime_error("resolver returned no addresses");
  }

  std::vector<ResolvedSocketAddress> results;
  for (GList *node = addresses.get(); node != nullptr; node = node->next) {
    results.push_back(make_socket_address(G_INET_ADDRESS(node->data), port));
  }
  co_return results;
}

static cardio::promise<int>
open_socket_async(cardio::io_uring &io, int family,
                  cardio::cancellation cancellation) {
  return io.submit<int>(
      [family](::io_uring_sqe *sqe) {
        ::io_uring_prep_socket(sqe, family, SOCK_STREAM | SOCK_CLOEXEC, 0, 0);
      },
      [](cardio::io_uring_completion completion) {
        if (completion.result < 0) {
          throw std::system_error(-completion.result, std::generic_category(),
                                  "TCP socket failed");
        }
        return completion.result;
      },
      std::move(cancellation));
}

static cardio::promise<void>
connect_socket_async(cardio::io_uring &io, int fd,
                     const ResolvedSocketAddress &address,
                     cardio::cancellation cancellation) {
  auto storage = std::make_shared<sockaddr_storage>(address.storage);
  const socklen_t length = address.length;
  return io.submit<void>(
      [fd, storage, length](::io_uring_sqe *sqe) {
        ::io_uring_prep_connect(
            sqe, fd, reinterpret_cast<const sockaddr *>(storage.get()), length);
      },
      [storage](cardio::io_uring_completion completion) {
        (void)storage;
        if (completion.result < 0) {
          throw std::system_error(-completion.result, std::generic_category(),
                                  "TCP connect failed");
        }
      },
      std::move(cancellation));
}

cardio::promise<int>
connect_tcp_socket_async(cardio::io_uring &io, std::string host,
                         std::uint16_t port,
                         cardio::cancellation cancellation) {
  const std::vector<ResolvedSocketAddress> addresses =
      co_await resolve_socket_addresses_async(std::move(host), port,
                                              cancellation);
  std::exception_ptr last_error;
  for (const ResolvedSocketAddress &address : addresses) {
    cancellation.throw_if_cancellation_requested();
    int fd = -1;
    try {
      fd = co_await open_socket_async(io, address.family, cancellation);
      co_await connect_socket_async(io, fd, address, cancellation);
      co_return fd;
    } catch (const cardio::canceled_exception &) {
      if (fd >= 0) {
        (void)::close(fd);
      }
      throw;
    } catch (...) {
      if (fd >= 0) {
        (void)::close(fd);
      }
      last_error = std::current_exception();
    }
  }

  if (last_error != nullptr) {
    std::rethrow_exception(last_error);
  }
  throw std::runtime_error("resolver returned no supported addresses");
}

} // namespace elder_terms
