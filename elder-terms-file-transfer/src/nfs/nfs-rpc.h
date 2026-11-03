// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once

#include "../file-transfer/remote-file-client.h"
#include <nfsc/libnfs.h>
#include <nfsc/libnfs-raw.h>
#include <functional>
#include <span>
#include <set>

namespace elder_terms {

/** Owns one event-loop-only libnfs context and all outstanding callbacks. */
struct NfsRpcSession {
  /** High-level context, null for raw RPC sessions. */
  nfs_context *nfs = nullptr;
  /** RPC context; owned by nfs when nfs is nonnull. */
  rpc_context *rpc = nullptr;
  /** Serializes complete filesystem operations, including their streams. */
  cardio::primitives::mutex operations;
  /** Cancels pending readiness waits before the context is destroyed. */
  cardio::cancellation_source stopping;
  /** Hard deadline for a logical request, in milliseconds. */
  int timeout_ms = 10000;
  /** True while a coroutine owns callback state. */
  bool active = false;
  /** Reject RPC traffic unless the actual TCP source port is below 1024. */
  bool require_reserved_port = false;
  /** Open high-level handles not yet transferred to a close callback. */
  std::set<nfsfh *> handles;
  /** Releases the context after pending callbacks have been drained. */
  ~NfsRpcSession();
  /** Stops pending work; active callback state remains owned by its coroutine. */
  void stop();
  /** Destroys the context and drains callbacks; used by the active operation. */
  void abort();
};

/** Completion status and an owned diagnostic, without borrowed callback data. */
struct NfsRpcResult {
  /** High-level result (negative errno), or RAW RPC status. */
  int status = 0;
  /** Owned error text. */
  std::string message;
};

/**
 * Creates an NFS v3/v4.0 context without automatic reconnect or background threads.
 * @param version Exactly 3 or 4 (minor version 0).
 * @param timeout_ms Request deadline in milliseconds.
 * @returns Session to configure before asynchronously mounting an export.
 */
std::shared_ptr<NfsRpcSession> create_nfs_rpc_session(int version, int timeout_ms);

/**
 * Creates a raw RPC context for MOUNT or NFS v2, without protocol fallback.
 * @param timeout_ms Request deadline in milliseconds.
 * @returns Unconnected TCP RPC session.
 */
std::shared_ptr<NfsRpcSession> create_nfs_raw_session(int timeout_ms);

/**
 * Configures ports and disables implicit mounting of nested exports.
 * @param session Unmounted high-level session.
 * @param nfs_port Zero for discovery/default, or an explicit NFS service port.
 * @param mount_port Zero for discovery, or an explicit MOUNT service port.
 * @remarks libnfs 5 exposes these options only through its public URL parser.
 * Only a constant ASCII configuration URL is parsed; server, export, and file
 * names are always passed separately as their original bytes.
 */
void configure_nfs_ports(const std::shared_ptr<NfsRpcSession> &session, int nfs_port, int mount_port);

/**
 * Reads into caller-owned storage on either public libnfs API generation.
 * @param session Mounted high-level context.
 * @param handle Open file handle owned by the caller.
 * @param buffer Destination storage retained until completion.
 * @param offset Byte offset within the remote file.
 * @param cancellation Cancellation signal.
 * @returns Bytes copied, or zero at EOF.
 */
cardio::promise<std::size_t> read_nfs_async(std::shared_ptr<NfsRpcSession> session,
    nfsfh *handle, std::span<std::byte> buffer, std::uint64_t offset, cardio::cancellation cancellation);

/**
 * Awaits one high-level operation using Cardio socket readiness and a deadline.
 * @param session Context kept alive until callback retirement.
 * @param start Queues one asynchronous operation; negative means not queued.
 * @param consume Copies callback-owned success data before it expires.
 * @param cancellation Cancellation ends this context, never replays the request.
 * @returns Status; transport failure or cancellation throws after draining callbacks.
 * @remarks Caller holds operations across any multi-request logical operation.
 */
cardio::promise<NfsRpcResult> execute_nfs_async(std::shared_ptr<NfsRpcSession> session,
    std::function<int(nfs_cb, void *)> start, std::function<void(int, void *)> consume,
    cardio::cancellation cancellation);

/**
 * Awaits a raw RPC task or connect, copying its data within the callback.
 * @param session Raw session kept alive until callback retirement.
 * @param start Queues the request; returns zero on success, negative on failure.
 * @param consume Copies success data; may throw a protocol error.
 * @param cancellation Cancellation ends this context without retrying a mutation.
 * @returns Successful RPC completion; RPC errors throw with their diagnostics.
 */
cardio::promise<void> execute_nfs_raw_async(std::shared_ptr<NfsRpcSession> session,
    std::function<int(rpc_cb, void *)> start, std::function<void(void *)> consume,
    cardio::cancellation cancellation);

/**
 * Connects to the requested RPC program with port discovery or a fixed port.
 * @param session Raw TCP session.
 * @param host Server hostname or numeric address.
 * @param port Zero for PORTMAP discovery, otherwise an explicit service port.
 * @param program RPC program number.
 * @param version Exact RPC program version.
 * @param cancellation Cancellation signal.
 */
cardio::promise<void> connect_nfs_raw_async(std::shared_ptr<NfsRpcSession> session,
    std::string host, int port, int program, int version, cardio::cancellation cancellation);

/**
 * Converts a high-level negative errno result into a remote error.
 * @param result Completed operation.
 * @param operation Human-readable operation name.
 */
void check_nfs_result(const NfsRpcResult &result, const char *operation);

} // namespace elder_terms
