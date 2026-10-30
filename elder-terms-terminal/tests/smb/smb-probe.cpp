// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include <cardio.h>
#include <fcntl.h>
#include <poll.h>
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#include <smb2/libsmb2-raw.h>
#include <array>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

static void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

struct Completion {
  bool done = false;
  int status = 0;
  unsigned callbacks = 0;
  void *data = nullptr;
  std::function<void(void *)> consume;
  std::exception_ptr failure;
  std::string detail;
};

static void completed(smb2_context *context, int status, void *data, void *opaque) {
  auto &result = *static_cast<Completion *>(opaque);
  result.status = status;
  ++result.callbacks;
  result.data = data;
  if (status < -4095) result.detail = nterror_to_str(static_cast<uint32_t>(status));
  else if (status < 0) result.detail = smb2_get_error(context);
  try { if (status == 0 && result.consume) result.consume(data); }
  catch (...) { result.failure = std::current_exception(); }
  result.done = true;
}

struct ProbeSession {
  smb2_context *context = smb2_init_context();
  ProbeSession() = default;
  ProbeSession(const ProbeSession &) = delete;
  ProbeSession &operator=(const ProbeSession &) = delete;
  ~ProbeSession() { if (context) smb2_destroy_context(context); }
};

// All callbacks and FD watches are retired before the callback state is released.
static cardio::promise<Completion> execute_async(ProbeSession &session,
    std::function<int(smb2_command_cb, void *)> start,
    std::function<void(void *)> consume, cardio::cancellation cancellation) {
  cancellation.throw_if_cancellation_requested();
  require(session.context != nullptr, "Session has ended");
  Completion result;
  result.consume = std::move(consume);
  const int initiated = start(completed, &result);
  if (initiated < 0) { result.done = true; result.status = initiated; result.detail = smb2_get_error(session.context); }
  auto deadline = cardio::cancellations::timeout(10000);
  auto stopped = cardio::cancellations::any(cancellation, deadline.get_cancellation());
  try {
    while (!result.done) {
      const int fd = smb2_get_fd(session.context);
      const int wanted = smb2_which_events(session.context);
      auto interest = cardio::fd_event::none;
      if (wanted & POLLIN) interest |= cardio::fd_event::read;
      if (wanted & POLLOUT) interest |= cardio::fd_event::write;
      auto ready = co_await cardio::from_fd(fd, interest, stopped.get_cancellation());
      int events = 0;
      if ((ready & cardio::fd_event::read) != cardio::fd_event::none) events |= POLLIN;
      if ((ready & cardio::fd_event::write) != cardio::fd_event::none) events |= POLLOUT;
      if ((ready & cardio::fd_event::error) != cardio::fd_event::none) events |= POLLERR;
      if ((ready & cardio::fd_event::hangup) != cardio::fd_event::none) events |= POLLHUP;
      // A failed connect callback may close the socket inside service(). Keep
      // its specific SMB status instead of replacing it with a later FD error.
      if (smb2_service(session.context, events) < 0 && !(result.done && result.status != 0))
        throw std::runtime_error(smb2_get_error(session.context));
    }
  } catch (...) {
    // Destroy aborts pending commands before their callback state disappears.
    smb2_destroy_context(session.context);
    session.context = nullptr;
    require(result.done && result.callbacks == 1, "Aborted command callback was not retired exactly once");
    throw;
  }
  (void)deadline.cancel();
  if (result.failure) std::rethrow_exception(result.failure);
  co_return result;
}

static cardio::promise<void> cancel_on_input_async(cardio::cancellation_source &source) {
  (void)co_await cardio::from_fd(STDIN_FILENO, cardio::fd_event::read);
  char byte;
  require(::read(STDIN_FILENO, &byte, 1) == 1, "Expected cancellation trigger");
  (void)source.cancel();
}

static void success(const Completion &completion, const char *operation = "operation") {
  if (completion.status != 0)
    throw std::runtime_error("SMB " + std::string(operation) + " failed: " +
        std::to_string(completion.status) + " " + completion.detail);
}

