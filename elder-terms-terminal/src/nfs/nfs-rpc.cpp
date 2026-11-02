// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "nfs-rpc.h"
#include <poll.h>
#include <cstring>

namespace elder_terms {

struct NfsCancellationScope {
  cardio::cancellation_source source;
  ~NfsCancellationScope() { (void)source.cancel(); }
};

NfsRpcSession::~NfsRpcSession() { abort(); }
void NfsRpcSession::stop() {
  (void)stopping.cancel();
  if (!active) abort();
}
void NfsRpcSession::abort() {
  // Destruction delivers cancellation callbacks while their state is alive.
  if (nfs) nfs_destroy_context(nfs);
  else if (rpc) rpc_destroy_context(rpc);
  nfs = nullptr;
  rpc = nullptr;
}

std::shared_ptr<NfsRpcSession> create_nfs_rpc_session(int version, int timeout_ms) {
  if ((version != 3 && version != 4) || timeout_ms <= 0)
    throw std::invalid_argument("Invalid NFS version or timeout");
  auto session = std::make_shared<NfsRpcSession>();
  session->nfs = nfs_init_context();
  if (!session->nfs) throw std::bad_alloc();
  session->rpc = nfs_get_rpc_context(session->nfs);
  session->timeout_ms = timeout_ms;
  if (nfs_set_version(session->nfs, version) != 0)
    throw RemoteFileError("The selected NFS version is unavailable", false);
  nfs_set_autoreconnect(session->nfs, 0);
#ifdef LIBNFS_API_V2
  nfs_set_retrans(session->nfs, 0);
#else
  // API v1 has no configurable TCP RPC retransmissions. Reconnect is disabled,
  // and Cardio still bounds the entire operation with an absolute deadline.
  nfs_set_readahead(session->nfs, 0);
  nfs_set_pagecache(session->nfs, 0);
#endif
  nfs_set_dircache(session->nfs, 0);
  configure_nfs_ports(session, 0, 0);
  nfs_set_timeout(session->nfs, timeout_ms);
  return session;
}
std::shared_ptr<NfsRpcSession> create_nfs_raw_session(int timeout_ms) {
  if (timeout_ms <= 0) throw std::invalid_argument("Invalid RPC timeout");
  auto session = std::make_shared<NfsRpcSession>();
  session->rpc = rpc_init_context();
  if (!session->rpc) throw std::bad_alloc();
  session->timeout_ms = timeout_ms;
#ifdef LIBNFS_API_V2
  rpc_set_timeout(session->rpc, timeout_ms);
#endif
  // API v1 does not publish rpc_set_timeout(). The Cardio deadline applies to
  // raw contexts too, and retires callbacks before their storage is released.
  return session;
}

void configure_nfs_ports(const std::shared_ptr<NfsRpcSession> &session, int nfs_port, int mount_port) {
  if (!session->nfs || nfs_port < 0 || nfs_port > 65535 || mount_port < 0 || mount_port > 65535)
    throw std::invalid_argument("Invalid NFS service port");
#ifdef LIBNFS_API_V2
  nfs_set_auto_traverse_mounts(session->nfs, 0);
  nfs_set_nfsport(session->nfs, nfs_port);
  nfs_set_mountport(session->nfs, mount_port);
#else
  const auto options = "nfs:///?auto-traverse-mounts=0&nfsport=" + std::to_string(nfs_port) +
      "&mountport=" + std::to_string(mount_port);
  auto *parsed = nfs_parse_url_incomplete(session->nfs, options.c_str());
  if (!parsed) throw RemoteFileError(nfs_get_error(session->nfs), false);
  nfs_destroy_url(parsed);
#endif
}

struct NfsCompletion {
  bool done = false;
  NfsRpcResult result;
  std::function<void(int, void *)> consume;
  std::exception_ptr failure;
};

static void complete(NfsCompletion &completion, int status, void *data, bool success) {
  completion.result.status = status;
  try {
    if (success && completion.consume) completion.consume(status, data);
    else if (!success && data) completion.result.message = static_cast<const char *>(data);
  } catch (...) { completion.failure = std::current_exception(); }
  completion.done = true;
}
static void high_completed(int status, nfs_context *, void *data, void *opaque) {
  complete(*static_cast<NfsCompletion *>(opaque), status, data, status >= 0);
}
static void raw_completed(rpc_context *, int status, void *data, void *opaque) {
  complete(*static_cast<NfsCompletion *>(opaque), status, data, status == RPC_STATUS_SUCCESS);
}

static cardio::promise<void> wait_nfs_async(const std::shared_ptr<NfsRpcSession> &session,
    NfsCompletion &completion, cardio::cancellation cancellation) {
  NfsCancellationScope deadline{cardio::cancellations::timeout(session->timeout_ms)};
  NfsCancellationScope stopped{cardio::cancellations::any(cancellation, session->stopping.get_cancellation())};
  NfsCancellationScope waiting{cardio::cancellations::any(stopped.source.get_cancellation(), deadline.source.get_cancellation())};
  try {
    while (!completion.done) {
      const auto fd = rpc_get_fd(session->rpc);
      const auto wanted = rpc_which_events(session->rpc);
      if (fd < 0) throw RemoteFileError("NFS socket is unavailable", true);
      auto interest = cardio::fd_event::none;
      if (wanted & POLLIN) interest |= cardio::fd_event::read;
      if (wanted & POLLOUT) interest |= cardio::fd_event::write;
      const auto ready = co_await cardio::from_fd(fd, interest, waiting.source.get_cancellation());
      int events = 0;
      if ((ready & cardio::fd_event::read) != cardio::fd_event::none) events |= POLLIN;
      if ((ready & cardio::fd_event::write) != cardio::fd_event::none) events |= POLLOUT;
      if ((ready & cardio::fd_event::error) != cardio::fd_event::none) events |= POLLERR;
      if ((ready & cardio::fd_event::hangup) != cardio::fd_event::none) events |= POLLHUP;
      const int status = session->nfs ? nfs_service(session->nfs, events) : rpc_service(session->rpc, events);
      if (status < 0) throw RemoteFileError(rpc_get_error(session->rpc), true);
    }
  } catch (const cardio::canceled_exception &) {
    // No retries or background service exist: the absolute request deadline also
    // bounds the library's RPC timers, even when no socket event arrives.
    session->abort();
    session->active = false;
    if (stopped.source.get_cancellation().is_cancellation_requested()) throw;
    throw RemoteFileError("NFS request timed out; server-side completion may be uncertain. Reconnect to continue.", true);
  } catch (...) {
    session->abort();
    session->active = false;
    throw;
  }
  session->active = false;
  if (completion.failure) std::rethrow_exception(completion.failure);
}

static void begin_nfs(NfsRpcSession &session, cardio::cancellation cancellation) {
  cancellation.throw_if_cancellation_requested();
  if (!session.rpc || session.stopping.get_cancellation().is_cancellation_requested())
    throw RemoteFileError("NFS connection has ended. Reconnect to continue.", true);
  if (session.active) throw std::logic_error("NFS context is already processing a request");
  session.active = true;
}

cardio::promise<NfsRpcResult> execute_nfs_async(std::shared_ptr<NfsRpcSession> session,
    std::function<int(nfs_cb, void *)> start, std::function<void(int, void *)> consume,
    cardio::cancellation cancellation) {
  begin_nfs(*session, cancellation);
  NfsCompletion completion;
  completion.consume = std::move(consume);
  try {
    const int status = start(high_completed, &completion);
    if (status < 0) {
      session->active = false;
      co_return NfsRpcResult{status, nfs_get_error(session->nfs)};
    }
    co_await wait_nfs_async(session, completion, cancellation);
  } catch (...) {
    if (session->active) { session->abort(); session->active = false; }
    throw;
  }
  co_return completion.result;
}

cardio::promise<void> execute_nfs_raw_async(std::shared_ptr<NfsRpcSession> session,
    std::function<int(rpc_cb, void *)> start, std::function<void(void *)> consume,
    cardio::cancellation cancellation) {
  begin_nfs(*session, cancellation);
  NfsCompletion completion;
  completion.consume = [consume = std::move(consume)](int, void *data) { if (consume) consume(data); };
  try {
    if (start(raw_completed, &completion) < 0) {
      session->active = false;
      throw RemoteFileError(rpc_get_error(session->rpc), false);
    }
    co_await wait_nfs_async(session, completion, cancellation);
  } catch (...) {
    if (session->active) { session->abort(); session->active = false; }
    throw;
  }
  if (completion.result.status != RPC_STATUS_SUCCESS) {
    session->abort();
    throw RemoteFileError("NFS RPC failure: " + completion.result.message, true);
  }
}

cardio::promise<void> connect_nfs_raw_async(std::shared_ptr<NfsRpcSession> session,
    std::string host, int port, int program, int version, cardio::cancellation cancellation) {
  co_await execute_nfs_raw_async(session, [&](auto cb, auto data) {
    return port == 0 ? rpc_connect_program_async(session->rpc, host.c_str(), program, version, cb, data)
        : rpc_connect_port_async(session->rpc, host.c_str(), port, program, version, cb, data);
  }, {}, cancellation);
}

void check_nfs_result(const NfsRpcResult &result, const char *operation) {
  if (result.status >= 0) return;
  const int error = -result.status;
  const bool lost = error == ECONNRESET || error == ENOTCONN || error == EPIPE || error == ETIMEDOUT || error == ESTALE;
  throw RemoteFileError(std::string("NFS ") + operation + ": " + result.message, lost);
}

cardio::promise<std::size_t> read_nfs_async(std::shared_ptr<NfsRpcSession> session,
    nfsfh *handle, std::span<std::byte> buffer, std::uint64_t offset, cardio::cancellation cancellation) {
  const auto result = co_await execute_nfs_async(session, [&](auto cb, auto data) {
#ifdef LIBNFS_API_V2
    return nfs_pread_async(session->nfs, handle, buffer.data(), buffer.size(), offset, cb, data);
#else
    return nfs_pread_async(session->nfs, handle, offset, buffer.size(), cb, data);
#endif
  }, [&](int count, void *data) {
    if (static_cast<std::size_t>(count) > buffer.size())
      throw RemoteFileError("NFS read returned more bytes than requested", true);
#ifndef LIBNFS_API_V2
    if (count != 0) std::memcpy(buffer.data(), data, count);
#else
    (void)data;
#endif
  }, cancellation);
  check_nfs_result(result, "read");
  co_return static_cast<std::size_t>(result.status);
}

} // namespace elder_terms
