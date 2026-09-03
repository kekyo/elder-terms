#include <elder-terms/ip-scanner.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace elder_terms_ip_scanner_test {

static void expect(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

static std::uint32_t ipv4(unsigned int first, unsigned int second,
                          unsigned int third, unsigned int fourth) {
  return (first << 24U) | (second << 16U) | (third << 8U) | fourth;
}

static void scan_plan_includes_complete_ranges_and_merges_overlaps() {
  const elder_terms::Ipv4ScanPlan plan = elder_terms::create_ipv4_scan_plan({
      {
          .address = ipv4(192, 168, 1, 42),
          .netmask = ipv4(255, 255, 255, 0),
      },
      {
          .address = ipv4(192, 168, 1, 129),
          .netmask = ipv4(255, 255, 255, 128),
      },
      {
          .address = ipv4(10, 0, 0, 1),
          .netmask = ipv4(255, 255, 255, 254),
      },
      {
          .address = ipv4(172, 16, 1, 3),
          .netmask = ipv4(255, 0, 255, 0),
      },
  });

  expect(plan.ranges.size() == 2, "overlapping IPv4 ranges were not merged");
  expect(plan.ranges[0].first == ipv4(10, 0, 0, 0) &&
             plan.ranges[0].last == ipv4(10, 0, 0, 1),
         "/31 range did not include both addresses");
  expect(plan.ranges[1].first == ipv4(192, 168, 1, 0) &&
             plan.ranges[1].last == ipv4(192, 168, 1, 255),
         "/24 range did not include network and broadcast addresses");
  expect(plan.total_addresses == 258,
         "merged IPv4 scan plan has an incorrect address count");
  expect(plan.ignored_interface_count == 1,
         "non-contiguous IPv4 netmask was not ignored");
}

static void scan_plan_limits_wide_networks_to_the_first_eight_host_bits() {
  const elder_terms::Ipv4ScanPlan plan = elder_terms::create_ipv4_scan_plan({
      {
          .address = ipv4(203, 0, 113, 5),
          .netmask = 0,
      },
      {
          .address = ipv4(172, 20, 55, 4),
          .netmask = ipv4(255, 255, 0, 0),
      },
      {
          .address = ipv4(192, 0, 3, 200),
          .netmask = ipv4(255, 255, 254, 0),
      },
  });

  expect(plan.ranges.size() == 3,
         "wide IPv4 ranges were not kept as separate first /24 ranges");
  expect(plan.ranges[0].first == ipv4(0, 0, 0, 0) &&
             plan.ranges[0].last == ipv4(0, 0, 0, 255),
         "/0 was not limited with its upper host bits set to zero");
  expect(plan.ranges[1].first == ipv4(172, 20, 0, 0) &&
             plan.ranges[1].last == ipv4(172, 20, 0, 255),
         "/16 was not limited with its upper host bits set to zero");
  expect(plan.ranges[2].first == ipv4(192, 0, 2, 0) &&
             plan.ranges[2].last == ipv4(192, 0, 2, 255),
         "/23 was not limited with its upper host bit set to zero");
  expect(plan.total_addresses == 768,
         "limited IPv4 scan plan has an incorrect address count");
}

static void scan_plan_uses_only_assigned_loopback_addresses() {
  const elder_terms::Ipv4ScanPlan plan = elder_terms::create_ipv4_scan_plan({
      {
          .address = ipv4(127, 0, 0, 1),
          .netmask = ipv4(255, 0, 0, 0),
      },
      {
          .address = ipv4(127, 42, 1, 9),
          .netmask = ipv4(255, 0, 0, 0),
      },
  });

  expect(plan.ranges.size() == 2,
         "separate assigned loopback addresses were merged into a subnet");
  expect(plan.ranges[0].first == ipv4(127, 0, 0, 1) &&
             plan.ranges[0].last == ipv4(127, 0, 0, 1),
         "primary loopback address was not limited to its assigned address");
  expect(plan.ranges[1].first == ipv4(127, 42, 1, 9) &&
             plan.ranges[1].last == ipv4(127, 42, 1, 9),
         "loopback alias was not limited to its assigned address");
  expect(plan.total_addresses == 2,
         "loopback scan plan contains unassigned addresses");
}

static void complete_with_shutdown(
    cardio::dispatcher_group_glib *group, bool *completed) {
  *completed = true;
  group->shutdown();
}

static void scan_reports_open_standard_ports_and_reverse_names() {
  cardio::dispatcher_group_glib group;
  cardio::dispatcher_host_glib dispatcher(group);
  std::vector<std::pair<std::uint32_t, std::uint16_t>> probes;
  std::vector<std::uint32_t> reverse_lookups;
  std::vector<elder_terms::IpScanEntry> entries;
  std::vector<elder_terms::IpScanProgress> progress;
  bool completed = false;

  elder_terms::IpScannerDependencies dependencies{
      .interfaces = {
          {
              .address = ipv4(192, 0, 2, 9),
              .netmask = ipv4(255, 255, 255, 252),
          },
          {
              .address = ipv4(198, 51, 100, 1),
              .netmask = ipv4(255, 0, 255, 0),
          },
      },
      .maximum_concurrent_hosts = 1,
      .probe_port = [&probes](std::uint32_t address, std::uint16_t port,
                              cardio::cancellation) {
        probes.emplace_back(address, port);
        return cardio::resolved(
            (address == ipv4(192, 0, 2, 8) && port == 22) ||
            (address == ipv4(192, 0, 2, 10) &&
             (port == 21 || port == 23)));
      },
      .reverse_lookup = [&reverse_lookups](std::uint32_t address,
                                           cardio::cancellation) {
        reverse_lookups.push_back(address);
        return cardio::resolved(
            address == ipv4(192, 0, 2, 8)
                ? std::string("router.example.test")
                : std::string());
      },
  };
  elder_terms::IpScannerCallbacks callbacks{
      .entry_changed = [&entries](const elder_terms::IpScanEntry &entry) {
        entries.push_back(entry);
      },
      .progress_changed =
          [&progress](const elder_terms::IpScanProgress &value) {
            progress.push_back(value);
          },
      .completed = [&group, &completed]() {
        complete_with_shutdown(&group, &completed);
      },
  };
  cardio::cancellation_source cancellation_source;

  auto task = elder_terms::scan_ipv4_hosts_async(
      std::move(dependencies), std::move(callbacks),
      cancellation_source.get_cancellation());
  dispatcher.park();
  task.unsafe_result();

  const std::vector<std::uint16_t> expected_ports = {21, 22, 23};
  expect(completed, "successful scan did not report completion");
  expect(probes.size() == 12, "scan did not probe three ports on every host");
  for (std::size_t index = 0; index < probes.size(); ++index) {
    expect(probes[index].second == expected_ports[index % 3],
           "scan probed a non-standard or incorrectly ordered port");
  }
  expect(reverse_lookups ==
             std::vector<std::uint32_t>({ipv4(192, 0, 2, 8),
                                         ipv4(192, 0, 2, 10)}),
         "reverse lookup did not run exactly for discovered hosts");
  expect(entries.size() == 3, "scan entry updates were not incremental");
  expect(entries[0].address == "192.0.2.8" &&
             entries[0].reverse_fqdn.empty() &&
             entries[0].open_ports == std::vector<std::uint16_t>({22}),
         "first open host was not reported before reverse lookup");
  expect(entries[1].address == "192.0.2.8" &&
             entries[1].reverse_fqdn == "router.example.test" &&
             entries[1].open_ports == std::vector<std::uint16_t>({22}),
         "reverse lookup did not update the existing scan entry");
  expect(entries[2].address == "192.0.2.10" &&
             entries[2].reverse_fqdn.empty() &&
             entries[2].open_ports ==
                 std::vector<std::uint16_t>({21, 23}),
         "open ports were not reported in ascending order");
  expect(progress.size() == 5, "scan did not report initial and host progress");
  for (std::size_t index = 0; index < progress.size(); ++index) {
    expect(progress[index].completed_addresses == index &&
               progress[index].total_addresses == 4 &&
               progress[index].ignored_interface_count == 1,
           "scan progress values were incorrect");
  }
}

struct ConcurrencyState {
  std::map<std::uint32_t, std::size_t> active_ports;
  std::size_t maximum_active_hosts = 0;
  std::size_t probe_count = 0;
};

static cardio::promise<bool> delayed_closed_probe(
    ConcurrencyState *state, std::uint32_t address, std::uint16_t,
    cardio::cancellation cancellation) {
  ++state->active_ports[address];
  ++state->probe_count;
  state->maximum_active_hosts =
      std::max(state->maximum_active_hosts, state->active_ports.size());
  try {
    co_await cardio::promises::delay(1, cancellation);
  } catch (...) {
    if (--state->active_ports[address] == 0) {
      state->active_ports.erase(address);
    }
    throw;
  }
  if (--state->active_ports[address] == 0) {
    state->active_ports.erase(address);
  }
  co_return false;
}

static void scan_limits_concurrent_hosts() {
  cardio::dispatcher_group_glib group;
  cardio::dispatcher_host_glib dispatcher(group);
  ConcurrencyState state;
  bool completed = false;
  elder_terms::IpScannerDependencies dependencies{
      .interfaces = {{
          .address = ipv4(10, 0, 0, 3),
          .netmask = ipv4(255, 255, 255, 248),
      }},
      .maximum_concurrent_hosts = 2,
      .probe_port = [&state](std::uint32_t address, std::uint16_t port,
                             cardio::cancellation cancellation) {
        return delayed_closed_probe(&state, address, port,
                                    std::move(cancellation));
      },
      .reverse_lookup = [](std::uint32_t, cardio::cancellation) {
        return cardio::resolved(std::string());
      },
  };
  elder_terms::IpScannerCallbacks callbacks{
      .entry_changed = [](const elder_terms::IpScanEntry &) {},
      .progress_changed = [](const elder_terms::IpScanProgress &) {},
      .completed = [&group, &completed]() {
        complete_with_shutdown(&group, &completed);
      },
  };
  cardio::cancellation_source cancellation_source;

  auto task = elder_terms::scan_ipv4_hosts_async(
      std::move(dependencies), std::move(callbacks),
      cancellation_source.get_cancellation());
  dispatcher.park();
  task.unsafe_result();

  expect(completed, "bounded scan did not complete");
  expect(state.probe_count == 24, "bounded scan skipped a host or port");
  expect(state.maximum_active_hosts == 2,
         "scan exceeded its configured host concurrency");
}

static cardio::promise<bool> wait_until_canceled_probe(
    std::uint32_t, std::uint16_t, cardio::cancellation cancellation) {
  co_await cardio::promises::delay(60000, std::move(cancellation));
  co_return false;
}

static cardio::promise<void> observe_canceled_scan(
    cardio::promise<void> scan, bool *canceled,
    std::exception_ptr *unexpected_error,
    cardio::dispatcher_group_glib *group) {
  try {
    co_await scan;
  } catch (const cardio::canceled_exception &) {
    *canceled = true;
  } catch (...) {
    *unexpected_error = std::current_exception();
  }
  group->shutdown();
}

static void scan_cancellation_stops_pending_work_without_completion() {
  cardio::dispatcher_group_glib group;
  cardio::dispatcher_host_glib dispatcher(group);
  bool completed = false;
  bool canceled = false;
  std::exception_ptr unexpected_error;
  elder_terms::IpScannerDependencies dependencies{
      .interfaces = {{
          .address = ipv4(10, 0, 0, 1),
          .netmask = ipv4(255, 255, 255, 0),
      }},
      .maximum_concurrent_hosts = 4,
      .probe_port = wait_until_canceled_probe,
      .reverse_lookup = [](std::uint32_t, cardio::cancellation) {
        return cardio::resolved(std::string());
      },
  };
  elder_terms::IpScannerCallbacks callbacks{
      .entry_changed = [](const elder_terms::IpScanEntry &) {},
      .progress_changed = [](const elder_terms::IpScanProgress &) {},
      .completed = [&completed]() { completed = true; },
  };
  cardio::cancellation_source cancellation_source;

  auto scan = elder_terms::scan_ipv4_hosts_async(
      std::move(dependencies), std::move(callbacks),
      cancellation_source.get_cancellation());
  auto observer = observe_canceled_scan(std::move(scan), &canceled,
                                        &unexpected_error, &group);
  expect(cancellation_source.cancel(), "scan cancellation was not requested");
  dispatcher.park();
  observer.unsafe_result();

  expect(canceled, "pending scan did not propagate cancellation");
  expect(!completed, "canceled scan reported successful completion");
  if (unexpected_error != nullptr) {
    std::rethrow_exception(unexpected_error);
  }
}

struct Listener {
  int fd;
  std::uint16_t port;
};

static Listener create_loopback_listener() {
  const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    throw std::system_error(errno, std::generic_category(), "socket failed");
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  if (::bind(fd, reinterpret_cast<const sockaddr *>(&address),
             sizeof(address)) != 0 ||
      ::listen(fd, 1) != 0) {
    const int error = errno;
    (void)::close(fd);
    throw std::system_error(error, std::generic_category(),
                            "loopback listen failed");
  }
  socklen_t length = sizeof(address);
  if (::getsockname(fd, reinterpret_cast<sockaddr *>(&address), &length) != 0) {
    const int error = errno;
    (void)::close(fd);
    throw std::system_error(error, std::generic_category(),
                            "getsockname failed");
  }
  return {
      .fd = fd,
      .port = ntohs(address.sin_port),
  };
}

static cardio::promise<void> collect_probe_result(
    cardio::promise<bool> probe, bool *open, std::exception_ptr *error,
    cardio::dispatcher_group_glib *group) {
  try {
    *open = co_await probe;
  } catch (...) {
    *error = std::current_exception();
  }
  group->shutdown();
}

static void gio_probe_detects_an_open_tcp_port_asynchronously() {
  Listener listener = create_loopback_listener();
  cardio::dispatcher_group_glib group;
  cardio::dispatcher_host_glib dispatcher(group);
  cardio::cancellation_source cancellation_source;
  bool open = false;
  std::exception_ptr error;

  auto probe = elder_terms::probe_ipv4_tcp_port_async(
      ipv4(127, 0, 0, 1), listener.port, 5000,
      cancellation_source.get_cancellation());
  auto collector =
      collect_probe_result(std::move(probe), &open, &error, &group);
  dispatcher.park();
  collector.unsafe_result();
  (void)::close(listener.fd);

  if (error != nullptr) {
    std::rethrow_exception(error);
  }
  expect(open, "GIO probe did not detect an open loopback TCP port");
}

} // namespace elder_terms_ip_scanner_test

int main() {
  try {
    elder_terms_ip_scanner_test::
        scan_plan_includes_complete_ranges_and_merges_overlaps();
    elder_terms_ip_scanner_test::
        scan_plan_limits_wide_networks_to_the_first_eight_host_bits();
    elder_terms_ip_scanner_test::
        scan_plan_uses_only_assigned_loopback_addresses();
    elder_terms_ip_scanner_test::
        scan_reports_open_standard_ports_and_reverse_names();
    elder_terms_ip_scanner_test::scan_limits_concurrent_hosts();
    elder_terms_ip_scanner_test::
        scan_cancellation_stops_pending_work_without_completion();
    elder_terms_ip_scanner_test::
        gio_probe_detects_an_open_tcp_port_asynchronously();
  } catch (const std::exception &exception) {
    std::cerr << exception.what() << '\n';
    return 1;
  }
  return 0;
}