static cardio::promise<std::vector<std::string>> resolve_async(std::string host, cardio::cancellation cancellation) {
  auto resolver = std::shared_ptr<GResolver>(g_resolver_get_default(), g_object_unref);
  auto pending = cardio::gio::submit<GList *>(
      [resolver, host](GCancellable *cancel, GAsyncReadyCallback callback, gpointer data) {
        g_resolver_lookup_by_name_async(resolver.get(), host.c_str(), cancel, callback, data);
      },
      [](GObject *object, GAsyncResult *result, GError **error) {
        return g_resolver_lookup_by_name_finish(G_RESOLVER(object), result, error);
      }, cancellation);
  auto *addresses = co_await pending;
  require(addresses != nullptr, "No resolved address");
  std::vector<std::string> result;
  for (auto *entry = addresses; entry != nullptr; entry = entry->next) {
    gchar *address = g_inet_address_to_string(G_INET_ADDRESS(entry->data));
    result.emplace_back(address);
    g_free(address);
  }
  g_resolver_free_addresses(addresses);
  cancellation.throw_if_cancellation_requested();
  co_return result;
}

static void configure(ProbeSession &session, const std::string &user, const std::string &mode) {
  require(session.context != nullptr, "SMB context allocation failed");
  smb2_set_user(session.context, user.c_str());
  smb2_set_password(session.context, g_getenv("ELDER_SMB_PASSWORD"));
  smb2_set_domain(session.context, "");
  // The documented URL option selects NTLMSSP without private enum values.
  auto *options = smb2_parse_url(session.context, "smb://localhost/share?sec=ntlmssp");
  require(options != nullptr, "Authentication option is unavailable");
  smb2_destroy_url(options);
  smb2_set_sign(session.context, 1);
  smb2_set_security_mode(session.context, SMB2_NEGOTIATE_SIGNING_ENABLED | SMB2_NEGOTIATE_SIGNING_REQUIRED);
  if (mode == "encrypt" || mode == "encrypt2") smb2_set_seal(session.context, 1);
  if (mode == "smb2" || mode == "encrypt2") smb2_set_version(session.context, SMB2_VERSION_0210);
}

static cardio::promise<std::string> connect_async(ProbeSession &session,
    const std::vector<std::string> &addresses, const std::string &port,
    const std::string &share, const std::string &user, const std::string &mode,
    cardio::cancellation cancellation) {
  std::exception_ptr last_failure;
  for (const auto &ip : addresses) {
    cancellation.throw_if_cancellation_requested();
    if (session.context) smb2_destroy_context(session.context);
    session.context = smb2_init_context();
    configure(session, user, mode);
    const auto address = (ip.find(':') != std::string::npos ? "[" + ip + "]" : ip) + ":" + port;
    try {
      success(co_await execute_async(session, [&](auto cb, auto data) {
        return smb2_connect_share_async(session.context, address.c_str(), share.c_str(), user.c_str(), cb, data);
      }, {}, cancellation), "connect");
      co_return address;
    } catch (...) {
      cancellation.throw_if_cancellation_requested();
      last_failure = std::current_exception();
    }
  }
  if (last_failure) std::rethrow_exception(last_failure);
  throw std::runtime_error("No usable server address");
}

static cardio::promise<smb2_create_reply> fault_open_raw_async(
    ProbeSession &session, const std::string &path, uint32_t access,
    uint32_t disposition) {
  smb2_create_request request{};
  request.impersonation_level = SMB2_IMPERSONATION_IMPERSONATION;
  request.desired_access = access;
  request.share_access = SMB2_FILE_SHARE_READ | SMB2_FILE_SHARE_WRITE | SMB2_FILE_SHARE_DELETE;
  request.create_disposition = disposition;
  request.create_options = SMB2_FILE_OPEN_REPARSE_POINT;
  request.name = path.c_str();
  smb2_create_reply handle{};
  success(co_await execute_async(session, [&](auto cb, auto data) {
    auto *pdu = smb2_cmd_create_async(session.context, &request, cb, data);
    if (!pdu) return -ENOMEM;
    smb2_queue_pdu(session.context, pdu);
    return 0;
  }, [&](void *data) { handle = *static_cast<smb2_create_reply *>(data); }, {}), "fault create");
  co_return handle;
}

