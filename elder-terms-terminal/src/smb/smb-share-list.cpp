// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/
#include "smb-share-list.h"
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#include <smb2/libsmb2-share-enum.h>
#include <algorithm>
#include <set>

namespace elder_terms {

RemoteDirectorySnapshot decode_smb_share_list(int status, const smb2_share_enum_reply *reply) {
  if (status == 234 || (status == 0 && reply && reply->entries_read != reply->total_entries))
    throw RemoteFileError("SMB share listing is incomplete. Enter a known /share/path directly.", false);
  if (status != 0) {
    const bool lost = status == -ECONNRESET || status == -ENOTCONN || status == -EPIPE || status == -ETIMEDOUT;
    throw RemoteFileError("SMB share listing failed (status " + std::to_string(status) +
        "). Enter a known /share/path directly.", lost);
  }
  if (!reply || reply->level != SMB2_SHARE_INFO_1 || (reply->entries_read && !reply->share_info.info_1))
    throw RemoteFileError("Invalid SMB share listing response", false);
  RemoteDirectorySnapshot result{"/", {}};
  std::set<std::string> names;
  for (uint32_t index = 0; index < reply->entries_read; ++index) {
    const auto &entry = reply->share_info.info_1[index];
    if ((entry.type & 3U) != SMB2_SHARE_TYPE_DISKTREE) continue;
    if (!entry.netname || !g_utf8_validate(entry.netname, -1, nullptr))
      throw RemoteFileError("Invalid SMB share name", false);
    const std::string name(entry.netname);
    if (name.empty() || name == "." || name == ".." || name.find_first_of("/\\:\r\n") != std::string::npos ||
        std::any_of(name.begin(), name.end(), [](unsigned char byte) { return byte < 32 || byte == 127; }) ||
        name.back() == '.' || name.back() == ' ')
      throw RemoteFileError("Unsupported SMB share name", false);
    auto *folded = g_utf8_casefold(name.c_str(), -1);
    const std::string key(folded);
    g_free(folded);
    if (!names.insert(key).second) throw RemoteFileError("Duplicate SMB share name", false);
    result.entries.push_back({.name = name, .path = "/" + name, .type = RemoteFileType::directory,
        .size = {}, .permissions = {}, .access_time_unix_seconds = {}, .modification_time_unix_seconds = {}});
  }
  return result;
}

} // namespace elder_terms
