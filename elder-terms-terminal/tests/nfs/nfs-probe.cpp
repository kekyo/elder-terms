// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "../../src/nfs/nfs-rpc.h"
#include "../../src/nfs/nfs-api.h"
#include "../../src/nfs/nfs-name.h"
#include <nfsc/libnfs-raw-mount.h>
#include <nfsc/libnfs-raw-nfs.h>
#include <glib-unix.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <map>
#include <set>

using namespace elder_terms;

static void require(bool value, const std::string &message) {
  if (!value) throw std::runtime_error(message);
}

struct Probe {
  std::string host;
  std::string export_path;
  std::string encoding;
  int version;
  int nfs_port;
  int mount_port;
  std::map<std::string, std::string> contents;
};

static void record_file(Probe &probe, const std::string &raw, std::string content) {
  const auto shown = display_nfs_name(raw, probe.encoding);
  std::cout << "name=" << nfs_identifier_component(raw) << " display=" << shown.text
            << " decoded=" << shown.decoded << " bytes=" << content.size() << '\n';
  probe.contents[raw] = std::move(content);
}

static cardio::promise<void> probe_exports(Probe &probe) {
  auto mount = create_nfs_raw_session(10000);
  const int mount_version = probe.version == 2 ? MOUNT_V1 : MOUNT_V3;
  co_await connect_nfs_raw_async(mount, probe.host, probe.mount_port, MOUNT_PROGRAM, mount_version, {});
  std::vector<std::string> paths;
  co_await execute_nfs_raw_async(mount, [&](auto cb, auto data) {
    return mount_version == MOUNT_V1 ? ELDER_NFS_RAW_CALL(rpc_mount1_export, mount->rpc, cb, data)
        : ELDER_NFS_RAW_CALL(rpc_mount3_export, mount->rpc, cb, data);
  }, [&](void *data) {
    for (auto *node = *static_cast<exports *>(data); node;) {
      exportnode entry{};
      std::memcpy(&entry, node, sizeof(entry));
      paths.emplace_back(entry.ex_dir);
      node = entry.ex_next;
    }
  }, {});
  require(std::find(paths.begin(), paths.end(), probe.export_path) != paths.end(), "MOUNT exports must include the original export path");
  std::cout << "exports=" << paths.size() << " mount-version=" << mount_version << '\n';
}

static cardio::promise<void> probe_high(Probe &probe) {
  auto session = create_nfs_rpc_session(probe.version, 10000);
  configure_nfs_ports(session, probe.nfs_port, probe.mount_port);
  check_nfs_result(co_await execute_nfs_async(session, [&](auto cb, auto data) {
    return nfs_mount_async(session->nfs, probe.host.c_str(), probe.export_path.c_str(), cb, data);
  }, {}, {}), "mount");
  require(nfs_get_version(session->nfs) == probe.version, "NFS version must not silently change");
  std::vector<std::string> names;
  check_nfs_result(co_await execute_nfs_async(session, [&](auto cb, auto data) {
    return nfs_opendir_async(session->nfs, "/", cb, data);
  }, [&](int, void *data) {
    auto *directory = static_cast<nfsdir *>(data);
    while (auto *entry = nfs_readdir(session->nfs, directory))
      if (entry->type == NF3REG) names.emplace_back(entry->name);
    nfs_closedir(session->nfs, directory);
  }, {}), "list");
  for (const auto &name : names) {
    nfsfh *handle = nullptr;
    const auto path = "/" + name;
    check_nfs_result(co_await execute_nfs_async(session, [&](auto cb, auto data) {
      return nfs_open_async(session->nfs, path.c_str(), O_RDONLY, cb, data);
    }, [&](int, void *data) { handle = static_cast<nfsfh *>(data); }, {}), "open");
    std::string content;
    std::array<char, 3> buffer{};
    for (;;) {
      const auto count = co_await read_nfs_async(session, handle, std::as_writable_bytes(std::span(buffer)), content.size(), {});
      if (count == 0) break;
      content.append(buffer.data(), count);
    }
    check_nfs_result(co_await execute_nfs_async(session, [&](auto cb, auto data) {
      return nfs_close_async(session->nfs, handle, cb, data);
    }, {}, {}), "close");
    record_file(probe, name, std::move(content));
  }
  check_nfs_result(co_await execute_nfs_async(session, [&](auto cb, auto data) {
    return nfs_umount_async(session->nfs, cb, data);
  }, {}, {}), "unmount");
}

