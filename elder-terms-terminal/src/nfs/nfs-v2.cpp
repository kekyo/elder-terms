// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "nfs-v2.h"
#include "nfs-api.h"
#include <nfsc/libnfs-raw-mount.h>
#include <nfsc/libnfs-raw-nfs.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <map>
#include <set>

namespace elder_terms {

struct Nfs2Upload {
  std::array<char, FHSIZE> parent{}, directory{}, file{};
  std::string path, name;
  std::uint32_t expected_size = 0, offset = 0, permissions = 0600;
  RemoteFileCreationState creation = RemoteFileCreationState::not_created;
  bool attempted = false, ready = false, closed = false;
};

struct Nfs2Filesystem {
  std::shared_ptr<NfsRpcSession> session;
  std::array<char, FHSIZE> root{};
  std::uint32_t chunk_size = NFSMAXDATA2;
  std::map<std::string, std::shared_ptr<Nfs2Upload>> uploads;
};

static void require_nfs2(const std::shared_ptr<Nfs2Filesystem> &filesystem) {
  if (!filesystem->session->rpc || filesystem->session->stopping.get_cancellation().is_cancellation_requested())
    throw RemoteFileError("NFS v2 connection has ended. Reconnect to continue.", true);
}

static void check_nfs2(nfsstat3 status, const char *operation) {
  if (status == NFS3_OK) return;
  throw RemoteFileError(std::string("NFS v2 ") + operation + ": " + nfsstat3_to_str(status), status == NFS3ERR_STALE);
}

static RemoteFileAttributes nfs2_attributes(const fattr2 &value) {
  RemoteFileAttributes result;
  result.type = value.type == NF2REG ? RemoteFileType::regular : value.type == NF2DIR ? RemoteFileType::directory :
      value.type == NF2LNK ? RemoteFileType::symbolic_link : RemoteFileType::other;
  result.size = value.size;
  result.permissions = value.mode;
  result.access_time_unix_seconds = value.atime.seconds;
  result.modification_time_unix_seconds = value.mtime.seconds;
  return result;
}

static void configure_nfs2_auth(rpc_context *rpc, const NfsClientOpenOptions &options) {
  if (options.uid) rpc_set_uid(rpc, std::bit_cast<std::int32_t>(*options.uid));
  if (options.gid) rpc_set_gid(rpc, std::bit_cast<std::int32_t>(*options.gid));
  auto groups = options.supplementary_gids;
  rpc_set_auxiliary_gids(rpc, groups.size(), groups.data());
}

cardio::promise<std::shared_ptr<Nfs2Filesystem>> open_nfs2_filesystem_async(
    std::shared_ptr<NfsRpcSession> session, NfsClientOpenOptions options,
    std::string address, std::string export_path, cardio::cancellation cancellation) {
  auto filesystem = std::make_shared<Nfs2Filesystem>();
  filesystem->session = session;
  auto stopped = cardio::cancellations::any(cancellation, session->stopping.get_cancellation());
  struct CancellationEnd { cardio::cancellation_source &source; ~CancellationEnd() { (void)source.cancel(); } } end{stopped};
  const auto stop = stopped.get_cancellation();
  auto mount = create_nfs_raw_session(options.connect_timeout_ms);
  configure_nfs2_auth(mount->rpc, options);
  co_await connect_nfs_raw_async(mount, address, options.mount_port, MOUNT_PROGRAM, MOUNT_V1, stop);
  mountres1 mounted{};
  co_await execute_nfs_raw_async(mount, [&](auto cb, auto data) {
    return ELDER_NFS_RAW_CALL(rpc_mount1_mnt, mount->rpc, cb, export_path.data(), data);
  }, [&](void *data) { std::memcpy(&mounted, data, sizeof(mounted)); }, stop);
  if (mounted.fhs_status != MNT1_OK)
    throw RemoteFileError("NFS v2 mount refused: " + std::to_string(mounted.fhs_status), false);
  std::memcpy(filesystem->root.data(), mounted.mountres1_u.mountinfo.fhandle, filesystem->root.size());
  configure_nfs2_auth(session->rpc, options);
  co_await connect_nfs_raw_async(session, address, options.nfs_port, NFS_PROGRAM, NFS_V2, stop);
  STATFS2args args{};
  std::memcpy(args.dir, filesystem->root.data(), filesystem->root.size());
  STATFS2res limits{};
  co_await execute_nfs_raw_async(session, [&](auto cb, auto data) {
    return ELDER_NFS_RAW_CALL(rpc_nfs2_statfs, session->rpc, cb, &args, data);
  }, [&](void *data) { std::memcpy(&limits, data, sizeof(limits)); }, stop);
  check_nfs2(limits.status, "filesystem limits");
  const auto size = limits.STATFS2res_u.resok.tsize;
  if (size == 0) throw RemoteFileError("NFS v2 server reported a zero transfer limit", false);
  filesystem->chunk_size = std::min<std::uint32_t>(size, NFSMAXDATA2);
  co_return filesystem;
}

static cardio::promise<fattr2> nfs2_get_attributes(const std::shared_ptr<Nfs2Filesystem> &filesystem,
    const std::array<char, FHSIZE> &handle, cardio::cancellation cancellation) {
  GETATTR2args args{};
  std::memcpy(args.fhandle, handle.data(), handle.size());
  GETATTR2res reply{};
  co_await execute_nfs_raw_async(filesystem->session, [&](auto cb, auto data) {
    return ELDER_NFS_RAW_CALL(rpc_nfs2_getattr, filesystem->session->rpc, cb, &args, data);
  }, [&](void *data) { std::memcpy(&reply, data, sizeof(reply)); }, cancellation);
  check_nfs2(reply.status, "attributes");
  co_return reply.GETATTR2res_u.resok.attributes;
}

struct Nfs2Location {
  std::array<char, FHSIZE> handle;
  fattr2 attributes;
};

static void nfs2_check_name(const std::string &name) {
  if (name.empty() || name == "." || name == ".." || name.size() > MAXNAMLEN2 ||
      name.find('/') != std::string::npos || name.find('\0') != std::string::npos)
    throw RemoteFileError("Invalid NFS v2 path component", false);
}

static cardio::promise<std::optional<Nfs2Location>> nfs2_lookup_child(
    const std::shared_ptr<Nfs2Filesystem> &filesystem, const std::array<char, FHSIZE> &parent,
    std::string name, cardio::cancellation cancellation) {
  nfs2_check_name(name);
  LOOKUP2args args{};
  std::memcpy(args.what.dir, parent.data(), parent.size());
  args.what.name = name.data();
  LOOKUP2res reply{};
  co_await execute_nfs_raw_async(filesystem->session, [&](auto cb, auto data) {
    return ELDER_NFS_RAW_CALL(rpc_nfs2_lookup, filesystem->session->rpc, cb, &args, data);
  }, [&](void *data) { std::memcpy(&reply, data, sizeof(reply)); }, cancellation);
  if (reply.status == NFS3ERR_NOENT) co_return std::nullopt;
  check_nfs2(reply.status, "lookup");
  Nfs2Location location{{}, reply.LOOKUP2res_u.resok.attributes};
  std::memcpy(location.handle.data(), reply.LOOKUP2res_u.resok.file, location.handle.size());
  co_return location;
}

static cardio::promise<std::optional<Nfs2Location>> nfs2_resolve(
    const std::shared_ptr<Nfs2Filesystem> &filesystem, const std::string &path,
    cardio::cancellation cancellation) {
  require_nfs2(filesystem);
  if (path.empty() || path.front() != '/') throw RemoteFileError("NFS v2 requires an export-relative absolute path", false);
  Nfs2Location location{filesystem->root, co_await nfs2_get_attributes(filesystem, filesystem->root, cancellation)};
  for (std::size_t start = 1; start < path.size();) {
    if (location.attributes.type != NF2DIR)
      throw RemoteFileError("NFS v2 does not traverse symbolic links in parent paths", false);
    const auto next = path.find('/', start);
    const auto child = co_await nfs2_lookup_child(filesystem, location.handle,
        path.substr(start, next == std::string::npos ? next : next - start), cancellation);
    if (!child) co_return std::nullopt;
    location = *child;
    if (next == std::string::npos) break;
    start = next + 1;
  }
  co_return location;
}

cardio::promise<std::optional<RemoteFileAttributes>> stat_nfs2_async(
    std::shared_ptr<Nfs2Filesystem> filesystem, std::string path, cardio::cancellation cancellation) {
  auto lock = std::move(co_await filesystem->session->operations.lock(cancellation));
  const auto found = co_await nfs2_resolve(filesystem, path, cancellation);
  if (!found) co_return std::nullopt;
  co_return nfs2_attributes(found->attributes);
}

cardio::promise<std::vector<Nfs2DirectoryEntry>> list_nfs2_directory_async(
    std::shared_ptr<Nfs2Filesystem> filesystem, std::string path, cardio::cancellation cancellation) {
  auto lock = std::move(co_await filesystem->session->operations.lock(cancellation));
  const auto directory = co_await nfs2_resolve(filesystem, path, cancellation);
  if (!directory || directory->attributes.type != NF2DIR) throw RemoteFileError("NFS v2 directory is unavailable", false);
  READDIR2args args{};
  std::memcpy(args.dir, directory->handle.data(), directory->handle.size());
  args.count = NFSMAXDATA2;
  std::set<std::string> cookies{std::string(args.cookie, sizeof(args.cookie))};
  std::set<std::string> names;
  std::vector<Nfs2DirectoryEntry> entries;
  bool eof = false;
  while (!eof) {
    nfsstat3 status = NFS3_OK;
    std::vector<std::string> page;
    co_await execute_nfs_raw_async(filesystem->session, [&](auto cb, auto data) {
      return ELDER_NFS_RAW_CALL(rpc_nfs2_readdir, filesystem->session->rpc, cb, &args, data);
    }, [&](void *data) {
      READDIR2res reply{};
      std::memcpy(&reply, data, sizeof(reply));
      status = reply.status;
      if (status != NFS3_OK) return;
      eof = reply.READDIR2res_u.resok.eof;
      for (auto *node = reply.READDIR2res_u.resok.entries; node;) {
        // The system library's RAW allocations can have four-byte alignment.
        entry2 entry{};
        std::memcpy(&entry, node, sizeof(entry));
        const std::string name(entry.name);
        if (name != "." && name != "..") page.push_back(name);
        std::memcpy(args.cookie, entry.cookie, sizeof(args.cookie));
        node = entry.nextentry;
      }
    }, cancellation);
    check_nfs2(status, "list directory");
    if (!eof && !cookies.insert(std::string(args.cookie, sizeof(args.cookie))).second)
      throw RemoteFileError("NFS v2 directory cookie made no progress; reload the directory", false);
    for (auto &name : page) {
      if (!names.insert(name).second) throw RemoteFileError("NFS v2 directory returned a duplicate entry; reload it", false);
      const auto item = co_await nfs2_lookup_child(filesystem, directory->handle, name, cancellation);
      if (item) entries.push_back({std::move(name), nfs2_attributes(item->attributes)});
    }
  }
  co_return entries;
}

struct Nfs2Reader final : RemoteFileReader {
  std::shared_ptr<Nfs2Filesystem> filesystem;
  std::array<char, FHSIZE> handle;
  std::uint64_t offset = 0;
  std::uint32_t expected_size = 0;
  bool closed = false;

