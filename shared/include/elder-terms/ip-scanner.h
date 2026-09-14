#pragma once

#include <cardio.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <elder-terms/export.h>

namespace elder_terms {

/**
 * IPv4 address and netmask assigned to a network interface.
 *
 * Both values use host byte order with the most significant IPv4 octet in the
 * most significant byte.
 */
struct Ipv4InterfaceAddress {
  /** Assigned IPv4 address. */
  std::uint32_t address;

  /** Contiguous IPv4 network mask. */
  std::uint32_t netmask;
};

/** Inclusive IPv4 address interval. */
struct Ipv4ScanRange {
  /** First address in the interval. */
  std::uint32_t first;

  /** Last address in the interval. */
  std::uint32_t last;
};

/** Normalized set of IPv4 addresses to scan. */
struct Ipv4ScanPlan {
  /** Sorted, non-overlapping address intervals. */
  std::vector<Ipv4ScanRange> ranges;

  /** Total number of addresses represented by ranges. */
  std::uint64_t total_addresses;

  /** Number of IPv4 interfaces ignored because their netmask was invalid. */
  std::size_t ignored_interface_count;
};

/** Origin of a hostname discovered by an IP scan. */
enum class IpScanNameSource {
  /** No name was resolved. */
  none,
  /** The operating system's default resolver returned the name. */
  system,
  /** Multicast DNS returned the name. */
  mdns,
  /** Link-local multicast name resolution returned the name. */
  llmnr,
};

/** One multicast name and the interface on which it was discovered. */
struct IpScanNameCandidate {
  /** Hostname exactly as returned by the resolver. */
  std::string name;

  /** Network interface index associated with this candidate. */
  int interface_index;
};

/** Discovered host and its services. */
struct IpScanEntry {
  /** Numeric IPv4 address. */
  std::string address;

  /** Resolved hostname (including single-label names), or an empty string. */
  std::string resolved_name;

  /** Open standard service ports in ascending order. */
  std::vector<std::uint16_t> open_ports;

  /** Origin of the resolved hostname, or none while unnamed. */
  IpScanNameSource name_source = IpScanNameSource::none;
};

/** Current IPv4 scan progress. */
struct IpScanProgress {
  /** Number of addresses whose probes and name lookups have completed. */
  std::uint64_t completed_addresses;

  /** Total number of unique addresses in the scan plan. */
  std::uint64_t total_addresses;

  /** Number of interfaces omitted due to an invalid IPv4 netmask. */
  std::size_t ignored_interface_count;
};

/** Injectable scanner operations and configuration. */
struct IpScannerDependencies {
  /** IPv4 interface addresses from which the scan plan is built. */
  std::vector<Ipv4InterfaceAddress> interfaces;

  /** Maximum number of hosts whose operations may be active concurrently. */
  std::size_t maximum_concurrent_hosts;

  /** Asynchronously checks whether a TCP port accepts a connection. */
  std::function<cardio::promise<bool>(std::uint32_t, std::uint16_t,
                                      cardio::cancellation)>
      probe_port;

  /** Asynchronously obtains an IPv4 name through the system resolver. */
  std::function<cardio::promise<std::string>(std::uint32_t,
                                             cardio::cancellation)>
      reverse_lookup;

  /**
   * Asynchronously obtains mDNS or LLMNR candidates for an IPv4 address.
   * An empty callback disables explicit multicast lookup.
   */
  std::function<cardio::promise<std::vector<IpScanNameCandidate>>(
      std::uint32_t, IpScanNameSource, cardio::cancellation)>
      multicast_lookup = {};

  /**
   * Creates a name-lookup deadline from a duration in milliseconds.
   * Tests may inject controlled deadline notifications instead of a clock.
   */
  std::function<cardio::cancellation_source(std::uint64_t)>
      name_lookup_timeout = [](std::uint64_t milliseconds) {
        return cardio::cancellations::timeout(milliseconds);
      };
};

/** Scanner result callbacks, invoked on the current Cardio dispatcher. */
struct IpScannerCallbacks {
  /** Reports a new entry and a later update when name lookup succeeds. */
  std::function<void(const IpScanEntry &)> entry_changed;

  /** Reports initial progress and each completed host. */
  std::function<void(const IpScanProgress &)> progress_changed;

  /** Reports successful completion. It is not invoked after cancellation. */
  std::function<void()> completed;
};

/**
 * Creates a sorted scan plan from configured IPv4 interfaces.
 *
 * @param interfaces IPv4 interface addresses and masks.
 * @return Merged inclusive address ranges and their total size.
 *
 * @remarks Network and broadcast addresses are intentionally retained. A
 * range with at most eight host bits is scanned in full. For a wider range,
 * excess upper host bits are fixed to zero and only its first 256 addresses
 * are included. A loopback interface contributes only its assigned address.
 * Interfaces with non-contiguous masks are counted and ignored.
 */
ELDER_TERMS_API Ipv4ScanPlan
create_ipv4_scan_plan(const std::vector<Ipv4InterfaceAddress> &interfaces);

/**
 * Asynchronously scans the standard FTP, SSH, and TELNET TCP ports.
 *
 * @param dependencies Interface data, concurrency, and asynchronous services.
 * @param callbacks Incremental result, progress, and completion callbacks.
 * @param cancellation Cancellation signal for every pending operation.
 * @return Promise resolved after all hosts finish.
 *
 * @remarks Ports 21, 22, and 23 are probed concurrently for each host. Name
 * lookup is attempted only for hosts with at least one open port. A discovered
 * host is reported before its name lookups complete.
 */
ELDER_TERMS_API cardio::promise<void>
scan_ipv4_hosts_async(IpScannerDependencies dependencies,
                      IpScannerCallbacks callbacks,
                      cardio::cancellation cancellation);

/**
 * Asynchronously probes one numeric IPv4 TCP endpoint through GIO.
 *
 * @param address IPv4 address in host byte order.
 * @param port TCP port number.
 * @param timeout_milliseconds Connection timeout in milliseconds.
 * @param cancellation Caller cancellation signal.
 * @return Promise resolving true only when the connection succeeds.
 */
ELDER_TERMS_API cardio::promise<bool>
probe_ipv4_tcp_port_async(std::uint32_t address, std::uint16_t port,
                          std::uint64_t timeout_milliseconds,
                          cardio::cancellation cancellation);

/**
 * Creates production scanner dependencies from this machine's interfaces.
 *
 * @return Dependencies using getifaddrs and asynchronous GIO network calls.
 * @throws std::system_error When the network interface list cannot be read.
 */
ELDER_TERMS_API IpScannerDependencies
create_system_ip_scanner_dependencies();

} // namespace elder_terms
