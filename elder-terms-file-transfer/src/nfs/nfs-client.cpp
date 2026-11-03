// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "nfs-client.h"
#include "nfs-rpc.h"
#include "nfs-api.h"
#include "nfs-name.h"
#include "nfs-v2.h"
#include <nfsc/libnfs-raw-mount.h>
#include <nfsc/libnfs-raw-nfs4.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <climits>
#include <cstring>
#include <map>
#include <utility>

namespace elder_terms {

static std::atomic<std::uint64_t> next_generation{1};

// Operation names borrow the owned path until the asynchronous compound completes.
static std::vector<nfs_argop4> v4_path_operations(std::string &path, nfs_opnum4 final) {
  std::vector<nfs_argop4> operations(1);
  operations.front().argop = OP_PUTROOTFH;
  for (std::size_t start = 1; start < path.size();) {
    const auto end = path.find('/', start);
    auto &op = operations.emplace_back();
    op.argop = OP_LOOKUP;
    op.nfs_argop4_u.oplookup.objname = {
        static_cast<u_int>((end == std::string::npos ? path.size() : end) - start), path.data() + start};
    if (end == std::string::npos) break;
    start = end + 1;
  }
  operations.emplace_back().argop = final;
  return operations;
}

struct NfsTransferLimits { std::uint64_t read; std::uint64_t write; };

static cardio::promise<NfsTransferLimits> load_v4_transfer_limits(const std::shared_ptr<NfsRpcSession> &session,
    std::string path, cardio::cancellation cancellation) {
  // libnfs 5.0.2 does not populate readmax/writemax when mounting v4. Query
  // the public protocol attributes through the same authenticated RPC context.
  auto operations = v4_path_operations(path, OP_GETATTR);
  std::uint32_t mask = (1U << FATTR4_MAXREAD) | (1U << FATTR4_MAXWRITE);
  operations.back().argop = OP_GETATTR;
  operations.back().nfs_argop4_u.opgetattr.attr_request = {1, &mask};
  COMPOUND4args args{};
  args.minorversion = 0;
  args.argarray = {static_cast<u_int>(operations.size()), operations.data()};
  NfsTransferLimits limits{};
  nfsstat4 status = NFS4_OK;
  co_await execute_nfs_raw_async(session, [&](auto cb, auto data) {
    return ELDER_NFS_RAW_CALL(rpc_nfs4_compound, session->rpc, cb, &args, data);
  }, [&](void *data) {
    const auto &result = *static_cast<COMPOUND4res *>(data);
    status = result.status;
    if (status != NFS4_OK) return;
    if (result.resarray.resarray_len != operations.size())
      throw RemoteFileError("Invalid NFS v4 transfer limit response", true);
    // libnfs 5.0.2's decoded array may be only four-byte aligned. Copy its
    // public value into aligned storage before accessing the 64-bit union.
    nfs_resop4 last{};
    std::memcpy(&last, reinterpret_cast<const char *>(result.resarray.resarray_val) +
        (result.resarray.resarray_len - 1) * sizeof(last), sizeof(last));
    if (last.resop != OP_GETATTR || last.nfs_resop4_u.opgetattr.status != NFS4_OK)
      throw RemoteFileError("Invalid NFS v4 transfer limit response", true);
    const auto &attributes = last.nfs_resop4_u.opgetattr.GETATTR4res_u.resok4.obj_attributes;
    const auto words = attributes.attrmask.bitmap4_len;
    const auto returned = words ? attributes.attrmask.bitmap4_val[0] : 0U;
    if ((returned & ~mask) || (words > 1 && std::any_of(attributes.attrmask.bitmap4_val + 1,
        attributes.attrmask.bitmap4_val + words, [](std::uint32_t word) { return word != 0; })))
      throw RemoteFileError("NFS v4 returned unexpected transfer limit attributes", true);
    const auto count = static_cast<unsigned>((returned >> FATTR4_MAXREAD) & 1U) +
        static_cast<unsigned>((returned >> FATTR4_MAXWRITE) & 1U);
    if (attributes.attr_vals.attrlist4_len != count * 8)
      throw RemoteFileError("NFS v4 returned malformed transfer limit attributes", true);
    const auto number = [&](std::size_t offset) {
      std::uint64_t value = 0;
      for (std::size_t index = 0; index < 8; ++index)
        value = (value << 8) | static_cast<unsigned char>(attributes.attr_vals.attrlist4_val[offset + index]);
      return value;
    };
    // RFC 7530 marks these attributes RECOMMENDED, not mandatory. Missing
    // limits must not prevent browsing; use modest requests until advertised.
    std::size_t offset = 0;
    const auto limit = [&](std::uint32_t bit) {
      if (!(returned & (1U << bit))) return UINT64_C(4096);
      const auto value = number(offset);
      offset += 8;
      return value;
    };
    const auto read_limit = limit(FATTR4_MAXREAD), write_limit = limit(FATTR4_MAXWRITE);
    if (!read_limit || !write_limit) throw RemoteFileError("NFS v4 server reported a zero transfer limit", false);
    // Public read/write completion counts are signed ints. The transfer engine
    // may request less, but no request can exceed either this bound or the server.
    limits = {std::min<std::uint64_t>(read_limit, INT_MAX), std::min<std::uint64_t>(write_limit, INT_MAX)};
  }, cancellation);
  if (status != NFS4_OK) throw RemoteFileError("NFS v4 transfer limit query failed: " + std::to_string(status), status == NFS4ERR_STALE);
  co_return limits;
}

static cardio::promise<std::string> read_v4_link(const std::shared_ptr<NfsRpcSession> &session,
    std::string path, cardio::cancellation cancellation) {
  auto operations = v4_path_operations(path, OP_READLINK);
  COMPOUND4args args{};
  args.minorversion = 0;
  args.argarray = {static_cast<u_int>(operations.size()), operations.data()};
  std::string value;
  nfsstat4 status = NFS4_OK;
  co_await execute_nfs_raw_async(session, [&](auto cb, auto data) {
    return ELDER_NFS_RAW_CALL(rpc_nfs4_compound, session->rpc, cb, &args, data);
  }, [&](void *data) {
    const auto &result = *static_cast<COMPOUND4res *>(data);
    status = result.status;
    if (status != NFS4_OK) return;
    if (result.resarray.resarray_len != operations.size())
      throw RemoteFileError("Invalid NFS v4 read link response", true);
    nfs_resop4 last{};
    std::memcpy(&last, reinterpret_cast<const char *>(result.resarray.resarray_val) +
        (result.resarray.resarray_len - 1) * sizeof(last), sizeof(last));
    if (last.resop != OP_READLINK || last.nfs_resop4_u.opreadlink.status != NFS4_OK)
      throw RemoteFileError("Invalid NFS v4 read link response", true);
    // In 5.0.2 the high-level v4 readlink uses strdup on an XDR byte array.
    // Its length is authoritative; the wire value need not be NUL-terminated.
    const auto &link = last.nfs_resop4_u.opreadlink.READLINK4res_u.resok4.link;
    value.assign(link.utf8string_val, link.utf8string_len);
  }, cancellation);
  if (status != NFS4_OK) throw RemoteFileError("NFS v4 read link failed: " + std::to_string(status), status == NFS4ERR_STALE);
  co_return value;
}

struct NfsResolveCancellation {
  cardio::cancellation_source source;
  ~NfsResolveCancellation() { (void)source.cancel(); }
};

static cardio::promise<std::vector<std::string>> resolve_addresses_async(
    const std::string &host, int timeout_ms, cardio::cancellation cancellation) {
  auto resolver = std::shared_ptr<GResolver>(g_resolver_get_default(), g_object_unref);
  NfsResolveCancellation deadline{cardio::cancellations::timeout(timeout_ms)};
  NfsResolveCancellation stopped{cardio::cancellations::any(cancellation, deadline.source.get_cancellation())};
  auto lookup = cardio::gio::submit<GList *>(
      [&](GCancellable *stop, GAsyncReadyCallback callback, gpointer data) {
        g_resolver_lookup_by_name_async(resolver.get(), host.c_str(), stop, callback, data);
      }, [](GObject *source, GAsyncResult *result, GError **error) {
        return g_resolver_lookup_by_name_finish(G_RESOLVER(source), result, error);
      }, stopped.source.get_cancellation());
  std::vector<std::string> result;
  try {
    auto addresses = std::unique_ptr<GList, decltype(&g_resolver_free_addresses)>(co_await lookup, g_resolver_free_addresses);
    for (auto *node = addresses.get(); node; node = node->next) {
      auto *text = g_inet_address_to_string(G_INET_ADDRESS(node->data));
      result.emplace_back(text);
      g_free(text);
    }
  } catch (...) {
    cancellation.throw_if_cancellation_requested();
    if (deadline.source.get_cancellation().is_cancellation_requested())
      throw RemoteFileError("NFS address lookup timed out", true);
    throw;
  }
  if (result.empty()) throw RemoteFileError("NFS hostname has no usable addresses", false);
  co_return result;
}

static void require_session(const std::shared_ptr<NfsRpcSession> &session) {
  if (!session->rpc || session->stopping.get_cancellation().is_cancellation_requested())
    throw RemoteFileError("NFS connection has ended. Reconnect to continue.", true);
}

static cardio::promise<void> perform(const std::shared_ptr<NfsRpcSession> &session,
    const char *name, std::function<int(nfs_cb, void *)> start, cardio::cancellation cancellation) {
  check_nfs_result(co_await execute_nfs_async(session, std::move(start), {}, cancellation), name);
}

static cardio::promise<std::optional<nfs_stat_64>> stat_native(
    const std::shared_ptr<NfsRpcSession> &session, const std::string &path, cardio::cancellation cancellation) {
  nfs_stat_64 attributes{};
  const auto result = co_await execute_nfs_async(session, [&](auto cb, auto data) {
    return nfs_lstat64_async(session->nfs, path.c_str(), cb, data);
  }, [&](int, void *data) { attributes = *static_cast<nfs_stat_64 *>(data); }, cancellation);
  if (result.status == -ENOENT) co_return std::nullopt;
  check_nfs_result(result, "attributes");
  co_return attributes;
}

static RemoteFileType file_type(std::uint64_t mode) {
  if (S_ISREG(mode)) return RemoteFileType::regular;
  if (S_ISDIR(mode)) return RemoteFileType::directory;
  if (S_ISLNK(mode)) return RemoteFileType::symbolic_link;
  return RemoteFileType::other;
}

static RemoteFileAttributes attributes_for(const std::string &path, const std::string &raw_name,
    const std::string &encoding, const nfs_stat_64 &attributes) {
  return {.name = display_nfs_name(raw_name, encoding).text, .path = path,
      .type = file_type(attributes.nfs_mode), .size = attributes.nfs_size,
      .permissions = static_cast<std::uint32_t>(attributes.nfs_mode),
      .access_time_unix_seconds = static_cast<std::int64_t>(attributes.nfs_atime),
      .modification_time_unix_seconds = static_cast<std::int64_t>(attributes.nfs_mtime)};
}

static cardio::promise<void> close_handle(const std::shared_ptr<NfsRpcSession> &session,
    nfsfh *&handle, cardio::cancellation cancellation) {
  if (!handle) co_return;
  if (!session->nfs) { handle = nullptr; co_return; }
  auto *closing = handle;
  co_await perform(session, "close", [&](auto cb, auto data) {
    const int status = nfs_close_async(session->nfs, closing, cb, data);
    if (status >= 0) {
      session->handles.erase(closing);
      handle = nullptr;
    }
    return status;
  }, cancellation);
}

struct NfsReader final : RemoteFileReader {
  std::shared_ptr<NfsRpcSession> session;
  nfsfh *handle = nullptr;
  std::uint64_t chunk_limit = 0;
  std::uint64_t offset = 0;
  ~NfsReader() override { if (handle) session->stop(); }
  cardio::promise<std::size_t> read_async(std::span<std::byte> buffer,
      cardio::cancellation cancellation) override {
    auto lock = std::move(co_await session->operations.lock(cancellation));
    require_session(session);
    if (!handle) throw RemoteFileError("NFS reader is closed", false);
    if (buffer.empty()) co_return 0;
    const auto amount = std::min<std::uint64_t>(buffer.size(), chunk_limit);
    if (!amount || offset > UINT64_MAX - amount) throw RemoteFileError("Invalid NFS read range", false);
    const auto count = co_await read_nfs_async(session, handle, buffer.first(amount), offset, cancellation);
    offset += count;
    co_return count;
  }
  cardio::promise<void> close_async(cardio::cancellation cancellation) override {
    auto lock = std::move(co_await session->operations.lock(cancellation));
    co_await close_handle(session, handle, cancellation);
  }
};

struct NfsWriter final : RemoteFileWriter {
  std::shared_ptr<NfsRpcSession> session;
  nfsfh *handle = nullptr;
  std::uint64_t chunk_limit = 0;
  std::string path;
  std::uint32_t permissions = 0600;
  std::uint64_t offset = 0;
  std::uint64_t expected_size = 0;
  RemoteFileCreationState creation = RemoteFileCreationState::not_created;
  bool attempted = false;
  bool closed = false;
  ~NfsWriter() override { if (handle) session->stop(); }
  RemoteFileCreationState creation_state() const noexcept override { return creation; }
  cardio::promise<void> ensure_open(cardio::cancellation cancellation) {
    require_session(session);
    if (handle) co_return;
    if (attempted || closed) throw RemoteFileError("NFS upload creation cannot be replayed", false);
    cancellation.throw_if_cancellation_requested();
    attempted = true;
    const auto result = co_await execute_nfs_async(session, [&](auto cb, auto data) {
      creation = RemoteFileCreationState::unknown;
      const int status = nfs_create_async(session->nfs, path.c_str(), O_EXCL | O_WRONLY | O_NOFOLLOW,
          permissions & 0777, cb, data);
      if (status < 0) creation = RemoteFileCreationState::not_created;
      return status;
    }, [&](int, void *data) {
      handle = static_cast<nfsfh *>(data);
      session->handles.insert(handle);
      creation = RemoteFileCreationState::created;
    }, cancellation);
    if (result.status == -EEXIST) creation = RemoteFileCreationState::not_created;
    check_nfs_result(result, "create upload");
  }
  cardio::promise<void> write_all_async(std::span<const std::byte> buffer,
      cardio::cancellation cancellation) override {
    auto lock = std::move(co_await session->operations.lock(cancellation));
    if (closed) throw RemoteFileError("NFS writer is closed", false);
    if (offset > expected_size || buffer.size() > expected_size - offset)
      throw RemoteFileError("NFS upload exceeded its declared size", false);
    if (buffer.empty()) co_return;
    co_await ensure_open(cancellation);
    while (!buffer.empty()) {
      const auto amount = std::min<std::uint64_t>(buffer.size(), chunk_limit);
      if (!amount) throw RemoteFileError("Invalid NFS write limit", true);
      const auto result = co_await execute_nfs_async(session, [&](auto cb, auto data) {
#ifdef LIBNFS_API_V2
        return nfs_pwrite_async(session->nfs, handle, buffer.data(), amount, offset, cb, data);
#else
        return nfs_pwrite_async(session->nfs, handle, offset, amount, buffer.data(), cb, data);
#endif
      }, {}, cancellation);
      check_nfs_result(result, "write");
      if (result.status == 0 || static_cast<std::uint64_t>(result.status) > amount)
        throw RemoteFileError("NFS write made no valid progress", true);
      offset += result.status;
      buffer = buffer.subspan(result.status);
    }
  }
  cardio::promise<void> close_async(cardio::cancellation cancellation) override {
    auto lock = std::move(co_await session->operations.lock(cancellation));
    if (closed) co_return;
    // Empty files have no write call. A failed CREATE is never retried during
    // cleanup, and an abandoned nonempty writer must not create a new file.
    if (!attempted && expected_size == 0) co_await ensure_open(cancellation);
    closed = true;
    if (!handle) {
      if (!attempted && expected_size != 0)
        throw RemoteFileError("NFS upload ended before its declared size", false);
      co_return;
    }
    const bool complete = offset == expected_size;
    co_await close_handle(session, handle, cancellation);
    if (!complete) throw RemoteFileError("NFS upload ended before its declared size", false);
  }
};

struct NfsExport {
  std::string bytes;
  std::shared_ptr<NfsRpcSession> session;
  std::optional<NfsTransferLimits> limits;
  std::shared_ptr<Nfs2Filesystem> v2;
};

struct NfsLocation {
  std::string root;
  std::string bytes;
  std::string name;
  std::string native;
};

struct NfsBrowserDirectory {
  std::string bytes;
  std::string parent;
  bool virtual_directory;
  std::vector<RemoteFileAttributes> children;
};

struct NfsServer final : RemoteFileClient {
  NfsClientOpenOptions options;
  std::vector<std::string> addresses;
  std::string generation;
  std::map<std::string, NfsExport> exports;
  std::map<std::string, std::string> browser_paths;
  std::map<std::string, NfsBrowserDirectory> browser_directories;
  std::shared_ptr<NfsRpcSession> namespace_session;
  cardio::primitives::mutex operations;
  bool stopped = false;
  bool transfer_active = false;
  std::string discovery_error;

