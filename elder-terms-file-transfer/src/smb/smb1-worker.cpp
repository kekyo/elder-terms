// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "smb1-protocol.h"
extern "C" {
#include <bdsm/bdsm.h>
}
#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>
#include <arpa/inet.h>
#include <algorithm>
#include <clocale>
#include <iostream>
#include <map>
#include <set>

namespace elder_terms::smb1 {

struct Failure : std::runtime_error {
  bool lost;
  Failure(std::string message, bool lost) : std::runtime_error(std::move(message)), lost(lost) {}
};

struct Handle {
  smb_fd fd;
  smb_tid tid;
  std::string path;
  bool writing;
  std::uint64_t expected;
  std::uint64_t written = 0;
};

struct Worker {
  std::unique_ptr<smb_session, decltype(&smb_session_destroy)> session{smb_session_new(), smb_session_destroy};
  std::map<std::string, smb_tid> trees;
  std::map<std::uint64_t, Handle> handles;
  std::set<std::string> uploads;
  std::uint64_t next_handle = 0;
  bool connected = false;
};

static void check(Worker &worker, int status, const char *operation) {
  if (status == 0) return;
  const auto nt = smb_session_get_nt_status(worker.session.get());
  throw Failure(std::string("SMB1 ") + operation + " failed (result " + std::to_string(status) +
      ", NT status " + std::to_string(nt) + ")", status == DSM_ERROR_NETWORK);
}

static void validate_text(const std::string &value) {
  if (value.find('\0') != std::string::npos ||
      !g_utf8_validate(value.data(), static_cast<gssize>(value.size()), nullptr))
    throw Failure("SMB1 requires valid UTF-8 names and credentials", false);
}

struct Path { smb_tid tid; std::string relative; };

static Path resolve_path(Worker &worker, const std::string &path) {
  validate_text(path);
  const auto end = path.find('/', 1);
  const auto share = path.substr(1, end == std::string::npos ? end : end - 1);
  if (share.empty() || g_ascii_strcasecmp(share.c_str(), "IPC$") == 0)
    throw Failure("SMB1 operation requires a disk share", false);
  auto found = worker.trees.find(share);
  if (found == worker.trees.end()) {
    smb_tid tid{};
    check(worker, smb_tree_connect(worker.session.get(), share.c_str(), &tid), "tree connect");
    found = worker.trees.emplace(share, tid).first;
  }
  auto relative = end == std::string::npos ? "\\" : path.substr(end);
  std::replace(relative.begin(), relative.end(), '/', '\\');
  return {found->second, std::move(relative)};
}

static void attributes(Payload &reply, smb_stat info, const std::string &name) {
  validate_text(name);
  reply.text(name);
  reply.number(smb_stat_get(info, SMB_STAT_ISDIR) != 0);
  reply.number(smb_stat_get(info, SMB_STAT_SIZE));
  reply.number(smb_stat_get(info, SMB_STAT_WTIME));
}

static bool exists(Worker &worker, const Path &path) {
  const auto info = smb_fstat(worker.session.get(), path.tid, path.relative.c_str());
  if (info) { smb_stat_destroy(info); return true; }
  const auto status = smb_session_get_nt_status(worker.session.get());
  if (status == NT_STATUS_OBJECT_NAME_NOT_FOUND || status == NT_STATUS_OBJECT_PATH_NOT_FOUND ||
      status == NT_STATUS_NO_SUCH_FILE) return false;
  throw Failure("SMB1 could not inspect the remote path (NT status " + std::to_string(status) + ")", status == 0);
}

static Payload execute(Worker &worker, Command command, Cursor &request) {
  auto *session = worker.session.get();
  Payload reply;
  if (!session) throw std::bad_alloc();
  if (command == Command::connect) {
    if (worker.connected) throw Failure("SMB1 worker is already connected", true);
    const auto ip = request.text(), hostname = request.text();
    const auto port = request.number();
    const auto domain = request.text(), user = request.text(), password = request.text();
    request.finish();
    validate_text(domain); validate_text(user); validate_text(password);
    in_addr address{};
    if (inet_pton(AF_INET, ip.c_str(), &address) != 1 || (port != 445 && port != 139))
      throw Failure("SMB1 supports direct IPv4 connections on ports 445 and 139", false);
    check(worker, smb_session_connect(session, hostname.c_str(), address.s_addr,
        port == 139 ? SMB_TRANSPORT_NBT : SMB_TRANSPORT_TCP), "connect");
    smb_session_set_creds(session, domain.c_str(), user.c_str(), password.c_str());
    check(worker, smb_session_login(session), "login");
    if (smb_session_is_guest(session) != 0)
      throw Failure("SMB1 server authenticated as a guest instead of the requested user", false);
    worker.connected = true;
    return reply;
  }
  if (!worker.connected) throw Failure("SMB1 authentication is required", true);
  if (command == Command::shares) {
    request.finish();
    smb_share_list list = nullptr;
    std::size_t count = 0;
    check(worker, smb_share_get_list(session, &list, &count), "share enumeration");
    const std::unique_ptr<char *, decltype(&smb_share_list_destroy)> owner(list, smb_share_list_destroy);
    std::vector<std::string> names;
    for (std::size_t index = 0; index < count; ++index) {
      const auto *name = smb_share_list_at(list, index);
      if (!name) throw Failure("SMB1 share name conversion failed; listing is incomplete", false);
      validate_text(name);
      if (g_ascii_strcasecmp(name, "IPC$") != 0) names.emplace_back(name);
    }
    reply.number(names.size());
    for (const auto &name : names) reply.text(name);
    return reply;
  }
  if (command == Command::read || command == Command::write || command == Command::close) {
    const auto id = request.number();
    auto found = worker.handles.find(id);
    if (found == worker.handles.end()) throw Failure("SMB1 file handle is closed", false);
    auto &handle = found->second;
    if (command == Command::read) {
      request.finish();
      std::array<std::byte, chunk_size> buffer{};
      const auto count = smb_fread(session, handle.fd, buffer.data(), buffer.size());
      if (count < 0 || static_cast<std::size_t>(count) > buffer.size())
        throw Failure("SMB1 file read failed", true);
      reply.data(std::span(buffer).first(static_cast<std::size_t>(count)));
    } else if (command == Command::write) {
      const auto data = request.data();
      request.finish();
      if (!handle.writing || data.size() > chunk_size || data.size() > handle.expected - handle.written)
        throw Failure("SMB1 upload size exceeds the declared size", false);
      std::vector<std::byte> buffer(data.begin(), data.end());
      for (std::size_t offset = 0; offset < buffer.size();) {
        const auto size = smb_fwrite(session, handle.fd, buffer.data() + offset, buffer.size() - offset);
        if (size <= 0 || static_cast<std::size_t>(size) > buffer.size() - offset)
          throw Failure("SMB1 file write failed or made no progress", true);
        offset += static_cast<std::size_t>(size);
        handle.written += static_cast<std::uint64_t>(size);
      }
    } else {
      request.finish();
      smb_fclose(session, handle.fd);
      const auto closed = handle;
      worker.handles.erase(found);
      if (closed.writing) {
        if (closed.written != closed.expected) throw Failure("SMB1 upload ended before its declared size", false);
        const auto info = smb_fstat(session, closed.tid, closed.path.c_str());
        if (!info) throw Failure("SMB1 could not confirm the completed upload", true);
        const auto size = smb_stat_get(info, SMB_STAT_SIZE);
        smb_stat_destroy(info);
        if (size != closed.expected) throw Failure("SMB1 completed upload has an unexpected size", false);
      }
    }
    return reply;
  }
  const auto path = request.text();
  const auto resolved = resolve_path(worker, path);
  if (command == Command::list) {
    request.finish();
    auto pattern = resolved.relative;
    if (pattern.back() != '\\') pattern += '\\';
    pattern += '*';
    const auto list = smb_find(session, resolved.tid, pattern.c_str());
    if (!list) throw Failure("SMB1 directory listing failed; its contents may be incomplete", false);
    const std::unique_ptr<smb_file, decltype(&smb_stat_list_destroy)> owner(list, smb_stat_list_destroy);
    std::vector<smb_stat> entries;
    for (auto item = list; item != nullptr; item = smb_stat_list_next(item)) {
      const auto *name = smb_stat_name(item);
      if (!name) throw Failure("SMB1 filename conversion failed; listing is incomplete", false);
      if (std::string_view(name) != "." && std::string_view(name) != "..") entries.push_back(item);
    }
    reply.number(entries.size());
    for (auto item : entries) attributes(reply, item, smb_stat_name(item));
  } else if (command == Command::stat) {
    request.finish();
    const auto info = smb_fstat(session, resolved.tid, resolved.relative.c_str());
    if (!info) {
      if (exists(worker, resolved)) throw Failure("SMB1 path changed during inspection", false);
      reply.number(0);
    } else {
      const std::unique_ptr<smb_file, decltype(&smb_stat_destroy)> owner(info, smb_stat_destroy);
      reply.number(1);
      attributes(reply, info, path.substr(path.find_last_of('/') + 1));
    }
  } else if (command == Command::mkdir) {
    request.finish();
    check(worker, smb_directory_create(session, resolved.tid, resolved.relative.c_str()), "mkdir");
  } else if (command == Command::rmdir) {
    request.finish();
    check(worker, smb_directory_rm(session, resolved.tid, resolved.relative.c_str()), "rmdir");
  } else if (command == Command::remove) {
    request.finish();
    if (worker.uploads.contains(path)) {
      // Preserve an old destination retained during an interrupted replacement.
      if (exists(worker, {resolved.tid, resolved.relative + "\\previous"}))
        throw Failure("SMB1 previous destination is retained inside " + path, false);
      const auto content = resolved.relative + "\\content";
      if (exists(worker, {resolved.tid, content})) check(worker, smb_file_rm(session, resolved.tid, content.c_str()), "remove upload");
      check(worker, smb_directory_rm(session, resolved.tid, resolved.relative.c_str()), "remove upload directory");
      worker.uploads.erase(path);
    } else check(worker, smb_file_rm(session, resolved.tid, resolved.relative.c_str()), "remove");
  } else if (command == Command::rename || command == Command::commit) {
    const auto destination = request.text();
    const auto target = resolve_path(worker, destination);
    const bool overwrite = command == Command::commit && request.number() != 0;
    request.finish();
    if (resolved.tid != target.tid) throw Failure("SMB1 rename cannot cross shares", false);
    if (command == Command::rename) {
      check(worker, smb_file_mv(session, resolved.tid, resolved.relative.c_str(), target.relative.c_str()), "rename");
    } else {
      if (!worker.uploads.contains(path)) throw Failure("SMB1 temporary upload ownership is unknown", false);
      const auto content = resolved.relative + "\\content", previous = resolved.relative + "\\previous";
      bool backed_up = false;
      if (overwrite && exists(worker, target)) {
        check(worker, smb_file_mv(session, target.tid, target.relative.c_str(), previous.c_str()), "retain previous destination");
        backed_up = true;
      }
      const auto status = smb_file_mv(session, resolved.tid, content.c_str(), target.relative.c_str());
      if (status != 0) {
        if (backed_up && smb_file_mv(session, target.tid, previous.c_str(), target.relative.c_str()) != 0)
          throw Failure("SMB1 replacement failed; the previous destination is retained inside " + path, true);
        check(worker, status, "commit upload");
      }
      if (backed_up) check(worker, smb_file_rm(session, resolved.tid, previous.c_str()), "remove previous destination");
      check(worker, smb_directory_rm(session, resolved.tid, resolved.relative.c_str()), "remove completed upload directory");
      worker.uploads.erase(path);
    }
  } else if (command == Command::read_open || command == Command::write_open) {
    const bool writing = command == Command::write_open;
    const auto expected = writing ? request.number() : 0;
    request.finish();
    auto file_path = resolved.relative;
    if (writing) {
      // libdsm cannot request exclusive file creation. An exclusively created
      // directory owns the staged file and any retained replacement instead.
      check(worker, smb_directory_create(session, resolved.tid, file_path.c_str()), "reserve upload directory");
      worker.uploads.insert(path);
      file_path += "\\content";
    }
    smb_fd fd{};
    check(worker, smb_fopen(session, resolved.tid, file_path.c_str(), writing ? SMB_MOD_RW : SMB_MOD_RO, &fd), "open file");
    const auto id = ++worker.next_handle;
    worker.handles.emplace(id, Handle{fd, resolved.tid, file_path, writing, expected});
    reply.number(id);
  } else throw Failure("Unknown SMB1 helper operation", true);
  return reply;
}

static cardio::promise<void> serve() {
  Worker worker;
  const std::unique_ptr<GInputStream, decltype(&g_object_unref)> input(g_unix_input_stream_new(0, false), g_object_unref);
  const std::unique_ptr<GOutputStream, decltype(&g_object_unref)> output(g_unix_output_stream_new(1, false), g_object_unref);
  for (;;) {
    const auto frame = co_await receive_frame(input.get(), {});
    Frame response{frame.id, frame.command, {}};
    bool lost = false;
    try {
      Cursor request{frame.payload.bytes};
      response.payload = execute(worker, frame.command, request);
    } catch (const std::exception &error) {
      const auto *failure = dynamic_cast<const Failure *>(&error);
      lost = !failure || failure->lost;
      response.command = Command::error;
      response.payload.number(lost);
      response.payload.text(error.what());
    }
    co_await send_frame(output.get(), response, {});
    if (lost) co_return;
  }
}

} // namespace elder_terms::smb1

int main() {
  // libdsm may call setlocale itself. Set the environment in this isolated
  // process so that every library call uses the same UTF-8 API boundary.
  if (setenv("LC_ALL", "C.UTF-8", 1) != 0 || !std::setlocale(LC_ALL, "")) return 1;
  cardio::dispatcher_group_glib group;
  cardio::dispatcher_host_glib_auto dispatcher(group);
  int status = 0;
  const auto run = [&]() -> cardio::promise<void> {
    try { co_await elder_terms::smb1::serve(); }
    catch (const std::exception &error) { std::cerr << error.what() << '\n'; status = 1; }
    group.shutdown();
  };
  auto pending = run();
  dispatcher.park();
  return status;
}
