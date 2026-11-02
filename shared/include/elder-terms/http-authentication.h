// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once

namespace elder_terms {
/** HTTP authentication methods permitted for a HTTP connection. */
enum class HttpAuthentication {
  /** Negotiate only Basic or Digest authentication. */
  automatic,
  /** Use HTTP Basic authentication. */
  basic,
  /** Use HTTP Digest authentication. */
  digest,
  /** Send no credentials. */
  none,
};

}