  ~NfsServer() override { stop(); }
  void stop() {
    stopped = true;
    if (namespace_session) namespace_session->stop();
    for (auto &[id, item] : exports) { (void)id; if (item.session) item.session->stop(); }
  }
  void require_open() const {
    if (stopped) throw RemoteFileError("NFS connection has ended. Reconnect to continue.", true);
  }
  void add_export(const std::string &bytes) {
    if (bytes.empty() || bytes.front() != '/' || bytes.find('\0') != std::string::npos)
      throw RemoteFileError("NFS server returned an invalid export path", false);
    exports.try_emplace("/" + generation + nfs_identifier_component(bytes), NfsExport{bytes, {}, {}, {}});
  }
  RemoteFileAttributes browser_attributes(const std::string &path) const {
    const auto &directory = browser_directories.at(path);
    RemoteFileAttributes result;
    result.path = path;
    result.name = display_nfs_name(directory.parent == "/" ? directory.bytes :
        directory.bytes.substr(directory.bytes.find_last_of('/') + 1), options.filename_encoding).text;
    result.type = RemoteFileType::directory;
    result.role = RemoteFileRole::nfs_export;
    return result;
  }
  void build_browser_tree() {
    for (const auto &[id, item] : exports) {
      browser_paths[item.bytes] = id;
      for (auto end = item.bytes.find('/', 1); end != std::string::npos; end = item.bytes.find('/', end + 1))
        browser_paths.try_emplace(item.bytes.substr(0, end), "");
    }
    for (auto &[bytes, id] : browser_paths) {
      if (!id.empty()) continue;
      std::string ancestor;
      std::size_t ancestor_length = 0;
      for (const auto &[export_id, item] : exports) {
        if ((item.bytes == "/" || bytes.starts_with(item.bytes + "/")) && item.bytes.size() > ancestor_length) {
          ancestor = export_id;
          ancestor_length = item.bytes.size();
        }
      }
      if (ancestor.empty()) {
        id = "/" + generation + "group-" + nfs_identifier_component(bytes);
      } else {
        // An ancestor that is already exported remains a real directory. Its
        // files must not disappear behind a synthetic list of nested exports.
        id = ancestor;
        for (auto start = ancestor_length == 1 ? 1 : ancestor_length + 1; start < bytes.size();) {
          const auto end = bytes.find('/', start);
          id += "/" + nfs_identifier_component(bytes.substr(start, end == std::string::npos ? end : end - start));
          if (end == std::string::npos) break;
          start = end + 1;
        }
      }
    }
    browser_directories.emplace("/", NfsBrowserDirectory{"/", "/", true, {}});
    for (const auto &[bytes, id] : browser_paths) {
      const auto separator = bytes.find_last_of('/');
      const auto parent = separator == 0 ? "/" : browser_paths.at(bytes.substr(0, separator));
      browser_directories.emplace(id, NfsBrowserDirectory{bytes, parent,
          !exports.contains(id.substr(0, id.find('/', 1))), {}});
    }
    for (const auto &[bytes, id] : browser_paths) {
      (void)bytes;
      browser_directories.at(browser_directories.at(id).parent).children.push_back(browser_attributes(id));
    }
  }
  bool is_virtual_directory(const std::string &path) const {
    const auto found = browser_directories.find(path);
    return found != browser_directories.end() && found->second.virtual_directory;
  }
  NfsLocation locate(const std::string &path) const {
    const auto separator = path.find('/', 1);
    const auto root = path.substr(0, separator);
    const auto found = exports.find(root);
    if (found == exports.end()) throw RemoteFileError("Unknown or expired NFS path identity", false);
    NfsLocation result{root, "", found->second.bytes, ""};
    for (auto start = separator; start != std::string::npos;) {
      const auto next = path.find('/', start + 1);
      const auto component = path.substr(start + 1, next == std::string::npos ? next : next - start - 1);
      const auto raw = nfs_identifier_bytes(component);
      if (raw.empty() || raw == "." || raw == ".." || raw.find('/') != std::string::npos || raw.find('\0') != std::string::npos)
        throw RemoteFileError("Invalid NFS path component", false);
      result.bytes += "/" + raw;
      result.name = raw;
      start = next;
    }
    if (result.bytes.empty()) result.bytes = "/";
    result.native = result.bytes;
    if (options.version == 4) {
      const auto &base = found->second.bytes;
      result.native = result.bytes == "/" ? base : (base == "/" ? "" : base) + result.bytes;
    }
    return result;
  }
  bool is_public_root(const NfsLocation &path) const {
    if (path.bytes == "/") return true;
    const auto &base = exports.at(path.root).bytes;
    const auto absolute = (base == "/" ? "" : base) + path.bytes;
    return std::any_of(exports.begin(), exports.end(), [&](const auto &entry) {
      return entry.second.bytes == absolute;
    });
  }
  void require_mutable(const NfsLocation &path) const {
    if (is_public_root(path)) throw RemoteFileError("NFS public roots cannot be modified", false);
  }
  std::string directory_child_path(const std::string &parent, const std::string &bytes) const {
    const auto location = locate(parent);
    const auto &base = exports.at(location.root).bytes;
    const auto absolute = (base == "/" ? "" : base) + (location.bytes == "/" ? "" : location.bytes) + "/" + bytes;
    const auto known = browser_paths.find(absolute);
    return known == browser_paths.end() ? parent + "/" + nfs_identifier_component(bytes) : known->second;
  }
  RemoteFileCapabilities capabilities() const noexcept override {
    return {.write_operations = true, .symbolic_links = true, .permissions = true, .access_time = true,
        .modification_time = true, .upload_commit = true};
  }
  RemotePathCapabilities path_capabilities(const std::string &path) const override {
    require_open();
    if (path == "/" || is_virtual_directory(path)) return {false, false};
    const auto target = locate(path);
    return {true, !is_public_root(target)};
  }
  std::string child_path(const std::string &parent, const std::string &name) const override {
    require_open();
    return directory_child_path(parent, encode_nfs_name(name, options.filename_encoding, 255));
  }
  std::string parent_path(const std::string &path) const override {
    if (path == "/") return path;
    const auto directory = browser_directories.find(path);
    if (directory != browser_directories.end()) return directory->second.parent;
    (void)locate(path);
    return RemoteFileClient::parent_path(path);
  }
  std::string display_path(const std::string &path) const override {
    if (path == "/" || path.empty()) return path;
    if (is_virtual_directory(path)) return display_nfs_name(browser_directories.at(path).bytes, options.filename_encoding).text;
    const auto target = locate(path);
    auto result = display_nfs_name(exports.at(target.root).bytes, options.filename_encoding).text + ":";
    for (std::size_t start = 0; start < target.bytes.size();) {
      const auto next = target.bytes.find('/', start + 1);
      result += "/" + display_nfs_name(target.bytes.substr(start + 1,
          next == std::string::npos ? next : next - start - 1), options.filename_encoding).text;
      if (next == std::string::npos) break;
      start = next;
    }
    return result;
  }
  bool path_input_editable() const noexcept override { return false; }
  std::optional<std::string> local_filename(const RemoteFileAttributes &item) const override {
    if (item.path == "/") return std::nullopt;
    auto raw = is_virtual_directory(item.path) ? browser_directories.at(item.path).bytes : locate(item.path).name;
    if (is_virtual_directory(item.path) || item.role == RemoteFileRole::nfs_export) raw = raw.substr(raw.find_last_of('/') + 1);
    const auto name = display_nfs_name(raw, options.filename_encoding);
    return name.decoded ? std::optional(name.text) : std::nullopt;
  }
  std::string import_link_target(const std::string &target) const override {
    return encode_nfs_link_target(target, options.filename_encoding, options.version == 2 ? 1024 : 4096);
  }
  std::string export_link_target(const std::string &target) const override {
    return decode_nfs_link_target(target, options.filename_encoding);
  }
  std::string upload_temporary_path(const std::string &destination, const std::string &suffix) const override {
    require_mutable(locate(destination));
    return child_path(destination.substr(0, destination.find_last_of('/')), suffix);
  }
  bool try_begin_transfer() override {
    require_open();
    if (transfer_active) return false;
    transfer_active = true;
    return true;
  }
  void end_transfer() override { transfer_active = false; }