  cardio::promise<std::size_t> read_async(std::span<std::byte> buffer, cardio::cancellation cancellation) override {
    auto lock = std::move(co_await filesystem->session->operations.lock(cancellation));
    require_nfs2(filesystem);
    if (closed) throw RemoteFileError("NFS v2 reader is closed", false);
    if (buffer.empty()) co_return 0;
    if (offset > UINT32_MAX) throw RemoteFileError("NFS v2 read offset exceeds 32 bits", false);
    READ2args args{};
    std::memcpy(args.file, handle.data(), handle.size());
    args.offset = static_cast<std::uint32_t>(offset);
    args.count = static_cast<std::uint32_t>(std::min<std::size_t>(buffer.size(), filesystem->chunk_size));
    nfsstat3 status = NFS3_OK;
    std::size_t count = 0;
    std::uint32_t size = 0;
    co_await execute_nfs_raw_async(filesystem->session, [&](auto cb, auto data) {
      return ELDER_NFS_RAW_CALL(rpc_nfs2_read, filesystem->session->rpc, cb, &args, data);
    }, [&](void *data) {
      READ2res reply{};
      std::memcpy(&reply, data, sizeof(reply));
      status = reply.status;
      if (status != NFS3_OK) return;
      count = reply.READ2res_u.resok.data.nfsdata2_len;
      size = reply.READ2res_u.resok.attributes.size;
      if (count > args.count || offset + count > UINT32_MAX)
        throw RemoteFileError("NFS v2 read returned an invalid range", true);
      if (count) std::memcpy(buffer.data(), reply.READ2res_u.resok.data.nfsdata2_val, count);
    }, cancellation);
    check_nfs2(status, "read");
    // The test server wraps a 4 GiB size to zero in v2 attributes. A data
    // response beyond that size must never become a successful empty copy.
    if (size != expected_size || offset + count > size)
      throw RemoteFileError("NFS v2 file size changed or cannot be represented in 32 bits", false);
    if (!count && offset < size) throw RemoteFileError("NFS v2 read made no progress before EOF", false);
    offset += count;
    co_return count;
  }
  cardio::promise<void> close_async(cardio::cancellation cancellation) override {
    auto lock = std::move(co_await filesystem->session->operations.lock(cancellation));
    closed = true;
    // v2 has no OPEN/CLOSE state; the copied handle owns no server resource.
  }
};

cardio::promise<std::unique_ptr<RemoteFileReader>> read_nfs2_file_async(
    std::shared_ptr<Nfs2Filesystem> filesystem, std::string path, cardio::cancellation cancellation) {
  auto lock = std::move(co_await filesystem->session->operations.lock(cancellation));
  const auto item = co_await nfs2_resolve(filesystem, path, cancellation);
  if (!item || item->attributes.type != NF2REG) throw RemoteFileError("NFS v2 read requires a regular file", false);
  // Some servers saturate unrepresentable sizes at UINT32_MAX. Refuse that
  // ambiguous boundary instead of reporting a truncated large file as complete.
  if (item->attributes.size == UINT32_MAX) throw RemoteFileError("NFS v2 file size reaches its 32-bit limit", false);
  auto reader = std::make_unique<Nfs2Reader>();
  reader->filesystem = filesystem;
  reader->handle = item->handle;
  reader->expected_size = item->attributes.size;
  co_return reader;
}

cardio::promise<std::string> read_nfs2_link_async(
    std::shared_ptr<Nfs2Filesystem> filesystem, std::string path, cardio::cancellation cancellation) {
  auto lock = std::move(co_await filesystem->session->operations.lock(cancellation));
  const auto item = co_await nfs2_resolve(filesystem, path, cancellation);
  if (!item || item->attributes.type != NF2LNK) throw RemoteFileError("NFS v2 item is not a symbolic link", false);
  READLINK2args args{};
  std::memcpy(args.file, item->handle.data(), item->handle.size());
  nfsstat3 status = NFS3_OK;
  std::string value;
  co_await execute_nfs_raw_async(filesystem->session, [&](auto cb, auto data) {
    return ELDER_NFS_RAW_CALL(rpc_nfs2_readlink, filesystem->session->rpc, cb, &args, data);
  }, [&](void *data) {
    READLINK2res reply{};
    std::memcpy(&reply, data, sizeof(reply));
    status = reply.status;
    if (status == NFS3_OK) value = reply.READLINK2res_u.resok.data;
  }, cancellation);
  check_nfs2(status, "read link");
  co_return value;
}

// v2 uses all-one fields to leave individual attributes unchanged. The public
// libnfs type calls the subsecond member nseconds, but v2 transmits microseconds.
static sattr2 nfs2_unchanged_attributes() {
  return {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX,
      {UINT32_MAX, UINT32_MAX}, {UINT32_MAX, UINT32_MAX}};
}

struct Nfs2Parent {
  std::array<char, FHSIZE> handle;
  std::string name;
};

static cardio::promise<Nfs2Parent> nfs2_parent(const std::shared_ptr<Nfs2Filesystem> &filesystem,
    const std::string &path, cardio::cancellation cancellation) {
  const auto slash = path.find_last_of('/');
  if (slash == std::string::npos) throw RemoteFileError("NFS v2 requires an absolute path", false);
  auto name = path.substr(slash + 1);
  nfs2_check_name(name);
  const auto parent = co_await nfs2_resolve(filesystem, slash == 0 ? "/" : path.substr(0, slash), cancellation);
  if (!parent || parent->attributes.type != NF2DIR) throw RemoteFileError("NFS v2 parent is not a directory", false);
  co_return Nfs2Parent{parent->handle, std::move(name)};
}

static cardio::promise<void> nfs2_set_attributes(const std::shared_ptr<Nfs2Filesystem> &filesystem,
    const std::array<char, FHSIZE> &handle, sattr2 attributes, cardio::cancellation cancellation) {
  SETATTR2args args{};
  std::memcpy(args.fhandle, handle.data(), handle.size());
  args.attributes = attributes;
  SETATTR2res reply{};
  co_await execute_nfs_raw_async(filesystem->session, [&](auto cb, auto data) {
    return ELDER_NFS_RAW_CALL(rpc_nfs2_setattr, filesystem->session->rpc, cb, &args, data);
  }, [&](void *data) { std::memcpy(&reply, data, sizeof(reply)); }, cancellation);
  check_nfs2(reply.status, "set attributes");
}

static cardio::promise<void> nfs2_remove_child(const std::shared_ptr<Nfs2Filesystem> &filesystem,
    const std::array<char, FHSIZE> &parent, std::string name, bool directory, cardio::cancellation cancellation) {
  REMOVE2args file{};
  std::memcpy(file.what.dir, parent.data(), parent.size());
  file.what.name = name.data();
  RMDIR2args folder{file.what};
  nfsstat3 status = NFS3_OK;
  co_await execute_nfs_raw_async(filesystem->session, [&](auto cb, auto data) {
    return directory ? ELDER_NFS_RAW_CALL(rpc_nfs2_rmdir, filesystem->session->rpc, cb, &folder, data) :
        ELDER_NFS_RAW_CALL(rpc_nfs2_remove, filesystem->session->rpc, cb, &file, data);
  }, [&](void *data) { std::memcpy(&status, data, sizeof(status)); }, cancellation);
  check_nfs2(status, directory ? "remove directory" : "remove file");
}

static cardio::promise<void> nfs2_clean_upload(const std::shared_ptr<Nfs2Filesystem> &filesystem,
    const std::shared_ptr<Nfs2Upload> &upload, cardio::cancellation cancellation) {
  if (upload->creation != RemoteFileCreationState::created)
    throw RemoteFileError("NFS v2 temporary upload ownership is not proven", false);
  const auto directory = co_await nfs2_lookup_child(filesystem, upload->parent, upload->name, cancellation);
  if (!directory || directory->handle != upload->directory)
    throw RemoteFileError("NFS v2 temporary directory identity changed; inspect it before cleanup", false);
  const auto file = co_await nfs2_lookup_child(filesystem, upload->directory, "payload", cancellation);
  if (file) {
    if (upload->ready && file->handle != upload->file)
      throw RemoteFileError("NFS v2 temporary file identity changed; inspect it before cleanup", false);
    co_await nfs2_remove_child(filesystem, upload->directory, "payload", false, cancellation);
  }
  co_await nfs2_remove_child(filesystem, upload->parent, upload->name, true, cancellation);
  filesystem->uploads.erase(upload->path);
}

struct Nfs2Writer final : RemoteFileWriter {
  std::shared_ptr<Nfs2Filesystem> filesystem;
  std::shared_ptr<Nfs2Upload> upload;
  ~Nfs2Writer() override {
    // Rejected or never-started creation owns no remote resource. Retaining
    // that local reservation would prevent a later explicit retry.
    if (upload->creation == RemoteFileCreationState::not_created) {
      const auto found = filesystem->uploads.find(upload->path);
      if (found != filesystem->uploads.end() && found->second == upload) filesystem->uploads.erase(found);
    }
  }
  RemoteFileCreationState creation_state() const noexcept override { return upload->creation; }

