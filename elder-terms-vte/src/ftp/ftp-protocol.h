#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace elder_terms {

/** One complete FTP control reply. */
struct FtpReply {
  /** Three-digit FTP reply code. */
  int code = 0;
  /** Reply lines without CRLF terminators. */
  std::vector<std::string> lines;
};

/** Incremental parser for RFC 959 single-line and multiline replies. */
class FtpReplyParser {
private:
  std::string pending_bytes;
  std::optional<int> multiline_code;
  std::vector<std::string> reply_lines;
  std::size_t reply_size = 0;
  std::deque<FtpReply> ready_replies;

  void consume_line(std::string line);

public:
  /**
   * Consumes a fragment received from the FTP control connection.
   *
   * @param bytes Raw control-connection bytes.
   * @throws std::runtime_error if a reply is malformed or exceeds limits.
   */
  void feed(std::string_view bytes);

  /**
   * Reports end of the control stream.
   *
   * @throws std::runtime_error if a partial reply remains.
   */
  void finish() const;

  /**
   * Removes the oldest complete reply.
   *
   * @returns Oldest reply, or no value if another line is required.
   */
  std::optional<FtpReply> take_reply();
};

/** IPv4 endpoint encoded by a PASV reply. */
struct FtpPassiveEndpoint {
  /** Dotted-decimal IPv4 address. */
  std::string address;
  /** TCP data port. */
  std::uint16_t port = 0;
};

/**
 * Parses the port encoded by an RFC 2428 EPSV reply.
 *
 * @param reply FTP reply with code 229.
 * @returns Valid TCP port.
 */
std::uint16_t parse_ftp_epsv_port(const FtpReply &reply);

/**
 * Parses the IPv4 endpoint encoded by an RFC 959 PASV reply.
 *
 * @param reply FTP reply with code 227.
 * @returns Encoded IPv4 address and TCP port.
 */
FtpPassiveEndpoint parse_ftp_pasv_endpoint(const FtpReply &reply);

/**
 * Parses and unescapes the pathname encoded by an RFC 959 PWD reply.
 *
 * @param reply FTP reply with code 257.
 * @returns Remote working-directory pathname.
 */
std::string parse_ftp_pwd_path(const FtpReply &reply);

/** Type of an entry parsed from an FTP directory listing. */
enum class FtpDirectoryEntryType {
  regular,
  directory,
  current_directory,
  parent_directory,
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