  cardio::promise<std::shared_ptr<NfsRpcSession>> mounted(const NfsLocation &path,
      cardio::cancellation cancellation) {
    require_open();
    auto &entry = exports.at(path.root);
    if (options.version == 4 && namespace_session) entry.session = namespace_session;
    if (!entry.session) {
      std::exception_ptr failure;
      for (const auto &address : addresses) {
        auto session = options.version == 2 ? create_nfs_raw_session(options.connect_timeout_ms) :
            create_nfs_rpc_session(options.version, options.connect_timeout_ms);
        if (session->nfs) {
          configure_nfs_ports(session, options.nfs_port, options.mount_port);
          if (options.uid) nfs_set_uid(session->nfs, std::bit_cast<std::int32_t>(*options.uid));
          if (options.gid) nfs_set_gid(session->nfs, std::bit_cast<std::int32_t>(*options.gid));
          auto groups = options.supplementary_gids;
          nfs_set_auxiliary_gids(session->nfs, groups.size(), groups.data());
        }
        entry.session = session;
        try {
          if (options.version == 2)
            entry.v2 = co_await open_nfs2_filesystem_async(session, options, address, entry.bytes, cancellation);
          else {
            co_await perform(session, "mount", [&](auto cb, auto data) {
              return nfs_mount_async(session->nfs, address.c_str(), options.version == 4 ? "/" : entry.bytes.c_str(), cb, data);
            }, cancellation);
          }
        } catch (const cardio::canceled_exception &) { entry.session.reset(); throw; }
        catch (...) { entry.session.reset(); failure = std::current_exception(); continue; }
        session->timeout_ms = options.request_timeout_ms;
        if (session->nfs) nfs_set_timeout(session->nfs, options.request_timeout_ms);
        if (options.version == 4) namespace_session = session;
        break;
      }
      if (!entry.session) std::rethrow_exception(failure);
    }
    require_session(entry.session);
    if (options.version != 2 && !entry.limits) {
      auto session_lock = std::move(co_await entry.session->operations.lock(cancellation));
      if (options.version == 4) entry.limits = co_await load_v4_transfer_limits(entry.session, entry.bytes, cancellation);
      else entry.limits = NfsTransferLimits{nfs_get_readmax(entry.session->nfs), nfs_get_writemax(entry.session->nfs)};
    }
    co_return entry.session;
  }

