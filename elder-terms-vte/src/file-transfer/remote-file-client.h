#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <cardio.h>

namespace elder_terms {

/** Kind of an item exposed by a remote file service. */
enum class RemoteFileType {
  regular,
  directory,
  symbolic_link,
  other,
};

/** Optional remote filesystem features understood by the transfer engine. */
struct RemoteFileCapabilities {
  /** True when symbolic links can be inspected and created. */
  bool symbolic_links = false;
  /** True when POSIX permission bits can be read and applied. */
  bool permissions = false;
  /** True when access timestamps can be read and applied. */
  bool access_time = false;
  /** True when modification timestamps can be read and applied. */
  bool modification_time = false;
};

/** Portable attributes used by the remote browser and transfer engine. */
struct RemoteFileAttributes {
  /** Item name without its parent path. */
  std::string name;
  /** Full remote path. */
  std::string path;
  /** Remote item kind, determined without following symbolic links. */
  RemoteFileType type = RemoteFileType::other;
  /** Regular-file size in bytes. */
  std::uint64_t size = 0;
  /** POSIX permission and type bits, when reported by the service. */
  std::optional<std::uint32_t> permissions;
  /** Last access time as Unix seconds, when reported by the service. */
  std::optional<std::int64_t> access_time_unix_seconds;
  /** Last modification time as Unix seconds, when reported by the service. */
  std::optional<std::int64_t> modification_time_unix_seconds;
};

/** One atomically loaded remote directory and its canonical path. */
struct RemoteDirectorySnapshot {
  /** Canonical path identifying the loaded directory. */
  std::string canonical_path;
  /** Directory entries excluding service-specific self and parent records. */
  std::vector<RemoteFileAttributes> entries;
};

/** Asynchronous reader for one remote regular file. */
class RemoteFileReader {
public:
  /** Releases the remote file handle. */
  virtual ~RemoteFileReader() = default;

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

/** Asynchronous writer for one remote regular file. */
class RemoteFileWriter {
public:
  /** Releases the remote file handle. */
  virtual ~RemoteFileWriter() = default;

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

/** Asynchronous filesystem operations for one authenticated remote service. */
class RemoteFileClient {
public:
  /** Releases the remote service session. */
  virtual ~RemoteFileClient() = default;

  /**
   * Returns immutable filesystem capabilities for this session.
   *
   * @returns Supported optional operations and metadata.
   */
  virtual auto capabilities() const noexcept
      -> RemoteFileCapabilities = 0;

  /**
   * Loads one directory as a single logical remote operation.
   *
   * @param path Remote path accepted by the service.
   * @param cancellation Operation cancellation signal.
   * @returns Canonical path and directory entries.
   */
  virtual cardio::promise<RemoteDirectorySnapshot>
  load_directory_async(std::string path,
                       cardio::cancellation cancellation) = 0;

  /**
   * Reads remote attributes without following a symbolic link.
   *
   * @param path Remote item path.
   * @param cancellation Operation cancellation signal.
   * @returns Attributes, or no value when the item does not exist.
   */
  virtual cardio::promise<std::optional<RemoteFileAttributes>>
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
   * @param permissions Optional POSIX permission bits.
   * @param cancellation Operation cancellation signal.
   */
  virtual cardio::promise<void>
  make_directory_async(std::string path,
                       std::optional<std::uint32_t> permissions,
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
   * Applies metadata supported by the remote service.
   *
   * @param path Remote item path.
   * @param attributes Optional metadata to apply.
   * @param cancellation Operation cancellation signal.
   */
  virtual cardio::promise<void>
  set_attributes_async(std::string path,
                       RemoteFileAttributes attributes,
                       cardio::cancellation cancellation) = 0;

  /**
   * Opens a remote regular file for reading.
   *
   * @param path Remote file path.
   * @param cancellation Operation cancellation signal.
   * @returns Remote file reader.
   */
  virtual cardio::promise<std::unique_ptr<RemoteFileReader>>
  open_read_async(std::string path,
                  cardio::cancellation cancellation) = 0;

  /**
   * Creates or truncates a remote regular file for writing.
   *
   * @param path Remote file path.
   * @param permissions Optional initial POSIX permission bits.
   * @param cancellation Operation cancellation signal.
   * @returns Remote file writer.
   */
  virtual cardio::promise<std::unique_ptr<RemoteFileWriter>>
  open_write_async(std::string path,
                   std::optional<std::uint32_t> permissions,
                   cardio::cancellation cancellation) = 0;

  /**
   * Acquires the service's single bulk-transfer slot.
   *
   * @returns True when the caller acquired the slot.
   */
  virtual bool try_begin_transfer() = 0;

  /** Releases the service's bulk-transfer slot. */
  virtual void end_transfer() = 0;
};

} // namespace elder_terms
