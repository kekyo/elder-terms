#include "ip-scanner.h"

#include <gio/gio.h>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace elder_terms {

struct Ipv4AddressCursor {
  const std::vector<Ipv4ScanRange> *ranges;
  std::size_t range_index;
  std::uint32_t current_address;
  bool initialized;
};

struct IpScannerState {
  IpScannerDependencies dependencies;
  IpScannerCallbacks callbacks;
  Ipv4ScanPlan plan;
  Ipv4AddressCursor cursor;
  std::uint64_t completed_addresses;
};

struct PortProbeResult {
  bool open;
  std::exception_ptr error;
};

static bool is_contiguous_ipv4_netmask(std::uint32_t netmask) {
  const std::uint32_t host_mask = ~netmask;
  return (host_mask & (host_mask + 1U)) == 0;
}

static bool ranges_touch_or_overlap(const Ipv4ScanRange &left,
                                    const Ipv4ScanRange &right) {
  return right.first <= left.last ||
         (left.last != (std::numeric_limits<std::uint32_t>::max)() &&
          right.first == left.last + 1U);
}

Ipv4ScanPlan
create_ipv4_scan_plan(const std::vector<Ipv4InterfaceAddress> &interfaces) {
  Ipv4ScanPlan result{
      .ranges = {},
      .total_addresses = 0,
      .ignored_interface_count = 0,
  };
  for (const Ipv4InterfaceAddress &interface_address : interfaces) {
    if (!is_contiguous_ipv4_netmask(interface_address.netmask)) {
      ++result.ignored_interface_count;
      continue;
    }
    const std::uint32_t host_mask = ~interface_address.netmask;
    const std::uint32_t first =
        interface_address.address & interface_address.netmask;
    result.ranges.push_back({
        .first = first,
        .last = first | host_mask,
    });
  }

  std::sort(result.ranges.begin(), result.ranges.end(),
            [](const Ipv4ScanRange &left, const Ipv4ScanRange &right) {
              if (left.first != right.first) {
                return left.first < right.first;
              }
              return left.last < right.last;
            });
  std::vector<Ipv4ScanRange> merged;
  for (const Ipv4ScanRange &range : result.ranges) {
    if (merged.empty() || !ranges_touch_or_overlap(merged.back(), range)) {
      merged.push_back(range);
      continue;
    }
    merged.back().last = std::max(merged.back().last, range.last);
  }
  result.ranges = std::move(merged);
  for (const Ipv4ScanRange &range : result.ranges) {
    result.total_addresses += static_cast<std::uint64_t>(range.last) -
                              static_cast<std::uint64_t>(range.first) + 1U;
  }
  return result;
}

static bool next_ipv4_address(Ipv4AddressCursor *cursor,
                              std::uint32_t *address) {
  if (cursor->range_index >= cursor->ranges->size()) {
    return false;
  }
  if (!cursor->initialized) {
    cursor->current_address = (*cursor->ranges)[cursor->range_index].first;
    cursor->initialized = true;
  }

  *address = cursor->current_address;
  const Ipv4ScanRange &range = (*cursor->ranges)[cursor->range_index];
  if (cursor->current_address == range.last) {
    ++cursor->range_index;
    cursor->initialized = false;
  } else {
    ++cursor->current_address;
  }
  return true;
}

static std::string format_ipv4_address(std::uint32_t address) {
  in_addr native_address{};
  native_address.s_addr = htonl(address);
  std::array<char, INET_ADDRSTRLEN> text{};
  if (inet_ntop(AF_INET, &native_address, text.data(), text.size()) == nullptr) {
    throw std::system_error(errno, std::generic_category(),
                            "IPv4 address formatting failed");
  }
  return text.data();
}

static void report_progress(const std::shared_ptr<IpScannerState> &state) {
  state->callbacks.progress_changed({
      .completed_addresses = state->completed_addresses,
      .total_addresses = state->plan.total_addresses,
      .ignored_interface_count = state->plan.ignored_interface_count,
  });
}

static cardio::promise<PortProbeResult>
observe_port_probe_async(cardio::promise<bool> probe) {
  try {
    co_return PortProbeResult{
        .open = co_await probe,
        .error = nullptr,
    };
  } catch (...) {
    co_return PortProbeResult{
        .open = false,
        .error = std::current_exception(),
    };
  }
}

