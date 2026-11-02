// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace elder_terms {

/** A filename's UTF-8 presentation, never used as its remote identity. */
struct NfsDisplayName {
  /** Converted UTF-8 name, or a diagnostic containing the original hex bytes. */
  std::string text;
  /** True when the original bytes could be decoded without replacement. */
  bool decoded;
};

/**
 * Checks both conversion directions without accepting replacement modifiers.
 * @param encoding Explicit filename encoding, independent of the locale.
 * @throws std::runtime_error When the encoding is unavailable or permissive.
 */
void validate_nfs_filename_encoding(const std::string &encoding);

/**
 * Converts a copied server name for display without losing undecodable entries.
 * @param bytes Original server bytes.
 * @param encoding Immutable connection encoding.
 * @returns UTF-8 presentation and whether it represents a decoded name.
 */
NfsDisplayName display_nfs_name(std::string_view bytes, const std::string &encoding);

/**
 * Strictly converts a newly entered filename and validates the wire component.
 * @param name UTF-8 input, without normalization or trimming.
 * @param encoding Immutable connection encoding.
 * @param maximum_bytes Protocol or server component limit.
 * @returns Encoded name; never a lossy approximation.
 * @throws std::runtime_error Before any I/O if the input cannot round-trip.
 */
std::string encode_nfs_name(std::string_view name, const std::string &encoding,
                          std::size_t maximum_bytes);

/**
 * Encodes bytes as one ASCII identifier component, independently of URLs.
 * @param bytes Original name or absolute export path.
 * @returns Collision-free percent representation, including every slash.
 */
std::string nfs_identifier_component(std::string_view bytes);

/**
 * Restores the bytes of one canonical identifier component.
 * @param identifier Component produced by nfs_identifier_component().
 * @returns Original byte string, without URL query or fragment interpretation.
 * @throws std::runtime_error For malformed or noncanonical identifiers.
 */
std::string nfs_identifier_bytes(std::string_view identifier);

} // namespace elder_terms