  cardio::promise<void> ensure_created(cardio::cancellation cancellation) {
    require_nfs2(filesystem);
    if (upload->ready) co_return;
    if (upload->attempted || upload->closed) throw RemoteFileError("NFS v2 upload creation cannot be replayed", false);
    cancellation.throw_if_cancellation_requested();
    upload->attempted = true;
    MKDIR2args directory{};
    std::memcpy(directory.where.dir, upload->parent.data(), upload->parent.size());
    directory.where.name = upload->name.data();
    directory.attributes = nfs2_unchanged_attributes();
    directory.attributes.mode = 0700;
    MKDIR2res made{};
    co_await execute_nfs_raw_async(filesystem->session, [&](auto cb, auto data) {
      upload->creation = RemoteFileCreationState::unknown;
      const int queued = ELDER_NFS_RAW_CALL(rpc_nfs2_mkdir, filesystem->session->rpc, cb, &directory, data);
      if (queued < 0) upload->creation = RemoteFileCreationState::not_created;
      return queued;
    }, [&](void *data) {
      std::memcpy(&made, data, sizeof(made));
      upload->creation = made.status == NFS3_OK ? RemoteFileCreationState::created : RemoteFileCreationState::not_created;
      if (made.status == NFS3_OK)
        std::memcpy(upload->directory.data(), made.MKDIR2res_u.resok.file, upload->directory.size());
    }, cancellation);
    check_nfs2(made.status, "claim upload directory");
    // CREATE itself is not exclusive. It is safe only inside the fresh private
    // directory whose successful MKDIR response proved this upload owns it.
    CREATE2args file{};
    std::memcpy(file.where.dir, upload->directory.data(), upload->directory.size());
    std::string payload = "payload";
    file.where.name = payload.data();
    file.attributes = nfs2_unchanged_attributes();
    file.attributes.mode = 0600;
    file.attributes.size = 0;
    CREATE2res created{};
    co_await execute_nfs_raw_async(filesystem->session, [&](auto cb, auto data) {
      return ELDER_NFS_RAW_CALL(rpc_nfs2_create, filesystem->session->rpc, cb, &file, data);
    }, [&](void *data) { std::memcpy(&created, data, sizeof(created)); }, cancellation);
    check_nfs2(created.status, "create upload payload");
    std::memcpy(upload->file.data(), created.CREATE2res_u.resok.file, upload->file.size());
    upload->ready = true;
  }

