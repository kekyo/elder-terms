#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <cardio.h>

namespace elder_terms {

class AuthenticatedSshTransport;

/**
 * Kind of an item exposed by an SFTP server.
 */
enum class SftpFileType {
  regular,
  directory,
  symbolic_link,
  other,
};

/**
 * Portable attributes used by the SFTP browser and transfer engine.
 */
struct SftpFileAttributes {
  /** Item name without its parent path. */
  std::string name;
  /** Full remote path. */
  std::string path;
  /** Remote item kind, determined without following symbolic links. */
  SftpFileType type = SftpFileType::other;
  /** Regular-file size in bytes. */
  std::uint64_t size = 0;
  /** POSIX permission and type bits reported by the server. */
  std::uint32_t permissions = 0;
  /** Last access time as Unix seconds. */
  std::int64_t access_time_unix_seconds = 0;
  /** Last modification time as Unix seconds. */
  std::int64_t modification_time_unix_seconds = 0;
};

/**
 * Asynchronous reader for one remote regular file.
 */
class SftpFileReader {
public:
  /**
   * Releases the remote file handle.
   */
  virtual ~SftpFileReader() = default;

  /**
   * Reads the next remote file chunk.
   *
   * @param buffer Destination buffer.
   * @param cancellation Operation cancellation signal.
   * @returns Number of bytes read, or zero at end of file.
   */
  virtual cardio::promise<std::size_t>
  read_async(std::span<std::byte> buffer,
             cardio::cancellation cancellation) = 0;

  /**
   * Closes the remote file handle.
   *
   * @param cancellation Operation cancellation signal.
   */
  virtual cardio::promise<void>
  close_async(cardio::cancellation cancellation) = 0;
};

/**
 * Asynchronous writer for one remote regular file.
 */
class SftpFileWriter {
public:
  /**
   * Releases the remote file handle.
   */
  virtual ~SftpFileWriter() = default;

  /**
   * Writes an entire remote file chunk.
   *
   * @param buffer Source buffer.
   * @param cancellation Operation cancellation signal.
   */
  virtual cardio::promise<void>
  write_all_async(std::span<const std::byte> buffer,
                  cardio::cancellation cancellation) = 0;

  /**
   * Closes the remote file handle.
   *
   * @param cancellation Operation cancellation signal.
   */
  virtual cardio::promise<void>
  close_async(cardio::cancellation cancellation) = 0;
};

/**
 * Asynchronous filesystem operations for one authenticated SFTP subsystem.
 */
class SftpClient {
public:
  /**
   * Closes the SFTP subsystem after outstanding file handles are released.
   */
  virtual ~SftpClient() = default;

  /**
   * Canonicalizes a remote path.
   *
   * @param path Remote path accepted by the server.
   * @param cancellation Operation cancellation signal.
   * @returns Canonical remote path.
   */
  virtual cardio::promise<std::string>
  canonicalize_path_async(std::string path,
                          cardio::cancellation cancellation) = 0;

  /**
   * Lists a remote directory without following returned symbolic links.
   *
   * @param path Remote directory path.
   * @param cancellation Operation cancellation signal.
   * @returns Directory entries excluding "." and "..".
   */
  virtual cardio::promise<std::vector<SftpFileAttributes>>
  list_directory_async(std::string path,
                       cardio::cancellation cancellation) = 0;

  /**
   * Reads remote attributes without following a symbolic link.
   *
   * @param path Remote item path.
   * @param cancellation Operation cancellation signal.
   * @returns Attributes, or no value when the item does not exist.
   */
  virtual cardio::promise<std::optional<SftpFileAttributes>>
  lstat_async(std::string path, cardio::cancellation cancellation) = 0;

  /**
   * Reads the stored target of a remote symbolic link.
   *
   * @param path Remote symbolic-link path.
   * @param cancellation Operation cancellation signal.
   * @returns Stored symbolic-link target.
   */
  virtual cardio::promise<std::string>
  read_link_async(std::string path,
                  cardio::cancellation cancellation) = 0;

  /**
   * Creates a remote directory.
   *
   * @param path Remote directory path.
   * @param permissions POSIX permission bits.
   * @param cancellation Operation cancellation signal.
   */
  virtual cardio::promise<void>
  make_directory_async(std::string path, std::uint32_t permissions,
                       cardio::cancellation cancellation) = 0;

  /**
   * Deletes a remote non-directory item.
   *
   * @param path Remote item path.
   * @param cancellation Operation cancellation signal.
   */
  virtual cardio::promise<void>
  remove_file_async(std::string path,
                    cardio::cancellation cancellation) = 0;

  /**
   * Deletes an empty remote directory.
   *
   * @param path Remote directory path.
   * @param cancellation Operation cancellation signal.
   */
  virtual cardio::promise<void>
  remove_directory_async(std::string path,
                         cardio::cancellation cancellation) = 0;

  /**
   * Renames a remote item.
   *
   * @param source_path Existing remote path.
   * @param destination_path New remote path.
   * @param cancellation Operation cancellation signal.
   */
  virtual cardio::promise<void>
  rename_async(std::string source_path, std::string destination_path,
               cardio::cancellation cancellation) = 0;

  /**
   * Creates a remote symbolic link without following its target.
   *
   * @param target Stored symbolic-link target.
   * @param path Remote symbolic-link path.
   * @param cancellation Operation cancellation signal.
   */
  virtual cardio::promise<void>
  make_symbolic_link_async(std::string target, std::string path,
                          cardio::cancellation cancellation) = 0;

  /**
   * Applies portable metadata to a remote item.
   *
   * @param path Remote item path.
   * @param permissions POSIX permission bits.
   * @param access_time_unix_seconds Last access time as Unix seconds.
   * @param modification_time_unix_seconds Last modification time as Unix
   * seconds.
   * @param cancellation Operation cancellation signal.
   */
  virtual cardio::promise<void>
  set_attributes_async(std::string path, std::uint32_t permissions,
                       std::int64_t access_time_unix_seconds,
                       std::int64_t modification_time_unix_seconds,
                       cardio::cancellation cancellation) = 0;

  /**
   * Opens a remote regular file for reading.
   *
   * @param path Remote file path.
   * @param cancellation Operation cancellation signal.
   * @returns Remote file reader.
   */
  virtual cardio::promise<std::unique_ptr<SftpFileReader>>
  open_read_async(std::string path,
                  cardio::cancellation cancellation) = 0;

  /**
   * Creates or truncates a remote regular file for writing.
   *
   * @param path Remote file path.
   * @param permissions Initial POSIX permission bits.
   * @param cancellation Operation cancellation signal.
   * @returns Remote file writer.
   */
  virtual cardio::promise<std::unique_ptr<SftpFileWriter>>
  open_write_async(std::string path, std::uint32_t permissions,
                   cardio::cancellation cancellation) = 0;

  /**
   * Acquires the authenticated transport's single bulk-transfer slot.
   *
   * @returns True when the caller acquired the slot.
   */
  virtual bool try_begin_transfer() = 0;

  /**
   * Releases the authenticated transport's bulk-transfer slot.
   */
  virtual void end_transfer() = 0;
};

/**
 * Opens an SFTP subsystem over an already authenticated SSH transport.
 *
 * @param transport Shared authenticated SSH transport.
 * @param cancellation Operation cancellation signal.
 * @returns Initialized SFTP client.
 */
cardio::promise<std::shared_ptr<SftpClient>>
open_sftp_client_async(
    std::shared_ptr<AuthenticatedSshTransport> transport,
    cardio::cancellation cancellation);

} // namespace elder_terms
