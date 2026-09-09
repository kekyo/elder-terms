#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace elder_terms {

/**
 * Parses and unescapes the pathname encoded by an RFC 959 PWD reply.
 *
 * @param line Completed PWD response line with code 257, without CRLF.
 * @returns Remote working-directory pathname.
 */
std::string parse_ftp_pwd_path(std::string_view line);

/** Type of an entry parsed from an FTP directory listing. */
enum class FtpDirectoryEntryType {
  /** Regular file. */
  regular,
  /** Directory entry. */
  directory,
  /** MLSD pseudo entry describing the listed directory itself. */
  current_directory,
  /** MLSD pseudo entry describing the parent directory. */
  parent_directory,
  /** Other or unsupported item type. */
  other,
};

/** Portable facts parsed from one FTP directory-listing line. */
struct FtpDirectoryEntry {
  /** Entry name as returned by the server. */
  std::string name;
  /** Parsed entry kind. */
  FtpDirectoryEntryType type = FtpDirectoryEntryType::other;
  /** File size, or zero when absent. */
  std::uint64_t size = 0;
  /** RFC 3659 UTC modification timestamp, when valid. */
  std::optional<std::int64_t> modification_time_unix_seconds;
};

/**
 * Parses one RFC 3659 MLSD data line.
 *
 * @param line Listing line without its line terminator.
 * @returns Parsed entry, or no value for a malformed line.
 */
std::optional<FtpDirectoryEntry>
parse_ftp_mlsd_entry(std::string_view line);

/**
 * Parses one common UNIX or DOS LIST data line.
 *
 * @param line Listing line without its line terminator.
 * @returns Parsed entry, or no value for an unknown format.
 *
 * @remarks LIST is not machine-readable in RFC 959. This parser is only a
 * compatibility fallback when MLSD is unavailable.
 */
std::optional<FtpDirectoryEntry>
parse_ftp_list_entry(std::string_view line);

/**
 * Checks whether text can be embedded as one FTP command argument.
 *
 * @param value Candidate username, password, or pathname.
 * @returns False when the value contains NUL or a control character.
 */
bool ftp_command_argument_is_safe(std::string_view value);

} // namespace elder_terms