  cardio::promise<void> write_all_async(std::span<const std::byte> buffer, cardio::cancellation cancellation) override {
    auto lock = std::move(co_await filesystem->session->operations.lock(cancellation));
    if (upload->closed) throw RemoteFileError("NFS v2 writer is closed", false);
    if (upload->offset > upload->expected_size || buffer.size() > upload->expected_size - upload->offset)
      throw RemoteFileError("NFS v2 upload exceeds its declared size", false);
    if (buffer.empty()) co_return;
    co_await ensure_created(cancellation);
    while (!buffer.empty()) {
      const auto count = static_cast<std::uint32_t>(std::min<std::size_t>(buffer.size(), filesystem->chunk_size));
      WRITE2args args{};
      std::memcpy(args.file, upload->file.data(), upload->file.size());
      args.offset = upload->offset;
      // Although unused by the protocol, system libnfs 5.0.2 sizes its outgoing
      // buffer from totalcount. It must cover the encoded data in this request.
      args.totalcount = count;
      args.data.nfsdata2_len = count;
      args.data.nfsdata2_val = reinterpret_cast<char *>(const_cast<std::byte *>(buffer.data()));
      WRITE2res reply{};
      co_await execute_nfs_raw_async(filesystem->session, [&](auto cb, auto data) {
        return ELDER_NFS_RAW_CALL(rpc_nfs2_write, filesystem->session->rpc, cb, &args, data);
      }, [&](void *data) { std::memcpy(&reply, data, sizeof(reply)); }, cancellation);
      check_nfs2(reply.status, "write");
      // v2 acknowledges the whole request, without a separate byte count.
      if (reply.WRITE2res_u.resok.attributes.size != upload->offset + count)
        throw RemoteFileError("NFS v2 WRITE returned an incomplete or changed file size", false);
      upload->offset += count;
      buffer = buffer.subspan(count);
    }
  }