  cardio::promise<RemoteDirectorySnapshot> load_directory_async(std::string path,
      cardio::cancellation cancellation) override {
    auto lock = std::move(co_await operations.lock(cancellation));
    require_open();
    RemoteDirectorySnapshot snapshot{path, {}};
    if (is_virtual_directory(path)) {
      if (path == "/" && options.version == 4) snapshot.notice = "NFS v4 public namespace";
      if (path == "/" && !discovery_error.empty()) snapshot.notice =
          (options.version == 4 ? "Namespace discovery failed; showing configured paths. " :
           "Export discovery failed; showing configured paths. ") + discovery_error;
      snapshot.entries = browser_directories.at(path).children;
      co_return snapshot;
    }
    const auto target = locate(path);
    auto session = co_await mounted(target, cancellation);
    if (options.version == 2) {
      auto entries = co_await list_nfs2_directory_async(exports.at(target.root).v2, target.native, cancellation);
      for (auto &entry : entries) {
        auto &item = entry.attributes;
        item.path = directory_child_path(path, entry.bytes);
        item.name = display_nfs_name(entry.bytes, options.filename_encoding).text;
        if (browser_directories.contains(item.path) || is_public_root(locate(item.path))) item.role = RemoteFileRole::nfs_export;
        snapshot.entries.push_back(std::move(item));
      }
      co_return snapshot;
    }
    auto session_lock = std::move(co_await session->operations.lock(cancellation));
    check_nfs_result(co_await execute_nfs_async(session, [&](auto cb, auto data) {
      return nfs_opendir_async(session->nfs, target.native.c_str(), cb, data);
    }, [&](int, void *data) {
      auto *directory = static_cast<nfsdir *>(data);
      struct DirectoryClose { nfs_context *context; nfsdir *directory; ~DirectoryClose() { nfs_closedir(context, directory); } } close{session->nfs, directory};
      while (auto *entry = nfs_readdir(session->nfs, directory)) {
        const std::string raw(entry->name);
        if (raw == "." || raw == "..") continue;
        RemoteFileAttributes item;
        item.path = directory_child_path(path, raw);
        item.name = display_nfs_name(raw, options.filename_encoding).text;
        item.type = entry->type == 1 ? RemoteFileType::regular : entry->type == 2 ? RemoteFileType::directory :
            entry->type == 5 ? RemoteFileType::symbolic_link : RemoteFileType::other;
        item.size = entry->size;
        item.permissions = entry->mode;
        item.access_time_unix_seconds = entry->atime.tv_sec;
        item.modification_time_unix_seconds = entry->mtime.tv_sec;
        if (browser_directories.contains(item.path) || is_public_root(locate(item.path))) item.role = RemoteFileRole::nfs_export;
        snapshot.entries.push_back(std::move(item));
      }
    }, cancellation), "list directory");
    co_return snapshot;
  }

