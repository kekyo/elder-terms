// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/
#pragma once
#include "curl-http-session.h"

namespace elder_terms {
/** Per-request URL policy, separate from HTTP transport and streaming. */
struct HttpReadOptions {
  /** Absolute validated initial URL. */
  std::string url;
  /** Maximum redirect hops. */
  std::size_t redirect_limit = 5;
  /** Resolves and validates each redirect before any connection is attempted. */
  std::function<std::string(const std::string &, const std::string &)> redirect;
  /** Observes successful headers before any body is consumed. */
  std::function<void(const std::string &, const CurlHttpResult &)> response;
};
/**
 * Opens a bounded GET stream and returns when successful headers arrive.
 * @param session Transport retained until completion or cancellation.
 * @param options Request URL and endpoint-specific redirect policy.
 * @param cancellation Cancellation for the complete response lifetime.
 * @returns Reader whose early close or destruction cancels pending HTTP work.
 */
cardio::promise<std::unique_ptr<RemoteFileReader>> open_http_reader_async(
    std::shared_ptr<CurlHttpSession> session, HttpReadOptions options,
    cardio::cancellation cancellation);
}