static cardio::promise<std::exception_ptr>
scan_hosts_worker_async(std::shared_ptr<IpScannerState> state,
                        cardio::cancellation cancellation) {
  try {
    std::uint32_t address = 0;
    while (next_ipv4_address(&state->cursor, &address)) {
      cancellation.throw_if_cancellation_requested();
      const auto port_results = co_await cardio::promises::all(
          observe_port_probe_async(
              state->dependencies.probe_port(address, 21, cancellation)),
          observe_port_probe_async(
              state->dependencies.probe_port(address, 22, cancellation)),
          observe_port_probe_async(
              state->dependencies.probe_port(address, 23, cancellation)));
      const std::array<PortProbeResult, 3> probes = {
          std::get<0>(port_results),
          std::get<1>(port_results),
          std::get<2>(port_results),
      };
      for (const PortProbeResult &probe : probes) {
        if (probe.error != nullptr) {
          std::rethrow_exception(probe.error);
        }
      }

      IpScanEntry entry{
          .address = format_ipv4_address(address),
          .reverse_fqdn = {},
          .open_ports = {},
      };
      if (probes[0].open) {
        entry.open_ports.push_back(21);
      }
      if (probes[1].open) {
        entry.open_ports.push_back(22);
      }
      if (probes[2].open) {
        entry.open_ports.push_back(23);
      }

      if (!entry.open_ports.empty()) {
        cancellation.throw_if_cancellation_requested();
        state->callbacks.entry_changed(entry);
        entry.reverse_fqdn =
            co_await state->dependencies.reverse_lookup(address, cancellation);
        if (!entry.reverse_fqdn.empty()) {
          cancellation.throw_if_cancellation_requested();
          state->callbacks.entry_changed(entry);
        }
      }

      cancellation.throw_if_cancellation_requested();
      ++state->completed_addresses;
      report_progress(state);
    }
    co_return nullptr;
  } catch (...) {
    co_return std::current_exception();
  }
}

cardio::promise<void>
scan_ipv4_hosts_async(IpScannerDependencies dependencies,
                      IpScannerCallbacks callbacks,
                      cardio::cancellation cancellation) {
  if (dependencies.maximum_concurrent_hosts == 0) {
    throw std::invalid_argument(
        "IP scanner host concurrency must be greater than zero");
  }
  if (!dependencies.probe_port || !dependencies.reverse_lookup ||
      !callbacks.entry_changed || !callbacks.progress_changed ||
      !callbacks.completed) {
    throw std::invalid_argument("IP scanner callbacks must not be empty");
  }

  auto state = std::make_shared<IpScannerState>(IpScannerState{
      .dependencies = std::move(dependencies),
      .callbacks = std::move(callbacks),
      .plan = {},
      .cursor = {},
      .completed_addresses = 0,
  });
  state->plan = create_ipv4_scan_plan(state->dependencies.interfaces);
  state->cursor = {
      .ranges = &state->plan.ranges,
      .range_index = 0,
      .current_address = 0,
      .initialized = false,
  };
  cancellation.throw_if_cancellation_requested();
  report_progress(state);

  const std::uint64_t worker_count =
      std::min<std::uint64_t>(state->plan.total_addresses,
                              state->dependencies.maximum_concurrent_hosts);
  std::vector<cardio::promise<std::exception_ptr>> workers;
  workers.reserve(static_cast<std::size_t>(worker_count));
  for (std::uint64_t index = 0; index < worker_count; ++index) {
    workers.push_back(scan_hosts_worker_async(state, cancellation));
  }
  const std::vector<std::exception_ptr> errors =
      co_await cardio::promises::all(std::move(workers));
  for (const std::exception_ptr &error : errors) {
    if (error != nullptr) {
      std::rethrow_exception(error);
    }
  }
  cancellation.throw_if_cancellation_requested();
  state->callbacks.completed();
}

static std::shared_ptr<GInetAddress>
make_gio_ipv4_address(std::uint32_t address) {
  const std::array<guint8, 4> bytes = {
      static_cast<guint8>(address >> 24U),
      static_cast<guint8>(address >> 16U),
      static_cast<guint8>(address >> 8U),
      static_cast<guint8>(address),
  };
  return std::shared_ptr<GInetAddress>(
      g_inet_address_new_from_bytes(bytes.data(), G_SOCKET_FAMILY_IPV4),
      g_object_unref);
}