  cardio::promise<std::optional<RemoteFileAttributes>> lstat_async(std::string path,
      cardio::cancellation cancellation) override {
    auto lock = std::move(co_await operations.lock(cancellation));
    require_open();
    if (is_virtual_directory(path)) co_return browser_attributes(path);
    const auto target = locate(path);
    auto session = co_await mounted(target, cancellation);
    if (options.version == 2) {
      auto item = co_await stat_nfs2_async(exports.at(target.root).v2, target.native, cancellation);
      if (item) {
        item->path = path;
        item->name = display_nfs_name(target.name, options.filename_encoding).text;
        if (browser_directories.contains(path) || is_public_root(target)) item->role = RemoteFileRole::nfs_export;
      }
      co_return item;
    }
    auto session_lock = std::move(co_await session->operations.lock(cancellation));
    const auto value = co_await stat_native(session, target.native, cancellation);
    if (!value) co_return std::nullopt;
    auto attributes = attributes_for(path, target.name, options.filename_encoding, *value);
    if (browser_directories.contains(path) || is_public_root(target)) attributes.role = RemoteFileRole::nfs_export;
    co_return attributes;
  }

  cardio::promise<std::unique_ptr<RemoteFileReader>> open_read_async(std::string path,
      cardio::cancellation cancellation) override {
    auto lock = std::move(co_await operations.lock(cancellation));
    const auto target = locate(path);
    auto session = co_await mounted(target, cancellation);
    if (options.version == 2) {
      auto reader = std::move(co_await read_nfs2_file_async(exports.at(target.root).v2, target.native, cancellation));
      co_return reader;
    }
    auto session_lock = std::move(co_await session->operations.lock(cancellation));
    auto reader = std::make_unique<NfsReader>();
    reader->session = session;
    reader->chunk_limit = exports.at(target.root).limits->read;
    check_nfs_result(co_await execute_nfs_async(session, [&](auto cb, auto data) {
      return nfs_open_async(session->nfs, target.native.c_str(), O_RDONLY | O_NOFOLLOW, cb, data);
    }, [&](int, void *data) {
      reader->handle = static_cast<nfsfh *>(data);
      session->handles.insert(reader->handle);
    }, cancellation), "open for reading");
    co_return reader;
  }

