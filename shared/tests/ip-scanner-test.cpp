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
#include <memory>
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
             entries[0].resolved_name.empty() &&
             entries[0].open_ports == std::vector<std::uint16_t>({22}),
         "first open host was not reported before reverse lookup");
  expect(entries[1].address == "192.0.2.8" &&
             entries[1].resolved_name == "router.example.test" &&
             entries[1].open_ports == std::vector<std::uint16_t>({22}),
         "reverse lookup did not update the existing scan entry");
  expect(entries[2].address == "192.0.2.10" &&
             entries[2].resolved_name.empty() &&
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

template <typename T>
static cardio::promise<T> wait_for_lookup_cancellation(
    cardio::cancellation cancellation) {
  auto source = std::make_shared<cardio::promise_source<T>>();
  auto registration = cancellation.on_cancellation_requested(
      [source]() { (void)source->try_cancel(); });
  co_return co_await source->get_promise();
}

struct NameLookupCase {
  std::string label;
  std::string system_name;
  std::vector<elder_terms::IpScanNameCandidate> mdns;
  std::vector<elder_terms::IpScanNameCandidate> llmnr;
  std::string expected_name;
  elder_terms::IpScanNameSource expected_source;
  bool expire_system = false;
  bool expire_total = false;
  bool cancel_scan = false;
  bool llmnr_first = false;
};

struct NameLookupTestState {
  const NameLookupCase &test_case;
  std::map<std::uint64_t, cardio::cancellation_source> deadlines;
  std::vector<elder_terms::IpScanNameSource> requests;
  cardio::cancellation_source cancellation;
  std::shared_ptr<cardio::promise_source<
      std::vector<elder_terms::IpScanNameCandidate>>> delayed_mdns =
      std::make_shared<cardio::promise_source<
          std::vector<elder_terms::IpScanNameCandidate>>>();
  cardio::promise_source<void> llmnr_ready;
};

static cardio::promise<std::string> system_name_for_test_async(
    NameLookupTestState *state, cardio::cancellation signal) {
  if (state->test_case.expire_system) {
    expect(state->deadlines.contains(1000), "system lookup has no 1s deadline");
    (void)state->deadlines.at(1000).cancel();
    co_return co_await wait_for_lookup_cancellation<std::string>(signal);
  }
  co_return state->test_case.system_name;
}

static cardio::promise<std::vector<elder_terms::IpScanNameCandidate>>
multicast_names_for_test_async(NameLookupTestState *state,
                               std::uint32_t address,
                               elder_terms::IpScanNameSource source,
                               cardio::cancellation signal) {
  using Source = elder_terms::IpScanNameSource;
  using Candidates = std::vector<elder_terms::IpScanNameCandidate>;
  const auto &test_case = state->test_case;
  expect(address == ipv4(192, 0, 2, 25), "lookup changed the IP address");
  state->requests.push_back(source);
  expect(source == Source::mdns || source == Source::llmnr,
         "unexpected multicast protocol");
  if (test_case.expire_total || test_case.cancel_scan) {
    if (test_case.expire_total && source == Source::mdns &&
        !test_case.mdns.empty()) {
      co_return test_case.mdns;
    }
    if (state->requests.size() == 2) {
      if (test_case.cancel_scan) {
        (void)state->cancellation.cancel();
      } else {
        expect(state->deadlines.contains(3000), "lookup has no shared 3s deadline");
        (void)state->deadlines.at(3000).cancel();
      }
    }
    co_return co_await wait_for_lookup_cancellation<Candidates>(signal);
  }
  if (test_case.llmnr_first) {
    co_return co_await state->delayed_mdns->get_promise();
  }
  co_return source == Source::mdns ? test_case.mdns : test_case.llmnr;
}

static cardio::promise<void> check_name_lookup_async(
    const NameLookupCase &test_case) {
  using Source = elder_terms::IpScanNameSource;
  NameLookupTestState state{
      .test_case = test_case,
      .deadlines = {},
      .requests = {},
      .cancellation = {},
      .llmnr_ready = {},
  };
  std::vector<elder_terms::IpScanEntry> entries;
  bool completed = false;
  // Keep the source completed when this case does not use delayed mDNS.
  if (!test_case.llmnr_first) {
    state.delayed_mdns->resolve({});
    state.llmnr_ready.resolve();
  }
  elder_terms::IpScannerDependencies dependencies{
      .interfaces = {{ipv4(192, 0, 2, 25), ipv4(255, 255, 255, 255)}},
      .maximum_concurrent_hosts = 1,
      .probe_port = [](std::uint32_t, std::uint16_t port,
                        cardio::cancellation) {
        return cardio::resolved(port == 22);
      },
      .reverse_lookup = [&](std::uint32_t, cardio::cancellation signal) {
        return system_name_for_test_async(&state, signal);
      },
      .multicast_lookup = [&](std::uint32_t address, Source source,
                               cardio::cancellation signal) {
        if (test_case.llmnr_first && source == Source::llmnr) {
          state.requests.push_back(source);
          auto reply = cardio::resolved(test_case.llmnr);
          state.llmnr_ready.resolve();
          return reply;
        }
        return multicast_names_for_test_async(&state, address, source, signal);
      },
      .name_lookup_timeout = [&](std::uint64_t duration) {
        expect(!state.deadlines.contains(duration), "name lookup restarted its deadline");
        return state.deadlines.emplace(duration, cardio::cancellation_source{}).first->second;
      },
  };
  bool canceled = false;
  try {
    auto scan = elder_terms::scan_ipv4_hosts_async(
        std::move(dependencies),
        {.entry_changed = [&](const elder_terms::IpScanEntry &entry) {
           entries.push_back(entry);
         },
         .progress_changed = [](const elder_terms::IpScanProgress &) {},
         .completed = [&]() { completed = true; }},
        state.cancellation.get_cancellation());
    if (test_case.llmnr_first) {
      // LLMNR has already completed before allowing mDNS to respond.
      co_await state.llmnr_ready.get_promise();
      state.delayed_mdns->resolve(test_case.mdns);
    }
    co_await scan;
  } catch (const cardio::canceled_exception &) {
    canceled = true;
  }
  expect(canceled == test_case.cancel_scan, "incorrect scan cancellation result");
  expect(completed != test_case.cancel_scan, "incorrect completion notification");
  expect(!entries.empty() && entries.front().resolved_name.empty(),
         "host was not reported before name lookup");
  expect(entries.back().resolved_name == test_case.expected_name,
         "incorrect resolved name: " + entries.back().resolved_name);
  expect(entries.back().name_source == test_case.expected_source,
         "incorrect name source");
  expect(entries.size() == (test_case.expected_name.empty() ? 1U : 2U),
         "lookup emitted duplicate or late entry updates");
  expect(state.requests.size() == (test_case.system_name.empty() ? 2U : 0U),
         "multicast lookup was not conditional on the system resolver result");
}

static cardio::promise<void> observe_name_lookup_test(
    const NameLookupCase &test_case, cardio::dispatcher_group_glib *group,
    std::exception_ptr *error) {
  try {
    co_await check_name_lookup_async(test_case);
  } catch (...) {
    *error = std::current_exception();
  }
  group->shutdown();
}

static void scan_resolves_multicast_names_with_shared_deadlines() {
  using Source = elder_terms::IpScanNameSource;
  const std::vector<NameLookupCase> cases = {
      {"mDNS only", "", {{"printer.local", 2}}, {}, "printer.local", Source::mdns},
      {"LLMNR only", "", {}, {{"printer", 2}}, "printer", Source::llmnr},
      {"system priority", "dns.example.test", {{"printer.local", 2}},
       {{"printer", 2}}, "dns.example.test", Source::system},
      {"mDNS priority", "", {{"printer.local", 2}}, {{"printer", 2}},
       "printer.local", Source::mdns},
      {"LLMNR replies first", "", {{"printer.local", 2}}, {{"printer", 2}},
       "printer.local", Source::mdns, false, false, false, true},
      {"stable candidates", "", {{"z.local", 2}, {"a.local", 3},
                                   {"bad/name", 1}, {"", 1}, {"a.local", 2}},
       {}, "a.local", Source::mdns},
      {"reordered candidates", "", {{"a.local", 2}, {"", 1},
                                     {"bad/name", 1}, {"a.local", 3}, {"z.local", 2}},
       {}, "a.local", Source::mdns},
      {"invalid names", "", {{"bad/name", 1}, {"bad:23", 1},
                               {"bad name", 1}, {"bad\nname", 1},
                               {".local", 1}, {"bad..local", 1},
                               {"bad.local", -1}},
       {{"printer", 2}}, "printer", Source::llmnr},
      {"no names", "", {}, {}, "", Source::none},
      {"system deadline", "", {{"printer.local", 2}}, {},
       "printer.local", Source::mdns, true},
      {"total deadline", "", {}, {}, "", Source::none, false, true},
      {"keep mDNS while LLMNR times out", "", {{"printer.local", 2}}, {},
       "printer.local", Source::mdns, false, true},
      {"user cancellation", "", {}, {}, "", Source::none, false, false, true},
  };
  std::size_t failures = 0;
  for (const auto &test_case : cases) {
    cardio::dispatcher_group_glib group;
    cardio::dispatcher_host_glib dispatcher(group);
    std::exception_ptr error;
    auto task = observe_name_lookup_test(test_case, &group, &error);
    dispatcher.park();
    if (error != nullptr) {
      ++failures;
      try {
        std::rethrow_exception(error);
      } catch (const std::exception &exception) {
        std::cerr << test_case.label << ": " << exception.what() << '\n';
      }
    }
  }
  expect(failures == 0, "multicast name lookup cases failed");
}

static cardio::promise<void> check_same_name_hosts_async(
    cardio::dispatcher_group_glib *group, std::exception_ptr *error) {
  try {
    std::map<std::string, std::string> hosts;
    std::size_t updates = 0;
    bool completed = false;
    elder_terms::IpScannerDependencies dependencies{
        .interfaces = {{ipv4(192, 0, 2, 25), UINT32_MAX},
                       {ipv4(192, 0, 2, 26), UINT32_MAX}},
        .maximum_concurrent_hosts = 2,
        .probe_port = [](std::uint32_t, std::uint16_t port,
                         cardio::cancellation) {
          return cardio::resolved(port == 22);
        },
        .reverse_lookup = [](std::uint32_t, cardio::cancellation) {
          return cardio::resolved(std::string());
        },
        .multicast_lookup = [](std::uint32_t, elder_terms::IpScanNameSource source,
                               cardio::cancellation) {
          std::vector<elder_terms::IpScanNameCandidate> candidates;
          if (source == elder_terms::IpScanNameSource::mdns) {
            candidates.push_back({"printer.local", 2});
          }
          return cardio::resolved(std::move(candidates));
        },
        .name_lookup_timeout = [](std::uint64_t) {
          return cardio::cancellation_source{};
        },
    };
    co_await elder_terms::scan_ipv4_hosts_async(
        std::move(dependencies),
        {.entry_changed = [&](const elder_terms::IpScanEntry &entry) {
           hosts[entry.address] = entry.resolved_name;
           ++updates;
         },
         .progress_changed = [](const elder_terms::IpScanProgress &) {},
         .completed = [&]() { completed = true; }}, {});
    expect(completed && updates == 4 && hosts.size() == 2 &&
               hosts.at("192.0.2.25") == "printer.local" &&
               hosts.at("192.0.2.26") == "printer.local",
           "hosts sharing one name were merged or lost their IP association");
  } catch (...) {
    *error = std::current_exception();
  }
  group->shutdown();
}

static void scan_keeps_same_name_hosts_separate() {
  cardio::dispatcher_group_glib group;
  cardio::dispatcher_host_glib dispatcher(group);
  std::exception_ptr error;
  auto task = check_same_name_hosts_async(&group, &error);
  dispatcher.park();
  if (error != nullptr) {
    std::rethrow_exception(error);
  }
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
    elder_terms_ip_scanner_test::
        scan_resolves_multicast_names_with_shared_deadlines();
    elder_terms_ip_scanner_test::scan_keeps_same_name_hosts_separate();
  } catch (const std::exception &exception) {
    std::cerr << exception.what() << '\n';
    return 1;
  }
  return 0;
}