static void check_v2(nfsstat3 status) {
  if (status != NFS3_OK) throw RemoteFileError(std::string("NFS v2: ") + nfsstat3_to_str(status), status == NFS3ERR_STALE);
}

static cardio::promise<void> probe_v2(Probe &probe) {
  auto mount = create_nfs_raw_session(10000);
  co_await connect_nfs_raw_async(mount, probe.host, probe.mount_port, MOUNT_PROGRAM, MOUNT_V1, {});
  std::array<char, FHSIZE> root{};
  co_await execute_nfs_raw_async(mount, [&](auto cb, auto data) {
    return ELDER_NFS_RAW_CALL(rpc_mount1_mnt, mount->rpc, cb, probe.export_path.data(), data);
  }, [&](void *data) {
    const auto &reply = *static_cast<mountres1 *>(data);
    require(reply.fhs_status == MNT1_OK, "NFS v2 MOUNT v1 must succeed");
    std::memcpy(root.data(), reply.mountres1_u.mountinfo.fhandle, root.size());
  }, {});
  auto session = create_nfs_raw_session(10000);
  co_await connect_nfs_raw_async(session, probe.host, probe.nfs_port, NFS_PROGRAM, NFS_V2, {});
  READDIR2args listing{};
  std::memcpy(listing.dir, root.data(), root.size());
  listing.count = 128; // Exercise multiple READDIR pages on the fixture.
  std::vector<std::string> names;
  std::set<std::string> cookies;
  bool eof = false;
  while (!eof) {
    co_await execute_nfs_raw_async(session, [&](auto cb, auto data) {
      return ELDER_NFS_RAW_CALL(rpc_nfs2_readdir, session->rpc, cb, &listing, data);
    }, [&](void *data) {
      const auto &reply = *static_cast<READDIR2res *>(data);
      check_v2(reply.status);
      const auto &page = reply.READDIR2res_u.resok;
      eof = page.eof;
      for (auto *node = page.entries; node;) {
        entry2 entry{};
        std::memcpy(&entry, node, sizeof(entry));
        const std::string name(entry.name);
        if (name != "." && name != "..") names.push_back(name);
        std::memcpy(listing.cookie, entry.cookie, sizeof(listing.cookie));
        node = entry.nextentry;
      }
    }, {});
    require(eof || cookies.insert(std::string(listing.cookie, sizeof(listing.cookie))).second,
        "NFS v2 READDIR cookie made no progress");
  }
  for (auto &name : names) {
    LOOKUP2args lookup{};
    std::memcpy(lookup.what.dir, root.data(), root.size());
    lookup.what.name = name.data();
    LOOKUP2resok item{};
    co_await execute_nfs_raw_async(session, [&](auto cb, auto data) {
      return ELDER_NFS_RAW_CALL(rpc_nfs2_lookup, session->rpc, cb, &lookup, data);
    }, [&](void *data) {
      const auto &reply = *static_cast<LOOKUP2res *>(data);
      check_v2(reply.status);
      item = reply.LOOKUP2res_u.resok;
    }, {});
    if (item.attributes.type != NF2REG) continue;
    READ2args read{};
    std::memcpy(read.file, item.file, sizeof(read.file));
    read.count = 3;
    std::string content;
    for (;;) {
      std::size_t count = 0;
      read.offset = content.size();
      co_await execute_nfs_raw_async(session, [&](auto cb, auto data) {
        return ELDER_NFS_RAW_CALL(rpc_nfs2_read, session->rpc, cb, &read, data);
      }, [&](void *data) {
        const auto &reply = *static_cast<READ2res *>(data);
        check_v2(reply.status);
        const auto &bytes = reply.READ2res_u.resok.data;
        count = bytes.nfsdata2_len;
        require(count <= read.count, "Invalid NFS v2 read count");
        if (count) content.append(bytes.nfsdata2_val, count);
      }, {});
      if (count == 0) break;
    }
    record_file(probe, name, std::move(content));
  }
  co_await execute_nfs_raw_async(mount, [&](auto cb, auto data) {
    return ELDER_NFS_RAW_CALL(rpc_mount1_umnt, mount->rpc, cb, probe.export_path.data(), data);
  }, {}, {});
}

