// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "nfs-ssh-proxy.h"
#include "nfs-rpc.h"
#include "nfs-api.h"
#include <nfsc/libnfs-raw-portmap.h>
#include <nfsc/libnfs-raw-mount.h>
#include <nfsc/libnfs-raw-nfs.h>
#include <netinet/in.h>
#include <charconv>
#include <cstring>

namespace elder_terms {

static cardio::promise<int> discover_tcp_port(std::shared_ptr<NfsRpcSession> session,
    int program, int version, cardio::cancellation cancellation) {
  std::uint32_t port = 0;
  co_await execute_nfs_raw_async(session, [&](auto cb, auto data) {
    return ELDER_NFS_RAW_CALL(rpc_pmap2_getport, session->rpc, program, version, IPPROTO_TCP, cb, data);
  }, [&](void *data) { std::memcpy(&port, data, sizeof(port)); }, cancellation);
  if (port == 0) {
    // An IPv6-only service may have no IPv4 PORTMAP registration. The returned
    // address is not a new destination: only its port belongs to the configured
    // server, whose hostname continues to be resolved by the SSH gateway.
    char netid[] = "tcp6", empty[] = "";
    pmap3_mapping mapping{static_cast<unsigned>(program), static_cast<unsigned>(version), netid, empty, empty};
    std::string address;
    co_await execute_nfs_raw_async(session, [&](auto cb, auto data) {
      return ELDER_NFS_RAW_CALL(rpc_pmap3_getaddr, session->rpc, &mapping, cb, data);
    }, [&](void *data) {
      pmap3_string_result result{};
      std::memcpy(&result, data, sizeof(result));
      if (result.addr) address = result.addr;
    }, cancellation);
    const auto low_separator = address.find_last_of('.');
    const auto high_separator = low_separator == std::string::npos || low_separator == 0 ? std::string::npos :
        address.find_last_of('.', low_separator - 1);
    if (high_separator != std::string::npos) {
      unsigned high = 256, low = 256;
      const auto *begin = address.data();
      const auto upper = std::from_chars(begin + high_separator + 1, begin + low_separator, high);
      const auto lower = std::from_chars(begin + low_separator + 1, begin + address.size(), low);
      if (upper.ec == std::errc{} && upper.ptr == begin + low_separator && lower.ec == std::errc{} &&
          lower.ptr == begin + address.size() && high <= 255 && low <= 255) port = high * 256 + low;
    }
  }
  if (port == 0 || port > 65535)
    throw RemoteFileError("NFS SSH proxy: no valid TCP port for RPC program " + std::to_string(program) +
        " version " + std::to_string(version), false);
  co_return static_cast<int>(port);
}

cardio::promise<NfsClientOpenOptions> forward_nfs_ssh_ports_async(
    std::shared_ptr<SshTcpProxy> proxy, NfsClientOpenOptions options, cardio::cancellation cancellation) {
  if (!proxy) throw std::invalid_argument("NFS SSH proxy requires an authenticated gateway");
  if (options.require_reserved_port)
    throw RemoteFileError("SSH proxy cannot require reserved NFS source ports; disable the reserved-port requirement or SSH proxy", false);
  if (options.address.empty() || (options.version != 2 && options.version != 3 && options.version != 4) ||
      options.nfs_port < 0 || options.nfs_port > 65535 || options.mount_port < 0 || options.mount_port > 65535 ||
      options.connect_timeout_ms <= 0)
    throw std::invalid_argument("Invalid NFS SSH proxy endpoint");
  const auto host = options.address;
  if (options.version == 4) {
    if (!options.nfs_port) options.nfs_port = 2049;
  } else if (!options.nfs_port || !options.mount_port) {
    const auto port = co_await forward_ssh_tcp_port_async(proxy, host, PMAP_PORT, cancellation);
    auto session = create_nfs_raw_session(options.connect_timeout_ms);
    co_await execute_nfs_raw_async(session, [&](auto cb, auto data) {
      return rpc_connect_async(session->rpc, "127.0.0.1", port, cb, data);
    }, {}, cancellation);
    if (!options.nfs_port)
      options.nfs_port = co_await discover_tcp_port(session, NFS_PROGRAM, options.version, cancellation);
    if (!options.mount_port)
      options.mount_port = co_await discover_tcp_port(session, MOUNT_PROGRAM, options.version == 2 ? MOUNT_V1 : MOUNT_V3, cancellation);
  }
  options.nfs_port = co_await forward_ssh_tcp_port_async(proxy, host, options.nfs_port, cancellation);
  if (options.version != 4)
    options.mount_port = co_await forward_ssh_tcp_port_async(proxy, host, options.mount_port, cancellation);
  // Explicit ports prevent libnfs from discovering or connecting outside the
  // forwarded endpoints, including later lazy mounts of additional exports.
  options.address = "127.0.0.1";
  co_return options;
}

} // namespace elder_terms
