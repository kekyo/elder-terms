#pragma once

#include <memory>
#include <string>

#include <cardio.h>

namespace elder_terms {

class AuthenticatedSshTransport;

/** Hash values calculated for one regular file. */
struct FileHashes {
  /** Lowercase MD5 digest. */
  std::string md5;
  /** Lowercase SHA-1 digest. */
  std::string sha1;
  /** Lowercase SHA-256 digest. */
  std::string sha256;
};

/**
 * Calculates all supported hashes while reading a local file once.
 *
 * @param path Native local filesystem path.
 * @param cancellation Operation cancellation signal.
 * @returns MD5, SHA-1, and SHA-256 hashes.
 */
cardio::promise<FileHashes>
calculate_local_file_hashes_async(std::string path,
                                  cardio::cancellation cancellation);

/**
 * Calculates all supported hashes by running checksum commands over SSH.
 *
 * @param transport Authenticated transport used to open an exec channel.
 * @param path Remote filesystem path passed safely to the remote shell.
 * @param cancellation Operation cancellation signal.
 * @returns MD5, SHA-1, and SHA-256 hashes returned by the server.
 */
cardio::promise<FileHashes> calculate_ssh_file_hashes_async(
    std::shared_ptr<AuthenticatedSshTransport> transport, std::string path,
    cardio::cancellation cancellation);

} // namespace elder_terms
