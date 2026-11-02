// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once

#include "remote-file-client.h"
#include <string_view>

namespace elder_terms {

/**
 * Chooses a bundled Material glyph without reading content or following links.
 * @param item Local or remote attributes, including a service-supplied role.
 * @returns Material preset identifier; unknown items use the generic file glyph.
 */
inline std::string_view file_transfer_node_icon(const RemoteFileAttributes &item) {
  if (item.type == RemoteFileType::symbolic_link) return "content/link";
  if (item.type == RemoteFileType::directory) {
    if (item.role == RemoteFileRole::smb_share) return "file/folder_shared";
    if (item.role == RemoteFileRole::nfs_export) return "device/storage";
    return "file/folder";
  }
  if (item.type != RemoteFileType::regular) return "editor/insert_drive_file";
  struct Suffix { std::string_view extension; std::string_view icon; };
  static constexpr Suffix suffixes[] = {
      {".txt", "action/description"}, {".md", "action/description"},
      {".rst", "action/description"}, {".doc", "action/description"},
      {".docx", "action/description"}, {".pdf", "image/picture_as_pdf"},
      {".c", "action/code"}, {".h", "action/code"}, {".cpp", "action/code"},
      {".hpp", "action/code"}, {".ts", "action/code"}, {".tsx", "action/code"},
      {".js", "action/code"}, {".jsx", "action/code"}, {".json", "action/code"},
      {".py", "action/code"}, {".rs", "action/code"}, {".sh", "action/code"},
      {".html", "action/code"}, {".css", "action/code"}, {".xml", "action/code"},
      {".png", "image/image"}, {".jpg", "image/image"}, {".jpeg", "image/image"},
      {".gif", "image/image"}, {".svg", "image/image"}, {".webp", "image/image"},
      {".bmp", "image/image"}, {".mp3", "image/audiotrack"},
      {".wav", "image/audiotrack"}, {".flac", "image/audiotrack"},
      {".ogg", "image/audiotrack"}, {".mp4", "av/movie"}, {".mkv", "av/movie"},
      {".mov", "av/movie"}, {".avi", "av/movie"}, {".webm", "av/movie"},
      {".zip", "content/archive"}, {".tar", "content/archive"},
      {".tar.gz", "content/archive"}, {".tar.bz2", "content/archive"},
      {".tar.xz", "content/archive"}, {".gz", "content/archive"},
      {".7z", "content/archive"}, {".rar", "content/archive"},
  };
  std::string name = item.name;
  for (char &ch : name) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
  std::string_view icon = "editor/insert_drive_file";
  std::size_t matched = 0;
  for (const auto &suffix : suffixes) {
    if (suffix.extension.size() > matched && name.ends_with(suffix.extension)) {
      icon = suffix.icon;
      matched = suffix.extension.size();
    }
  }
  return icon;
}

} // namespace elder_terms