  cardio::promise<void> close_async(cardio::cancellation cancellation) override {
    auto lock = std::move(co_await filesystem->session->operations.lock(cancellation));
    if (upload->closed) co_return;
    if (!upload->attempted && upload->expected_size == 0) co_await ensure_created(cancellation);
    upload->closed = true;
    if (!upload->ready || upload->offset != upload->expected_size)
      throw RemoteFileError("NFS v2 upload did not finish its declared size", false);
    auto attributes = nfs2_unchanged_attributes();
    attributes.mode = upload->permissions & 0777;
    co_await nfs2_set_attributes(filesystem, upload->file, attributes, cancellation);
  }
};

cardio::promise<std::unique_ptr<RemoteFileWriter>> write_nfs2_file_async(
    std::shared_ptr<Nfs2Filesystem> filesystem, std::string path, std::uint64_t size,
    std::optional<std::uint32_t> permissions, cardio::cancellation cancellation) {
  auto lock = std::move(co_await filesystem->session->operations.lock(cancellation));
  if (size >= UINT32_MAX) throw RemoteFileError("NFS v2 upload size reaches its 32-bit limit", false);
  if (filesystem->uploads.contains(path)) throw RemoteFileError("NFS v2 upload path is already in use", false);
  auto parent = co_await nfs2_parent(filesystem, path, cancellation);
  auto upload = std::make_shared<Nfs2Upload>();
  upload->parent = parent.handle;
  upload->path = path;
  upload->name = std::move(parent.name);
  upload->expected_size = static_cast<std::uint32_t>(size);
  upload->permissions = permissions.value_or(0600);
  filesystem->uploads[path] = upload;
  auto writer = std::make_unique<Nfs2Writer>();
  writer->filesystem = filesystem;
  writer->upload = upload;
  co_return writer;
}

cardio::promise<void> make_nfs2_directory_async(std::shared_ptr<Nfs2Filesystem> filesystem,
    std::string path, std::optional<std::uint32_t> permissions, cardio::cancellation cancellation) {
  auto lock = std::move(co_await filesystem->session->operations.lock(cancellation));
  auto parent = co_await nfs2_parent(filesystem, path, cancellation);
  MKDIR2args args{};
  std::memcpy(args.where.dir, parent.handle.data(), parent.handle.size());
  args.where.name = parent.name.data();
  args.attributes = nfs2_unchanged_attributes();
  args.attributes.mode = permissions.value_or(0755) & 0777;
  MKDIR2res reply{};
  co_await execute_nfs_raw_async(filesystem->session, [&](auto cb, auto data) {
    return ELDER_NFS_RAW_CALL(rpc_nfs2_mkdir, filesystem->session->rpc, cb, &args, data);
  }, [&](void *data) { std::memcpy(&reply, data, sizeof(reply)); }, cancellation);
  check_nfs2(reply.status, "create directory");
}

cardio::promise<void> remove_nfs2_async(std::shared_ptr<Nfs2Filesystem> filesystem,
    std::string path, bool directory, cardio::cancellation cancellation) {
  auto lock = std::move(co_await filesystem->session->operations.lock(cancellation));
  require_nfs2(filesystem);
  if (const auto found = filesystem->uploads.find(path); found != filesystem->uploads.end()) {
    auto upload = found->second;
    co_await nfs2_clean_upload(filesystem, upload, cancellation);
    co_return;
  }
  const auto parent = co_await nfs2_parent(filesystem, path, cancellation);
  co_await nfs2_remove_child(filesystem, parent.handle, parent.name, directory, cancellation);
}

cardio::promise<void> rename_nfs2_async(std::shared_ptr<Nfs2Filesystem> filesystem,
    std::string source, std::string destination, bool overwrite, bool upload, cardio::cancellation cancellation) {
  auto lock = std::move(co_await filesystem->session->operations.lock(cancellation));
  auto to = co_await nfs2_parent(filesystem, destination, cancellation);
  std::shared_ptr<Nfs2Upload> state;
  Nfs2Parent from{};
  if (upload) {
    const auto found = filesystem->uploads.find(source);
    if (found == filesystem->uploads.end()) throw RemoteFileError("NFS v2 upload ownership is unavailable", false);
    state = found->second;
    if (state->creation != RemoteFileCreationState::created || !state->ready || !state->closed ||
        state->offset != state->expected_size) throw RemoteFileError("NFS v2 upload is incomplete", false);
    from = {state->directory, "payload"};
  } else {
    from = co_await nfs2_parent(filesystem, source, cancellation);
    if (!overwrite && (co_await nfs2_lookup_child(filesystem, to.handle, to.name, cancellation)))
      throw RemoteFileError("NFS v2 rename destination already exists", false);
  }
  nfsstat3 status = NFS3_OK;
  if (upload && !overwrite) {
    LINK2args args{};
    std::memcpy(args.from, state->file.data(), state->file.size());
    std::memcpy(args.to.dir, to.handle.data(), to.handle.size());
    args.to.name = to.name.data();
    co_await execute_nfs_raw_async(filesystem->session, [&](auto cb, auto data) {
      return ELDER_NFS_RAW_CALL(rpc_nfs2_link, filesystem->session->rpc, cb, &args, data);
    }, [&](void *data) { std::memcpy(&status, data, sizeof(status)); }, cancellation);
  } else {
    RENAME2args args{};
    std::memcpy(args.from.dir, from.handle.data(), from.handle.size());
    args.from.name = from.name.data();
    std::memcpy(args.to.dir, to.handle.data(), to.handle.size());
    args.to.name = to.name.data();
    co_await execute_nfs_raw_async(filesystem->session, [&](auto cb, auto data) {
      return ELDER_NFS_RAW_CALL(rpc_nfs2_rename, filesystem->session->rpc, cb, &args, data);
    }, [&](void *data) { std::memcpy(&status, data, sizeof(status)); }, cancellation);
  }
  check_nfs2(status, upload ? "commit upload" : "rename");
  if (state) co_await nfs2_clean_upload(filesystem, state, cancellation);
}

cardio::promise<void> make_nfs2_link_async(std::shared_ptr<Nfs2Filesystem> filesystem,
    std::string target, std::string path, cardio::cancellation cancellation) {
  auto lock = std::move(co_await filesystem->session->operations.lock(cancellation));
  if (target.empty() || target.size() > MAXPATHLEN2 || target.find('\0') != std::string::npos)
    throw RemoteFileError("Invalid NFS v2 link target", false);
  auto parent = co_await nfs2_parent(filesystem, path, cancellation);
  SYMLINK2args args{};
  std::memcpy(args.from.dir, parent.handle.data(), parent.handle.size());
  args.from.name = parent.name.data();
  args.to = target.data();
  args.attributes = nfs2_unchanged_attributes();
  args.attributes.mode = 0777;
  SYMLINK2res reply{};
  co_await execute_nfs_raw_async(filesystem->session, [&](auto cb, auto data) {
    return ELDER_NFS_RAW_CALL(rpc_nfs2_symlink, filesystem->session->rpc, cb, &args, data);
  }, [&](void *data) { std::memcpy(&reply, data, sizeof(reply)); }, cancellation);
  check_nfs2(reply.status, "create link");
}

cardio::promise<void> set_nfs2_attributes_async(std::shared_ptr<Nfs2Filesystem> filesystem,
    std::string path, RemoteFileAttributes attributes, cardio::cancellation cancellation) {
  auto lock = std::move(co_await filesystem->session->operations.lock(cancellation));
  require_nfs2(filesystem);
  auto value = nfs2_unchanged_attributes();
  if (attributes.permissions && attributes.type != RemoteFileType::symbolic_link) value.mode = *attributes.permissions & 0777;
  const auto set_time = [](nfstime3 &field, const std::optional<std::int64_t> &seconds) {
    if (!seconds) return;
    if (*seconds < 0 || static_cast<std::uint64_t>(*seconds) >= UINT32_MAX)
      throw RemoteFileError("NFS v2 timestamp exceeds its 32-bit range", false);
    field = {static_cast<std::uint32_t>(*seconds), 0};
  };
  set_time(value.atime, attributes.access_time_unix_seconds);
  set_time(value.mtime, attributes.modification_time_unix_seconds);
  std::array<char, FHSIZE> handle;
  if (const auto found = filesystem->uploads.find(path); found != filesystem->uploads.end()) {
    if (found->second->creation != RemoteFileCreationState::created || !found->second->ready)
      throw RemoteFileError("NFS v2 upload attributes require proven ownership", false);
    handle = found->second->file;
  } else {
    const auto item = co_await nfs2_resolve(filesystem, path, cancellation);
    if (!item) throw RemoteFileError("NFS v2 item disappeared before updating attributes", false);
    handle = item->handle;
  }
  co_await nfs2_set_attributes(filesystem, handle, value, cancellation);
}

} // namespace elder_terms