cardio::promise<bool>
probe_ipv4_tcp_port_async(std::uint32_t address, std::uint16_t port,
                          std::uint64_t timeout_milliseconds,
                          cardio::cancellation cancellation) {
  cancellation.throw_if_cancellation_requested();
  auto timeout_source = cardio::cancellations::timeout(timeout_milliseconds);
  auto combined_source = cardio::cancellations::any(
      cancellation, timeout_source.get_cancellation());
  auto client =
      std::shared_ptr<GSocketClient>(g_socket_client_new(), g_object_unref);
  g_socket_client_set_family(client.get(), G_SOCKET_FAMILY_IPV4);
  g_socket_client_set_enable_proxy(client.get(), false);
  auto inet_address = make_gio_ipv4_address(address);
  auto socket_address = std::shared_ptr<GSocketAddress>(
      g_inet_socket_address_new(inet_address.get(), port), g_object_unref);

  try {
    GSocketConnection *raw_connection =
        co_await cardio::gio::submit<GSocketConnection *>(
            [client, socket_address](GCancellable *gio_cancellation,
                                     GAsyncReadyCallback callback,
                                     gpointer user_data) {
              g_socket_client_connect_async(
                  client.get(), G_SOCKET_CONNECTABLE(socket_address.get()),
                  gio_cancellation, callback, user_data);
            },
            [client](GObject *, GAsyncResult *result, GError **error) {
              return g_socket_client_connect_finish(client.get(), result,
                                                    error);
            },
            combined_source.get_cancellation());
    auto connection = std::shared_ptr<GSocketConnection>(raw_connection,
                                                          g_object_unref);
    co_return connection != nullptr;
  } catch (const cardio::canceled_exception &) {
    cancellation.throw_if_cancellation_requested();
    co_return false;
  } catch (const cardio::gio::gio_error &) {
    co_return false;
  }
}

static cardio::promise<std::string>
reverse_lookup_ipv4_async(std::uint32_t address,
                          std::uint64_t timeout_milliseconds,
                          cardio::cancellation cancellation) {
  cancellation.throw_if_cancellation_requested();
  auto timeout_source = cardio::cancellations::timeout(timeout_milliseconds);
  auto combined_source = cardio::cancellations::any(
      cancellation, timeout_source.get_cancellation());
  auto resolver =
      std::shared_ptr<GResolver>(g_resolver_get_default(), g_object_unref);
  auto inet_address = make_gio_ipv4_address(address);

  try {
    gchar *raw_name = co_await cardio::gio::submit<gchar *>(
        [resolver, inet_address](GCancellable *gio_cancellation,
                                 GAsyncReadyCallback callback,
                                 gpointer user_data) {
          g_resolver_lookup_by_address_async(
              resolver.get(), inet_address.get(), gio_cancellation, callback,
              user_data);
        },
        [resolver](GObject *, GAsyncResult *result, GError **error) {
          return g_resolver_lookup_by_address_finish(resolver.get(), result,
                                                     error);
        },
        combined_source.get_cancellation());
    auto name = std::unique_ptr<gchar, decltype(&g_free)>(raw_name, g_free);
    co_return name != nullptr ? std::string(name.get()) : std::string();
  } catch (const cardio::canceled_exception &) {
    cancellation.throw_if_cancellation_requested();
    co_return std::string();
  } catch (const cardio::gio::gio_error &) {
    co_return std::string();
  }
}

static std::vector<Ipv4InterfaceAddress> read_ipv4_interfaces() {
  ifaddrs *raw_interfaces = nullptr;
  if (getifaddrs(&raw_interfaces) != 0) {
    throw std::system_error(errno, std::generic_category(),
                            "reading network interfaces failed");
  }
  auto interfaces =
      std::unique_ptr<ifaddrs, decltype(&freeifaddrs)>(raw_interfaces,
                                                       freeifaddrs);
  std::vector<Ipv4InterfaceAddress> result;
  for (const ifaddrs *current = interfaces.get(); current != nullptr;
       current = current->ifa_next) {
    if (current->ifa_addr == nullptr || current->ifa_netmask == nullptr ||
        current->ifa_addr->sa_family != AF_INET ||
        current->ifa_netmask->sa_family != AF_INET) {
      continue;
    }
    const auto *address =
        reinterpret_cast<const sockaddr_in *>(current->ifa_addr);
    const auto *netmask =
        reinterpret_cast<const sockaddr_in *>(current->ifa_netmask);
    result.push_back({
        .address = ntohl(address->sin_addr.s_addr),
        .netmask = ntohl(netmask->sin_addr.s_addr),
    });
  }
  return result;
}

IpScannerDependencies create_system_ip_scanner_dependencies() {
  return {
      .interfaces = read_ipv4_interfaces(),
      .maximum_concurrent_hosts = 32,
      .probe_port = [](std::uint32_t address, std::uint16_t port,
                       cardio::cancellation cancellation) {
        return probe_ipv4_tcp_port_async(address, port, 1000,
                                         std::move(cancellation));
      },
      .reverse_lookup = [](std::uint32_t address,
                           cardio::cancellation cancellation) {
        return reverse_lookup_ipv4_async(address, 3000,
                                         std::move(cancellation));
      },
  };
}

} // namespace elder_terms
