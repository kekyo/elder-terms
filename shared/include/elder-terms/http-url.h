// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once

#include <string>
#include <elder-terms/export.h>
namespace elder_terms {
/**
 * Resolves and normalizes an HTTP reference, preserving encoded URL bytes.
 * @param page Absolute resolution base, or empty for an absolute reference.
 * @param reference Absolute or relative URL without embedded credentials.
 * @returns Canonical URL without a fragment.
 * @throws std::invalid_argument For unsupported or ambiguous URLs.
 */
ELDER_TERMS_API std::string http_resolve_url(const std::string &page, const std::string &reference);

/**
 * Validates a directory URL defining the permitted origin and path boundary.
 * @param url Absolute URL ending in a slash, without query or credentials.
 * @returns Canonical scope URL.
 */
ELDER_TERMS_API std::string http_scope_url(const std::string &url);

/**
 * Checks a canonical URL against the configured origin and directory boundary.
 * @param scope Canonical directory URL.
 * @param url Canonical resource URL.
 * @returns True only inside the configured scope.
 */
ELDER_TERMS_API bool http_url_in_scope(const std::string &scope, const std::string &url);

/**
 * Produces a safe filename candidate without changing the resource URL.
 * @param url Canonical resource URL.
 * @returns UTF-8 name or an encoded name when decoding is not possible.
 */
ELDER_TERMS_API std::string http_resource_name(const std::string &url);

}