static cardio::promise<uint32_t> fault_write_raw_async(
    ProbeSession &session, const smb2_create_reply &handle,
    const std::array<uint8_t, 5> &bytes, cardio::cancellation cancellation) {
  smb2_write_request request{};
  request.length = bytes.size();
  request.buf = bytes.data();
  std::memcpy(request.file_id, handle.file_id, SMB2_FD_SIZE);
  uint32_t count = 0;
  success(co_await execute_async(session, [&](auto cb, auto data) {
    auto *pdu = smb2_cmd_write_async(session.context, &request, 0, cb, data);
    if (!pdu) return -ENOMEM;
    smb2_queue_pdu(session.context, pdu);
    return 0;
  }, [&](void *data) { count = static_cast<smb2_write_reply *>(data)->count; }, cancellation), "fault write");
  co_return count;
}

static cardio::promise<uint32_t> fault_read_raw_async(
    ProbeSession &session, const smb2_create_reply &handle,
    std::array<uint8_t, 5> &bytes, cardio::cancellation cancellation) {
  smb2_read_request request{};
  request.length = bytes.size();
  request.buf = bytes.data();
  std::memcpy(request.file_id, handle.file_id, SMB2_FD_SIZE);
  uint32_t count = 0;
  success(co_await execute_async(session, [&](auto cb, auto data) {
    auto *pdu = smb2_cmd_read_async(session.context, &request, cb, data);
    if (!pdu) return -ENOMEM;
    smb2_queue_pdu(session.context, pdu);
    return 0;
  }, [&](void *data) { count = static_cast<smb2_read_reply *>(data)->data_length; }, cancellation), "fault read");
  co_return count;
}

static cardio::promise<void> fault_close_raw_async(
    ProbeSession &session, const smb2_create_reply &handle) {
  smb2_close_request request{};
  std::memcpy(request.file_id, handle.file_id, SMB2_FD_SIZE);
  success(co_await execute_async(session, [&](auto cb, auto data) {
    auto *pdu = smb2_cmd_close_async(session.context, &request, cb, data);
    if (!pdu) return -ENOMEM;
    smb2_queue_pdu(session.context, pdu);
    return 0;
  }, {}, {}), "fault close");
}

static cardio::promise<void> transfer_fault_async(ProbeSession &session,
    const std::string &root, const std::string &address, const std::string &share,
    const std::string &user, const std::string &mode) {
  const std::string path = root + "/interrupted.bin";
  auto handle = co_await fault_open_raw_async(session, path,
      SMB2_FILE_READ_DATA | SMB2_FILE_WRITE_DATA, SMB2_FILE_CREATE);
  std::array<uint8_t, 5> bytes{'b', 'e', 'f', 'o', 'r'};
  require(co_await fault_write_raw_async(session, handle, bytes, {}) == 5,
          "Fault fixture creation failed");
  std::cout << "READY transfer fault " << mode << std::endl;
  (void)co_await cardio::from_fd(STDIN_FILENO, cardio::fd_event::read);
  char gate;
  require(::read(STDIN_FILENO, &gate, 1) == 1, "Expected fault fixture readiness");
  cardio::cancellation_source cancellation;
  cardio::promise<void> cancel_task;
  if (mode != "disconnect-read") cancel_task = cancel_on_input_async(cancellation);
  bytes = {'a', 'f', 't', 'e', 'r'};
  bool interrupted = false;
  try {
    if (mode == "cancel-write")
      (void)co_await fault_write_raw_async(session, handle, bytes,
                                          cancellation.get_cancellation());
    else
      (void)co_await fault_read_raw_async(session, handle, bytes,
                                         cancellation.get_cancellation());
  } catch (const cardio::canceled_exception &) {
    require(mode != "disconnect-read", "Disconnect was mistaken for cancellation");
    interrupted = true;
  } catch (const std::runtime_error &) {
    require(mode == "disconnect-read", "Transfer cancellation failed");
    interrupted = true;
  }
  if (mode != "disconnect-read") co_await cancel_task;
  require(interrupted && session.context == nullptr, "Interrupted transfer kept its session");
  ProbeSession recovered;
  configure(recovered, user, "basic");
  success(co_await execute_async(recovered, [&](auto cb, auto data) {
    return smb2_connect_share_async(recovered.context, address.c_str(), share.c_str(), user.c_str(), cb, data);
  }, {}, {}), "reconnect");
  handle = co_await fault_open_raw_async(recovered, path,
      SMB2_FILE_READ_DATA, SMB2_FILE_OPEN);
  const auto count = co_await fault_read_raw_async(recovered, handle, bytes, {});
  require(count == 5 && std::string(bytes.begin(), bytes.end()) ==
      (mode == "cancel-write" ? "after" : "befor"), "Interrupted transfer content differs");
  co_await fault_close_raw_async(recovered, handle);
  success(co_await execute_async(recovered, [&](auto cb, auto data) { return smb2_unlink_async(recovered.context, path.c_str(), cb, data); }, {}, {}));
  success(co_await execute_async(recovered, [&](auto cb, auto data) { return smb2_rmdir_async(recovered.context, root.c_str(), cb, data); }, {}, {}));
  std::cout << "PASS " << mode << " reconnect/content/owned cleanup\n";
}

