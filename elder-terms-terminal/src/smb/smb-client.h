// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once

#include "../file-transfer/remote-file-client.h"
#include <elder-terms/settings/smb-settings.h>

namespace elder_terms {

/** Endpoint and credentials for one direct SMB2/3 connection. */
struct SmbClientOpenOptions {
  /** Validated settings, including the runtime user name. */
  SmbConnectionSettings connection;
  /** Runtime password; never persisted in a settings store. */
  std::string password;
  /** Called once when an established session ends; runs on its dispatcher. */
  std::function<void()> disconnected = {};
};

/**
 * Connects to one SMB share using the caller's cardio/GIO dispatcher.
 * @param options Endpoint, security requirements and runtime credentials.
 * @param cancellation Cancellation covering DNS and connection setup.
 * @returns Remote client whose paths are rooted at the configured share.
 */
cardio::promise<std::shared_ptr<RemoteFileClient>> open_smb_client_async(
    SmbClientOpenOptions options, cardio::cancellation cancellation);

/**
 * Cancels pending SMB operations and retires callbacks before shutdown.
 * @param client Client returned by open_smb_client_async; null is ignored.
 */
cardio::promise<void> stop_smb_client_async(std::shared_ptr<RemoteFileClient> client);

} // namespace elder_terms
