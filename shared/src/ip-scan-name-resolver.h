#pragma once

#include <gio/gio.h>
#include <elder-terms/ip-scanner.h>

namespace elder_terms_ip_scan {

/** Asynchronous multicast callback used by the scanner. */
using MulticastLookup =
    decltype(elder_terms::IpScannerDependencies::multicast_lookup);

/**
 * Creates one scan's multicast lookup adapter.
 *
 * @param bus_type System bus in production, isolated session bus in tests.
 * @return Callback retaining the scan's connection and service availability.
 */
MulticastLookup create_multicast_name_lookup(GBusType bus_type);

} // namespace elder_terms_ip_scan