static cardio::promise<void> probe_async(std::string host, std::string share,
    std::string user, std::string port, std::string mode) {
  if (mode == "dns-cancel") {
    cardio::cancellation_source cancellation;
    auto resolving = resolve_async(host, cancellation.get_cancellation());
    (void)cancellation.cancel();
    bool canceled = false;
    try { (void)co_await resolving; }
    catch (const cardio::canceled_exception &) { canceled = true; }
    require(canceled, "DNS lookup ignored cancellation");
    std::cout << "PASS DNS cancellation\n";
    co_return;
  }
  ProbeSession session;
  auto dns_deadline = cardio::cancellations::timeout(10000);
  auto addresses = co_await resolve_async(host, dns_deadline.get_cancellation());
  (void)dns_deadline.cancel();
  std::string address;
  cardio::cancellation_source cancellation;
  cardio::promise<void> cancel_task;
  if (mode == "cancel") cancel_task = cancel_on_input_async(cancellation);
  bool canceled = false;
  try {
    address = co_await connect_async(session, addresses, port, share, user, mode, cancellation.get_cancellation());
  } catch (const cardio::canceled_exception &) {
    require(mode == "cancel" || mode == "timeout", "Unexpected cancellation");
    require(session.context == nullptr, "Canceled session still alive");
    canceled = true;
  }
  if (canceled) {
    if (mode == "cancel") co_await cancel_task;
    std::cout << "PASS " << mode << " retired callback and FD\n";
    co_return;
  }
  require(mode != "cancel" && mode != "timeout", "Fault fixture unexpectedly connected");
  std::cout << "connected dialect=" << smb2_get_dialect(session.context) << '\n';
  auto listed = co_await execute_async(session, [&](auto cb, auto data) {
    return smb2_opendir_async(session.context, "", cb, data);
  }, {}, {});
  success(listed);
  auto *directory = static_cast<smb2dir *>(listed.data);
  std::size_t entries = 0;
  while (smb2_readdir(session.context, directory)) ++entries;
  smb2_closedir(session.context, directory);
  std::cout << "listed entries=" << entries << '\n';
  if (const char *reparse = g_getenv("ELDER_SMB_REPARSE_PATH")) {
    smb2_create_request request{};
    request.impersonation_level = SMB2_IMPERSONATION_IMPERSONATION;
    request.desired_access = SMB2_FILE_READ_ATTRIBUTES;
    request.share_access = SMB2_FILE_SHARE_READ | SMB2_FILE_SHARE_WRITE | SMB2_FILE_SHARE_DELETE;
    request.create_disposition = SMB2_FILE_OPEN;
    request.create_options = SMB2_FILE_OPEN_REPARSE_POINT;
    request.name = reparse;
    smb2_create_reply handle{};
    auto inspected = co_await execute_async(session, [&](auto cb, auto data) {
      auto *pdu = smb2_cmd_create_async(session.context, &request, cb, data);
      if (!pdu) return -ENOMEM;
      smb2_queue_pdu(session.context, pdu);
      return 0;
    }, [&](void *data) { handle = *static_cast<smb2_create_reply *>(data); }, {});
    if (inspected.status == 0) {
      require((handle.file_attributes & SMB2_FILE_ATTRIBUTE_REPARSE_POINT) != 0, "No-follow open returned a regular target");
      smb2_close_request close{};
      std::memcpy(close.file_id, handle.file_id, SMB2_FD_SIZE);
      success(co_await execute_async(session, [&](auto cb, auto data) {
        auto *pdu = smb2_cmd_close_async(session.context, &close, cb, data);
        if (!pdu) return -ENOMEM;
        smb2_queue_pdu(session.context, pdu);
        return 0;
      }, {}, {}));
      std::cout << "PASS reparse attribute without traversal\n";
    } else {
      std::cout << "PASS no-follow open rejected reparse status=" << inspected.status << '\n';
    }
  }
  const std::string root = "elder-probe-" + std::to_string(getpid());
  success(co_await execute_async(session, [&](auto cb, auto data) {
    return smb2_mkdir_async(session.context, root.c_str(), cb, data);
  }, {}, {}), "mkdir");
  if (mode == "cancel-read" || mode == "cancel-write" || mode == "disconnect-read") {
    co_await transfer_fault_async(session, root, address, share, user, mode);
    co_return;
  }
  const std::string source = root + "/日本語 space.tmp";
  const std::string destination = root + "/CoMmItTeD.txt";
  // Exercise empty files and multiple chunks without depending on packet sizes.
  for (const std::size_t size : {std::size_t{0}, std::size_t{1024 * 1024 + 37}}) {
    const std::string path = root + "/size-" + std::to_string(size);
    auto created = co_await execute_async(session, [&](auto cb, auto data) {
      return smb2_open_async(session.context, path.c_str(), O_RDWR | O_CREAT | O_EXCL, cb, data);
    }, {}, {});
    success(created, ("create " + path).c_str());
    auto *file = static_cast<smb2fh *>(created.data);
    std::array<uint8_t, 65536> bytes{};
    for (std::size_t offset = 0; offset < size;) {
      const auto amount = std::min(bytes.size(), size - offset);
      for (std::size_t i = 0; i < amount; ++i) bytes[i] = (offset + i) % 251;
      auto written = co_await execute_async(session, [&](auto cb, auto data) {
        return smb2_pwrite_async(session.context, file, bytes.data(), amount, offset, cb, data);
      }, {}, {});
      require(written.status > 0, "Writing made no progress");
      offset += written.status;
    }
    std::size_t offset = 0;
    for (;;) {
      auto read = co_await execute_async(session, [&](auto cb, auto data) {
        return smb2_pread_async(session.context, file, bytes.data(), bytes.size(), offset, cb, data);
      }, {}, {});
      require(read.status >= 0, "Read failed");
      if (read.status == 0) break;
      for (int i = 0; i < read.status; ++i)
        require(bytes[i] == (offset + i) % 251, "File bytes differ");
      offset += read.status;
    }
    require(offset == size, "File length differs");
    success(co_await execute_async(session, [&](auto cb, auto data) { return smb2_close_async(session.context, file, cb, data); }, {}, {}));
    success(co_await execute_async(session, [&](auto cb, auto data) { return smb2_unlink_async(session.context, path.c_str(), cb, data); }, {}, {}));
  }
  for (const auto &path : {source, destination}) {
    auto opened = co_await execute_async(session, [&](auto cb, auto data) {
      return smb2_open_async(session.context, path.c_str(), O_WRONLY | O_CREAT | O_EXCL, cb, data);
    }, {}, {});
    success(opened, ("create " + path).c_str());
    auto *file = static_cast<smb2fh *>(opened.data);
    const std::string content = path == source ? "new complete content" : "previous content";
    auto written = co_await execute_async(session, [&](auto cb, auto data) {
      return smb2_pwrite_async(session.context, file, reinterpret_cast<const uint8_t *>(content.data()), content.size(), 0, cb, data);
    }, {}, {});
    require(written.status == static_cast<int>(content.size()), "Short write");
    success(co_await execute_async(session, [&](auto cb, auto data) { return smb2_close_async(session.context, file, cb, data); }, {}, {}));
  }
  auto collision = co_await execute_async(session, [&](auto cb, auto data) {
    return smb2_open_async(session.context, source.c_str(), O_WRONLY | O_CREAT | O_EXCL, cb, data);
  }, {}, {});
  require(collision.status == -EEXIST, "Exclusive create must reject existing files");
  auto refused = co_await execute_async(session, [&](auto cb, auto data) {
    return smb2_rename_async(session.context, source.c_str(), destination.c_str(), cb, data);
  }, {}, {});
  require(refused.status != 0, "Ordinary rename must not replace");
  smb2_create_request request{};
  request.impersonation_level = SMB2_IMPERSONATION_IMPERSONATION;
  request.desired_access = SMB2_DELETE | SMB2_FILE_READ_ATTRIBUTES;
  request.share_access = SMB2_FILE_SHARE_READ | SMB2_FILE_SHARE_WRITE | SMB2_FILE_SHARE_DELETE;
  request.create_disposition = SMB2_FILE_OPEN;
  request.create_options = SMB2_FILE_OPEN_REPARSE_POINT;
  request.name = source.c_str();
  smb2_create_reply handle{};
  success(co_await execute_async(session, [&](auto cb, auto data) {
    auto *pdu = smb2_cmd_create_async(session.context, &request, cb, data);
    if (!pdu) return -ENOMEM;
    smb2_queue_pdu(session.context, pdu);
    return 0;
  }, [&](void *data) { handle = *static_cast<smb2_create_reply *>(data); }, {}));
  require((handle.file_attributes & SMB2_FILE_ATTRIBUTE_REPARSE_POINT) == 0, "Probe file is not a reparse point");
  smb2_file_rename_info rename{};
  rename.replace_if_exist = 1;
  rename.file_name = reinterpret_cast<const uint8_t *>(destination.c_str());
  smb2_set_info_request change{};
  change.info_type = SMB2_0_INFO_FILE;
  change.file_info_class = SMB2_FILE_RENAME_INFORMATION;
  std::memcpy(change.file_id, handle.file_id, SMB2_FD_SIZE);
  change.input_data = &rename;
  success(co_await execute_async(session, [&](auto cb, auto data) {
    auto *pdu = smb2_cmd_set_info_async(session.context, &change, cb, data);
    if (!pdu) return -ENOMEM;
    smb2_queue_pdu(session.context, pdu);
    return 0;
  }, {}, {}), "replace");
  smb2_close_request close{};
  std::memcpy(close.file_id, handle.file_id, SMB2_FD_SIZE);
  success(co_await execute_async(session, [&](auto cb, auto data) {
    auto *pdu = smb2_cmd_close_async(session.context, &close, cb, data);
    if (!pdu) return -ENOMEM;
    smb2_queue_pdu(session.context, pdu);
    return 0;
  }, {}, {}));
  // The server must enforce a handle with no sharing, including in one session.
  request.name = destination.c_str();
  request.share_access = 0;
  request.desired_access = SMB2_FILE_READ_DATA | SMB2_FILE_WRITE_ATTRIBUTES;
  success(co_await execute_async(session, [&](auto cb, auto data) {
    auto *pdu = smb2_cmd_create_async(session.context, &request, cb, data);
    if (!pdu) return -ENOMEM;
    smb2_queue_pdu(session.context, pdu);
    return 0;
  }, [&](void *data) { handle = *static_cast<smb2_create_reply *>(data); }, {}));
  auto locked = co_await execute_async(session, [&](auto cb, auto data) {
    return smb2_open_async(session.context, destination.c_str(), O_WRONLY, cb, data);
  }, {}, {});
  require(locked.status != 0, "Exclusive share mode did not protect the file");
  smb2_file_basic_info basic{};
  basic.last_write_time.tv_sec = 1700000000;
  basic.file_attributes = SMB2_FILE_ATTRIBUTE_READONLY;
  change.file_info_class = SMB2_FILE_BASIC_INFORMATION;
  std::memcpy(change.file_id, handle.file_id, SMB2_FD_SIZE);
  change.input_data = &basic;
  success(co_await execute_async(session, [&](auto cb, auto data) {
    auto *pdu = smb2_cmd_set_info_async(session.context, &change, cb, data);
    if (!pdu) return -ENOMEM;
    smb2_queue_pdu(session.context, pdu);
    return 0;
  }, {}, {}));
  std::memcpy(close.file_id, handle.file_id, SMB2_FD_SIZE);
  success(co_await execute_async(session, [&](auto cb, auto data) {
    auto *pdu = smb2_cmd_close_async(session.context, &close, cb, data);
    if (!pdu) return -ENOMEM;
    smb2_queue_pdu(session.context, pdu);
    return 0;
  }, {}, {}));
  auto readonly = co_await execute_async(session, [&](auto cb, auto data) {
    return smb2_open_async(session.context, destination.c_str(), O_WRONLY, cb, data);
  }, {}, {});
  require(readonly.status != 0, "Read-only file accepted write access");
  request.share_access = SMB2_FILE_SHARE_READ | SMB2_FILE_SHARE_WRITE | SMB2_FILE_SHARE_DELETE;
  success(co_await execute_async(session, [&](auto cb, auto data) {
    auto *pdu = smb2_cmd_create_async(session.context, &request, cb, data);
    if (!pdu) return -ENOMEM;
    smb2_queue_pdu(session.context, pdu);
    return 0;
  }, [&](void *data) { handle = *static_cast<smb2_create_reply *>(data); }, {}));
  std::memcpy(change.file_id, handle.file_id, SMB2_FD_SIZE);
  basic.file_attributes = SMB2_FILE_ATTRIBUTE_NORMAL;
  success(co_await execute_async(session, [&](auto cb, auto data) {
    auto *pdu = smb2_cmd_set_info_async(session.context, &change, cb, data);
    if (!pdu) return -ENOMEM;
    smb2_queue_pdu(session.context, pdu);
    return 0;
  }, {}, {}));
  std::memcpy(close.file_id, handle.file_id, SMB2_FD_SIZE);
  success(co_await execute_async(session, [&](auto cb, auto data) {
    auto *pdu = smb2_cmd_close_async(session.context, &close, cb, data);
    if (!pdu) return -ENOMEM;
    smb2_queue_pdu(session.context, pdu);
    return 0;
  }, {}, {}));
  smb2_stat_64 attributes{};
  success(co_await execute_async(session, [&](auto cb, auto data) {
    return smb2_stat_async(session.context, destination.c_str(), &attributes, cb, data);
  }, {}, {}));
  require(attributes.smb2_mtime == 1700000000, "Modification time did not round trip");
  const std::string case_path = root + "/committed.TXT";
  auto opened = co_await execute_async(session, [&](auto cb, auto data) { return smb2_open_async(session.context, case_path.c_str(), O_RDONLY, cb, data); }, {}, {});
  success(opened);
  auto *file = static_cast<smb2fh *>(opened.data);
  std::array<uint8_t, 128> contents{};
  auto read = co_await execute_async(session, [&](auto cb, auto data) { return smb2_pread_async(session.context, file, contents.data(), contents.size(), 0, cb, data); }, {}, {});
  require(read.status == 20 && std::string(reinterpret_cast<char *>(contents.data()), 20) == "new complete content", "Replacement content mismatch");
  success(co_await execute_async(session, [&](auto cb, auto data) { return smb2_close_async(session.context, file, cb, data); }, {}, {}));
  success(co_await execute_async(session, [&](auto cb, auto data) { return smb2_rename_async(session.context, destination.c_str(), source.c_str(), cb, data); }, {}, {}));
  success(co_await execute_async(session, [&](auto cb, auto data) { return smb2_rename_async(session.context, source.c_str(), destination.c_str(), cb, data); }, {}, {}));
  success(co_await execute_async(session, [&](auto cb, auto data) { return smb2_unlink_async(session.context, destination.c_str(), cb, data); }, {}, {}));
  success(co_await execute_async(session, [&](auto cb, auto data) { return smb2_rmdir_async(session.context, root.c_str(), cb, data); }, {}, {}));
  std::cout << "PASS list/read/write/exclusive-create/no-replace/replace/lock/readonly/mtime/delete\n";
}

static cardio::promise<void> run_async(cardio::dispatcher_group_glib &group,
    int argc, char **argv, std::exception_ptr &failure) {
  try {
    if (argc != 6) throw std::invalid_argument("Usage: smb-probe host share user port mode (ELDER_SMB_PASSWORD); modes: basic,smb2,encrypt,encrypt2,dns-cancel,cancel,timeout,disconnect,cancel-read,cancel-write,disconnect-read");
    co_await probe_async(argv[1], argv[2], argv[3], argv[4], argv[5]);
  } catch (...) { failure = std::current_exception(); }
  group.shutdown();
}

int main(int argc, char **argv) {
  cardio::dispatcher_group_glib group;
  cardio::dispatcher_host_glib_auto dispatcher(group);
  std::exception_ptr failure;
  auto task = run_async(group, argc, argv, failure);
  dispatcher.park();
  if (failure) {
    try {std::rethrow_exception(failure);}
    catch (const std::exception &error) {std::cerr << error.what() << '\n';}
    return 1;
  }
  return 0;
}
