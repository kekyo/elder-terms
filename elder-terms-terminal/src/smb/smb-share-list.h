// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/
#pragma once
#include "../file-transfer/remote-file-client.h"
struct smb2_share_enum_reply;
namespace elder_terms {
/**
 * Copies a complete level-1 share reply into a virtual directory snapshot.
 * @param status libsmb2 callback status, including positive Windows errors.
 * @param reply Borrowed callback data; ownership remains with the caller.
 * @returns Disk shares only, with absolute server paths and no fabricated metadata.
 * @throws RemoteFileError for an error, incomplete reply, or invalid share name.
 */
RemoteDirectorySnapshot decode_smb_share_list(int status, const smb2_share_enum_reply *reply);
}