  cardio::promise<std::unique_ptr<RemoteFileWriter>> open_write_async(std::string path,
      std::uint64_t size, std::optional<std::uint32_t> permissions, cardio::cancellation cancellation) override {
    auto lock = std::move(co_await operations.lock(cancellation));
    const auto target = locate(path);
    require_mutable(target);
    auto session = co_await mounted(target, cancellation);
    if (options.version == 2) {
      auto writer = std::move(co_await write_nfs2_file_async(exports.at(target.root).v2, target.native, size, permissions, cancellation));
      co_return writer;
    }
    auto session_lock = std::move(co_await session->operations.lock(cancellation));
    auto writer = std::make_unique<NfsWriter>();
    writer->session = session;
    writer->path = target.native;
    writer->chunk_limit = exports.at(target.root).limits->write;
    writer->permissions = permissions.value_or(0600);
    writer->expected_size = size;
    // Return ownership tracking before CREATE can reach the server. If its
    // reply is lost, the transfer engine can report the uncertain temporary.
    co_return writer;
  }

  cardio::promise<void> make_directory_async(std::string path, std::optional<std::uint32_t> permissions,
      cardio::cancellation cancellation) override {
    auto lock = std::move(co_await operations.lock(cancellation));
    const auto target = locate(path);
    require_mutable(target);
    auto session = co_await mounted(target, cancellation);
    if (options.version == 2) {
      co_await make_nfs2_directory_async(exports.at(target.root).v2, target.native, permissions, cancellation);
      co_return;
    }
    auto session_lock = std::move(co_await session->operations.lock(cancellation));
    co_await perform(session, "create directory", [&](auto cb, auto data) {
      return nfs_mkdir2_async(session->nfs, target.native.c_str(), permissions.value_or(0755) & 0777, cb, data);
    }, cancellation);
  }