static cardio::promise<void> probe_fault(int version, const std::string &host, int port, const std::string &mode) {
  auto session = version == 2 ? create_nfs_raw_session(700) : create_nfs_rpc_session(version, 700);
  const guint signal = g_unix_signal_add(SIGUSR1, +[](void *data) -> gboolean {
    static_cast<NfsRpcSession *>(data)->stop();
    return G_SOURCE_CONTINUE;
  }, session.get());
  bool expected = false;
  try {
    if (version == 2) co_await connect_nfs_raw_async(session, host, port, NFS_PROGRAM, NFS_V2, {});
    else {
      configure_nfs_ports(session, port, port);
      check_nfs_result(co_await execute_nfs_async(session, [&](auto cb, auto data) {
        return nfs_mount_async(session->nfs, host.c_str(), "/", cb, data);
      }, {}, {}), "fault mount");
    }
  } catch (const cardio::canceled_exception &) { expected = mode == "cancel"; }
  catch (const RemoteFileError &error) {
    expected = error.connection_lost && (mode == "disconnect" ||
        (mode == "timeout" && std::string(error.what()).find("timed out") != std::string::npos));
  }
  g_source_remove(signal);
  require(expected && !session->rpc && !session->active,
      "Stopping must retire callbacks, release the context, and preserve the failure reason");
  std::cout << "PASS fault " << version << ' ' << mode << '\n';
}

int main(int argc, char **argv) {
  if (argc != 8 && argc != 6) {
    std::cerr << "Usage: nfs-probe version host export nfs-port mount-port encoding fixture | --fault version host port cancel|timeout|disconnect\n";
    return 2;
  }
  cardio::dispatcher_group_glib group;
  cardio::dispatcher_host_glib dispatcher(group);
  std::exception_ptr failure;
  auto body = [&]() -> cardio::promise<void> {
    try {
      if (std::string(argv[1]) == "--fault") {
        co_await probe_fault(std::stoi(argv[2]), argv[3], std::stoi(argv[4]), argv[5]);
      } else {
        Probe probe{argv[2], argv[3], argv[6], std::stoi(argv[1]), std::stoi(argv[4]), std::stoi(argv[5]), {}};
        if (probe.version != 4) co_await probe_exports(probe);
        if (probe.version == 2) co_await probe_v2(probe);
        else co_await probe_high(probe);
        if (std::string(argv[7]) == "fixture") {
          require(probe.contents.at("plain.txt") == "plain\n" && probe.contents.at("日本語表") == "UTF-8\n" &&
              probe.contents.at("\x93\xfa\x96\x7b\x8c\xea\x95\x5c") == "CP932\n" &&
              probe.contents.at("\xc6\xfc\xcb\xdc\xb8\xec\xc9\xbd") == "EUC-JP\n" &&
              probe.contents.at("\xff") == "raw\n" && probe.contents.at("zero").empty() &&
              probe.contents.at(" ?#% ") == "punctuation\n" && probe.contents.at("が") == "NFC\n" &&
              probe.contents.at("が") == "NFD\n", "Each original name must select its own unmodified content");
        }
        std::cout << "PASS NFS " << probe.version << " " << probe.encoding << " byte-identical lookup/read\n";
      }
    } catch (...) { failure = std::current_exception(); }
    group.shutdown();
  };
  auto task = body();
  dispatcher.park();
  if (failure) {
    try { std::rethrow_exception(failure); }
    catch (const std::exception &error) { std::cerr << error.what() << '\n'; }
    return 1;
  }
  return 0;
}
