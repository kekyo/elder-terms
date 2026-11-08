// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "../../src/file-transfer/remote-file-client.h"
#include "../../src/file-transfer/file-transfer-node-icon.h"

#include <iostream>
#include <optional>
#include <string>

namespace {

auto expect(bool condition, const char *message) -> bool {
  if (!condition) {
    std::cerr << "file-transfer-contract-test: FAIL: " << message << '\n';
  }
  return condition;
}

} // namespace

auto main() -> int {
  using namespace elder_terms;
  for (const auto &[name, icon] : {
           std::pair{"README", "editor/insert_drive_file"},
           {"notes.TXT", "action/description"}, {"manual.PDF", "image/picture_as_pdf"},
           {"code.TS", "action/code"}, {"image.PNG", "image/image"},
           {"sound.WAV", "image/audiotrack"}, {"video.MP4", "av/movie"},
           {"backup.TAR.GZ", "content/archive"}, {"unknown.xyz", "editor/insert_drive_file"}}) {
    RemoteFileAttributes item;
    item.name = name;
    item.type = RemoteFileType::regular;
    if (!expect(file_transfer_node_icon(item) == std::string(icon),
                "File classification must recognize case-insensitive and compound extensions")) return 1;
  }
  RemoteFileAttributes folder;
  folder.name = "misleading.pdf";
  folder.type = RemoteFileType::directory;
  if (!expect(file_transfer_node_icon(folder) == std::string("file/folder"), "Directories ignore extensions")) return 1;
  folder.role = RemoteFileRole::smb_share;
  if (!expect(file_transfer_node_icon(folder) == std::string("file/folder_shared"), "Shares have a distinct directory icon")) return 1;
  folder.role = RemoteFileRole::nfs_export;
  if (!expect(file_transfer_node_icon(folder) == std::string("device/storage"), "Exports have a distinct directory icon")) return 1;
  folder.type = RemoteFileType::symbolic_link;
  if (!expect(file_transfer_node_icon(folder) == std::string("content/link"), "Links show their own type without following targets")) return 1;
  const elder_terms::RemoteFileCapabilities sftp{
      .symbolic_links = true,
      .permissions = true,
      .access_time = true,
      .modification_time = true,
  };
  const elder_terms::RemoteFileCapabilities ftp{
      .symbolic_links = false,
      .permissions = false,
      .access_time = false,
      .modification_time = true,
  };
  const elder_terms::RemoteFileAttributes entry{
      .name = "payload.bin",
      .path = "/remote/payload.bin",
      .type = elder_terms::RemoteFileType::regular,
      .size = 42,
      .permissions = std::nullopt,
      .access_time_unix_seconds = std::nullopt,
      .modification_time_unix_seconds = 1'725'177'600,
  };

  if (!expect(sftp.symbolic_links && sftp.permissions && sftp.access_time &&
                  sftp.modification_time,
              "SFTP capabilities should expose portable metadata") ||
      !expect(!ftp.symbolic_links && !ftp.permissions && !ftp.access_time &&
                  ftp.modification_time,
              "FTP capabilities should describe unsupported metadata") ||
      !expect(entry.type == elder_terms::RemoteFileType::regular &&
                  entry.size == 42 && !entry.permissions.has_value() &&
                  !entry.access_time_unix_seconds.has_value() &&
                  entry.modification_time_unix_seconds == 1'725'177'600,
              "remote entries should preserve optional metadata")) {
    return 1;
  }

  std::cout << "file-transfer-contract-test: PASS\n";
  return 0;
}