  cardio::promise<void> remove(std::string path, bool directory, cardio::cancellation cancellation) {
    auto lock = std::move(co_await operations.lock(cancellation));
    const auto target = locate(path);
    require_mutable(target);
    auto session = co_await mounted(target, cancellation);
    if (options.version == 2) {
      co_await remove_nfs2_async(exports.at(target.root).v2, target.native, directory, cancellation);
      co_return;
    }
    auto session_lock = std::move(co_await session->operations.lock(cancellation));
    co_await perform(session, "remove", [&](auto cb, auto data) {
      return directory ? nfs_rmdir_async(session->nfs, target.native.c_str(), cb, data) :
          nfs_unlink_async(session->nfs, target.native.c_str(), cb, data);
    }, cancellation);
  }
  cardio::promise<void> remove_file_async(std::string path, cardio::cancellation cancellation) override {
    co_await remove(std::move(path), false, cancellation);
  }
  cardio::promise<void> remove_directory_async(std::string path, cardio::cancellation cancellation) override {
    co_await remove(std::move(path), true, cancellation);
  }

  cardio::promise<void> rename(std::string source, std::string destination, bool overwrite, bool upload,
      cardio::cancellation cancellation) {
    auto lock = std::move(co_await operations.lock(cancellation));
    const auto from = locate(source), to = locate(destination);
    require_mutable(from); require_mutable(to);
    if (from.root != to.root) throw RemoteFileError("Cannot rename across NFS exports", false);
    auto session = co_await mounted(from, cancellation);
    if (options.version == 2) {
      co_await rename_nfs2_async(exports.at(from.root).v2, from.native, to.native, overwrite, upload, cancellation);
      co_return;
    }
    auto session_lock = std::move(co_await session->operations.lock(cancellation));
    if (upload && !overwrite) {
      // LINK creates the final name atomically only when absent. The adjacent
      // exclusive upload remains owned until its own temporary name is removed.
      co_await perform(session, "commit upload", [&](auto cb, auto data) {
        return nfs_link_async(session->nfs, from.native.c_str(), to.native.c_str(), cb, data);
      }, cancellation);
      co_await perform(session, "remove committed temporary", [&](auto cb, auto data) {
        return nfs_unlink_async(session->nfs, from.native.c_str(), cb, data);
      }, cancellation);
    } else {
      if (!overwrite && (co_await stat_native(session, to.native, cancellation)))
        throw RemoteFileError("NFS rename destination already exists", false);
      co_await perform(session, "rename", [&](auto cb, auto data) {
        return nfs_rename_async(session->nfs, from.native.c_str(), to.native.c_str(), cb, data);
      }, cancellation);
    }
  }
  cardio::promise<void> rename_async(std::string source, std::string destination,
      cardio::cancellation cancellation) override {
    co_await rename(std::move(source), std::move(destination), false, false, cancellation);
  }
  cardio::promise<void> commit_upload_async(std::string source, std::string destination,
      bool overwrite, cardio::cancellation cancellation) override {
    co_await rename(std::move(source), std::move(destination), overwrite, true, cancellation);
  }

  cardio::promise<std::string> read_link_async(std::string path, cardio::cancellation cancellation) override {
    auto lock = std::move(co_await operations.lock(cancellation));
    const auto target = locate(path);
    auto session = co_await mounted(target, cancellation);
    if (options.version == 2)
      co_return co_await read_nfs2_link_async(exports.at(target.root).v2, target.native, cancellation);
    auto session_lock = std::move(co_await session->operations.lock(cancellation));
    if (options.version == 4) {
      co_return co_await read_v4_link(session, target.native, cancellation);
    }
    std::string value;
    check_nfs_result(co_await execute_nfs_async(session, [&](auto cb, auto data) {
      return nfs_readlink_async(session->nfs, target.native.c_str(), cb, data);
    }, [&](int, void *data) { value = static_cast<char *>(data); }, cancellation), "read link");
    co_return value;
  }
  cardio::promise<void> make_symbolic_link_async(std::string target, std::string path,
      cardio::cancellation cancellation) override {
    if (target.find('\0') != std::string::npos) throw RemoteFileError("Invalid NFS link target", false);
    auto lock = std::move(co_await operations.lock(cancellation));
    const auto location = locate(path);
    require_mutable(location);
    auto session = co_await mounted(location, cancellation);
    if (options.version == 2) {
      co_await make_nfs2_link_async(exports.at(location.root).v2, target, location.native, cancellation);
      co_return;
    }
    auto session_lock = std::move(co_await session->operations.lock(cancellation));
    co_await perform(session, "create link", [&](auto cb, auto data) {
      return nfs_symlink_async(session->nfs, target.c_str(), location.native.c_str(), cb, data);
    }, cancellation);
  }

