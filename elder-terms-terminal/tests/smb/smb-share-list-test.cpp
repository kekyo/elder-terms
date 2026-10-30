// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/
#include "../../src/smb/smb-share-list.h"
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#include <smb2/libsmb2-share-enum.h>
#include <iostream>

static void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
int main() {
  using namespace elder_terms;
  try {
    char public_name[] = "Public", hidden_name[] = "Admin$", unicode_name[] = "日本語 space";
    char ipc[] = "IPC$", printer[] = "Printer", remark[] = "description";
    smb2_share_info_1 entries[]{{public_name, 0, remark}, {hidden_name, SMB2_SHARE_TYPE_HIDDEN, remark},
      {unicode_name, 0, remark}, {ipc, SMB2_SHARE_TYPE_IPC, remark}, {printer, SMB2_SHARE_TYPE_PRINTQ, remark}};
    smb2_share_enum_reply reply{};
    reply.level = 1; reply.entries_read = reply.total_entries = 5; reply.share_info.info_1 = entries;
    const auto list = decode_smb_share_list(0, &reply);
    require(list.canonical_path == "/" && list.entries.size() == 3, "Only disk shares belong in the server root");
    require(list.entries[0].path == "/Public" && list.entries[1].path == "/Admin$" && list.entries[2].path == "/日本語 space", "Share paths and UTF-8 names differ");
    require(!list.entries[0].size && !list.entries[0].modification_time_unix_seconds, "Share metadata must remain unknown");
    for (const int status : {5, 234, -EACCES, -ECONNRESET}) {
      bool rejected = false;
      try { (void)decode_smb_share_list(status, &reply); }
      catch (const RemoteFileError &error) { rejected = error.connection_lost == (status == -ECONNRESET); }
      require(rejected, "Windows and errno failures must not become successful or empty listings");
    }
    reply.total_entries = 6;
    bool partial = false;
    try { (void)decode_smb_share_list(0, &reply); } catch (const RemoteFileError &error) { partial = !error.connection_lost; }
    require(partial, "A partial success reply must not masquerade as a complete listing");
    reply.total_entries = 5;
    char invalid[] = "../outside";
    entries[0].netname = invalid;
    bool escaped = false;
    try { (void)decode_smb_share_list(0, &reply); } catch (const RemoteFileError &) { escaped = true; }
    require(escaped, "Invalid share names must not become browsable paths");
    reply.entries_read = reply.total_entries = 0; reply.share_info.info_1 = nullptr;
    require(decode_smb_share_list(0, &reply).entries.empty(), "An explicitly empty server must produce an empty listing");
    bool absent = false;
    try { (void)decode_smb_share_list(0, nullptr); } catch (const RemoteFileError &) { absent = true; }
    require(absent, "Missing replies must not become empty listings");
    return 0;
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