  cardio::promise<void> set_attributes_async(std::string path, RemoteFileAttributes attributes,
      cardio::cancellation cancellation) override {
    auto lock = std::move(co_await operations.lock(cancellation));
    const auto target = locate(path);
    require_mutable(target);
    auto session = co_await mounted(target, cancellation);
    if (options.version == 2) {
      co_await set_nfs2_attributes_async(exports.at(target.root).v2, target.native, attributes, cancellation);
      co_return;
    }
    auto session_lock = std::move(co_await session->operations.lock(cancellation));
    if (attributes.permissions && attributes.type != RemoteFileType::symbolic_link)
      co_await perform(session, "permissions", [&](auto cb, auto data) {
        return nfs_lchmod_async(session->nfs, target.native.c_str(), *attributes.permissions & 0777, cb, data);
      }, cancellation);
    if (attributes.access_time_unix_seconds || attributes.modification_time_unix_seconds) {
      const auto current = co_await stat_native(session, target.native, cancellation);
      if (!current) throw RemoteFileError("NFS item disappeared before updating timestamps", false);
      timeval times[2]{};
      times[0].tv_sec = attributes.access_time_unix_seconds.value_or(current->nfs_atime);
      times[1].tv_sec = attributes.modification_time_unix_seconds.value_or(current->nfs_mtime);
      co_await perform(session, "timestamps", [&](auto cb, auto data) {
        return nfs_lutimes_async(session->nfs, target.native.c_str(), times, cb, data);
      }, cancellation);
    }
  }
};

static std::string encode_export(const std::string &input, const std::string &encoding) {
  if (input.empty() || input.front() != '/') throw RemoteFileError("Known NFS exports must be absolute paths", false);
  if (input == "/") return input;
  std::string path;
  for (std::size_t start = 1; start < input.size();) {
    const auto next = input.find('/', start);
    path += "/" + encode_nfs_name(input.substr(start, next == std::string::npos ? next : next - start), encoding, 255);
    if (next == std::string::npos) break;
    start = next + 1;
  }
  if (path.size() > 1024) throw RemoteFileError("NFS export path is too long", false);
  return path;
}

cardio::promise<std::shared_ptr<RemoteFileClient>> open_nfs_server_async(
    NfsClientOpenOptions options, cardio::cancellation cancellation) {
  cancellation.throw_if_cancellation_requested();
  if (options.version != 2 && options.version != 3 && options.version != 4)
    throw RemoteFileError("Select NFS v2, v3 or v4.0", false);
  if (options.address.empty() || options.address.find_first_of(" /\\@?#\r\n") != std::string::npos ||
      options.address.find('\0') != std::string::npos || options.nfs_port < 0 || options.nfs_port > 65535 ||
      options.mount_port < 0 || options.mount_port > 65535 || options.connect_timeout_ms <= 0 ||
      options.request_timeout_ms <= 0 || options.supplementary_gids.size() > 16)
    throw RemoteFileError("Invalid NFS connection settings", false);
  validate_nfs_filename_encoding(options.filename_encoding);
  auto server = std::make_shared<NfsServer>();
  server->options = std::move(options);
  // libnfs's public connect entry points resolve hostnames synchronously. Pass
  // only numeric addresses after completing cancellable GIO resolution.
  server->addresses = co_await resolve_addresses_async(server->options.address, server->options.connect_timeout_ms, cancellation);
  server->generation = "nfs-" + std::to_string(next_generation.fetch_add(1)) + "-";
  const auto &paths = server->options.version == 4 ? server->options.known_namespace_paths : server->options.known_exports;
  for (const auto &path : paths)
    server->add_export(encode_export(path, server->options.filename_encoding));
  if (server->options.version == 4 && server->options.discover_namespace) {
    const auto root_id = "/" + server->generation + nfs_identifier_component("/");
    const bool registered_root = server->exports.contains(root_id);
    server->add_export("/");
    try {
      const auto root = co_await server->load_directory_async(root_id, cancellation);
      for (const auto &entry : root.entries) {
        if (entry.type == RemoteFileType::directory)
          server->add_export("/" + nfs_identifier_bytes(entry.path.substr(entry.path.find_last_of('/') + 1)));
      }
    } catch (const cardio::canceled_exception &) { throw; }
    catch (const std::exception &error) {
      if (!registered_root) server->exports.erase(root_id);
      server->discovery_error = error.what();
      if (server->exports.empty()) throw;
    }
  }
  if (server->options.version != 4 && server->options.discover_exports) {
    for (const auto &address : server->addresses) {
      try {
        auto mount = create_nfs_raw_session(server->options.connect_timeout_ms);
        if (server->options.uid) rpc_set_uid(mount->rpc, std::bit_cast<std::int32_t>(*server->options.uid));
        if (server->options.gid) rpc_set_gid(mount->rpc, std::bit_cast<std::int32_t>(*server->options.gid));
        auto groups = server->options.supplementary_gids;
        rpc_set_auxiliary_gids(mount->rpc, groups.size(), groups.data());
        co_await connect_nfs_raw_async(mount, address, server->options.mount_port, MOUNT_PROGRAM,
            server->options.version == 2 ? MOUNT_V1 : MOUNT_V3, cancellation);
        co_await execute_nfs_raw_async(mount, [&](auto cb, auto data) {
          return server->options.version == 2 ? ELDER_NFS_RAW_CALL(rpc_mount1_export, mount->rpc, cb, data) :
              ELDER_NFS_RAW_CALL(rpc_mount3_export, mount->rpc, cb, data);
        }, [&](void *data) {
          for (auto *node = *static_cast<::exports *>(data); node;) {
            // RAW allocations in system libnfs 5.0.2 may be four-byte aligned.
            exportnode entry{};
            std::memcpy(&entry, node, sizeof(entry));
            server->add_export(entry.ex_dir);
            node = entry.ex_next;
          }
        }, cancellation);
        server->discovery_error.clear();
        break;
      } catch (const cardio::canceled_exception &) { throw; }
      catch (const std::exception &error) {
        server->discovery_error = error.what();
      }
    }
    if (server->exports.empty() && !server->discovery_error.empty())
      throw RemoteFileError(server->discovery_error, true);
  }
  if (server->exports.empty()) throw RemoteFileError("No NFS public paths were found; configure a known path", false);
  server->build_browser_tree();
  co_return server;
}

cardio::promise<void> stop_nfs_client_async(std::shared_ptr<RemoteFileClient> client) {
  if (!client) co_return;
  auto server = std::dynamic_pointer_cast<NfsServer>(client);
  if (!server) throw std::invalid_argument("Expected an NFS server client");
  server->stop();
  auto lock = std::move(co_await server->operations.lock());
  for (auto &[id, item] : server->exports) {
    (void)id;
    if (item.session) { auto session_lock = std::move(co_await item.session->operations.lock()); }
  }
}

std::string nfs_export_identity(const std::shared_ptr<RemoteFileClient> &client, const std::string &export_path) {
  const auto server = std::dynamic_pointer_cast<NfsServer>(client);
  if (!server) throw std::invalid_argument("Expected an NFS server client");
  server->require_open();
  if (export_path.empty()) return "/";
  const auto raw = encode_export(export_path, server->options.filename_encoding);
  for (const auto &[id, item] : server->exports) if (item.bytes == raw) return id;
  throw RemoteFileError("The configured NFS export is unavailable", false);
}

} // namespace elder_terms
