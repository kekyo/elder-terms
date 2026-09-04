#include "sftp-client.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <libssh/libssh.h>
#include <libssh/sftp.h>

#include "../terminal-sessions/ssh-session/authenticated-ssh-transport.h"

namespace elder_terms {

struct LibsshSftpSessionState {
  sftp_session session = nullptr;
};

struct LibsshSftpFileState {
  sftp_file file = nullptr;
};

static std::runtime_error
libssh_sftp_failure(ssh_session session, sftp_session sftp,
                    const std::string &operation) {
  const int code = sftp == nullptr ? SSH_ERROR : sftp_get_error(sftp);
  const char *detail =
      session == nullptr ? nullptr : ssh_get_error(session);
  std::string message =
      operation + " (SFTP status " + std::to_string(code) + ")";
  if (detail != nullptr && detail[0] != '\0') {
    message += ": ";
    message += detail;
  }
  return std::runtime_error(message);
}

static RemoteFileType
sftp_file_type(const sftp_attributes attributes) {
  if (attributes == nullptr) {
    return RemoteFileType::other;
  }
  switch (attributes->type) {
  case SSH_FILEXFER_TYPE_REGULAR:
    return RemoteFileType::regular;
  case SSH_FILEXFER_TYPE_DIRECTORY:
    return RemoteFileType::directory;
  case SSH_FILEXFER_TYPE_SYMLINK:
    return RemoteFileType::symbolic_link;
  default:
    break;
  }
  if (S_ISREG(attributes->permissions)) {
    return RemoteFileType::regular;
  }
  if (S_ISDIR(attributes->permissions)) {
    return RemoteFileType::directory;
  }
  if (S_ISLNK(attributes->permissions)) {
    return RemoteFileType::symbolic_link;
  }
  return RemoteFileType::other;
}

static bool valid_remote_child_name(const std::string &name) {
  return !name.empty() && name != "." && name != ".." &&
         name.find('/') == std::string::npos;
}

static std::string sftp_child_path(const std::string &directory,
                                   const std::string &name) {
  if (!valid_remote_child_name(name)) {
    throw std::runtime_error(
        "SFTP server returned an invalid directory entry name");
  }
  if (directory.empty() || directory == ".") {
    return directory.empty() ? name : "./" + name;
  }
  if (directory == "/") {
    return "/" + name;
  }
  return directory.back() == '/' ? directory + name
                                 : directory + "/" + name;
}

static std::string sftp_path_name(const std::string &path) {
  std::string trimmed = path;
  while (trimmed.size() > 1 && trimmed.back() == '/') {
    trimmed.pop_back();
  }
  const std::size_t separator = trimmed.find_last_of('/');
  return separator == std::string::npos
             ? trimmed
             : trimmed.substr(separator + 1);
}

static RemoteFileAttributes
portable_attributes(const sftp_attributes attributes,
                    std::string path, std::string name) {
  return RemoteFileAttributes{
      .name = std::move(name),
      .path = std::move(path),
      .type = sftp_file_type(attributes),
      .size = attributes == nullptr ? 0 : attributes->size,
      .permissions =
          attributes == nullptr
              ? std::optional<std::uint32_t>{}
              : std::optional<std::uint32_t>{attributes->permissions},
      .access_time_unix_seconds =
          attributes == nullptr
              ? std::optional<std::int64_t>{}
              : std::optional<std::int64_t>{
                    static_cast<std::int64_t>(attributes->atime)},
      .modification_time_unix_seconds =
          attributes == nullptr
              ? std::optional<std::int64_t>{}
              : std::optional<std::int64_t>{
                    static_cast<std::int64_t>(attributes->mtime)},
  };
}

static void run_with_blocking_session(
    ssh_session session, const std::function<void()> &operation) {
  const int previous = ssh_is_blocking(session);
  ssh_set_blocking(session, 1);
  try {
    operation();
  } catch (...) {
    ssh_set_blocking(session, previous);
    throw;
  }
  ssh_set_blocking(session, previous);
}

class LibsshSftpClient final
    : public RemoteFileClient,
      public std::enable_shared_from_this<LibsshSftpClient> {
private:
  class Reader;
  class Writer;

  std::shared_ptr<AuthenticatedSshTransport> transport;
  std::shared_ptr<LibsshSftpSessionState> state;

  explicit LibsshSftpClient(
      std::shared_ptr<AuthenticatedSshTransport> transport)
      : transport(std::move(transport)),
        state(std::make_shared<LibsshSftpSessionState>()) {
  }

  cardio::promise<void>
  initialize_async(cardio::cancellation cancellation) {
    const std::shared_ptr<LibsshSftpClient> owner =
        shared_from_this();
    co_await transport->execute_serialized_async(
        [owner](ssh_session session) {
          run_with_blocking_session(session, [owner, session]() {
            sftp_session sftp = sftp_new(session);
            if (sftp == nullptr) {
              throw libssh_sftp_failure(
                  session, nullptr, "Failed to allocate SFTP subsystem");
            }
            if (sftp_init(sftp) != SSH_OK) {
              const std::runtime_error error = libssh_sftp_failure(
                  session, sftp, "Failed to initialize SFTP subsystem");
              sftp_free(sftp);
              throw error;
            }
            owner->state->session = sftp;
          });
        },
        std::move(cancellation));
  }

  cardio::promise<void> run_async(
      std::function<void(ssh_session, sftp_session)> operation,
      cardio::cancellation cancellation) {
    const std::shared_ptr<LibsshSftpClient> owner =
        shared_from_this();
    co_await transport->execute_serialized_async(
        [owner, operation = std::move(operation)](
            ssh_session session) {
          run_with_blocking_session(session, [owner, operation, session]() {
            if (owner->state->session == nullptr) {
              throw std::runtime_error("SFTP subsystem is closed");
            }
            operation(session, owner->state->session);
          });
        },
        std::move(cancellation));
  }

  void close_file_later(
      std::shared_ptr<LibsshSftpFileState> file_state) noexcept {
    const std::shared_ptr<LibsshSftpSessionState> session_state = state;
    (void)transport->enqueue_serialized(
        [session_state, file_state](ssh_session session) {
          if (session_state->session == nullptr ||
              file_state->file == nullptr) {
            return;
          }
          run_with_blocking_session(session, [file_state]() {
            (void)sftp_close(file_state->file);
            file_state->file = nullptr;
          });
        });
  }

public:
  ~LibsshSftpClient() override {
    const std::shared_ptr<LibsshSftpSessionState> session_state = state;
    (void)transport->enqueue_serialized(
        [session_state](ssh_session session) {
          if (session_state->session == nullptr) {
            return;
          }
          run_with_blocking_session(session, [session_state]() {
            sftp_free(session_state->session);
            session_state->session = nullptr;
          });
        });
  }

  LibsshSftpClient(const LibsshSftpClient &) = delete;
  LibsshSftpClient &operator=(const LibsshSftpClient &) = delete;

  static cardio::promise<std::shared_ptr<RemoteFileClient>>
  open_async(std::shared_ptr<AuthenticatedSshTransport> transport,
             cardio::cancellation cancellation) {
    if (transport == nullptr) {
      throw std::invalid_argument(
          "Authenticated SSH transport is required for SFTP");
    }
    std::shared_ptr<LibsshSftpClient> client(
        new LibsshSftpClient(std::move(transport)));
    co_await client->initialize_async(std::move(cancellation));
    co_return client;
  }

  auto capabilities() const noexcept
      -> RemoteFileCapabilities override {
    return {
        .symbolic_links = true,
        .permissions = true,
        .access_time = true,
        .modification_time = true,
    };
  }

  cardio::promise<RemoteDirectorySnapshot>
  load_directory_async(
      std::string path,
      cardio::cancellation cancellation) override {
    auto result = std::make_shared<RemoteDirectorySnapshot>();
    co_await run_async(
        [path = std::move(path), result](ssh_session session,
                                         sftp_session sftp) {
          char *canonical = sftp_canonicalize_path(sftp, path.c_str());
          if (canonical == nullptr) {
            throw libssh_sftp_failure(
                session, sftp, "Failed to canonicalize remote path");
          }
          result->canonical_path = canonical;
          ssh_string_free_char(canonical);

          sftp_dir directory =
              sftp_opendir(sftp, result->canonical_path.c_str());
          if (directory == nullptr) {
            throw libssh_sftp_failure(
                session, sftp, "Failed to open remote directory");
          }
          std::exception_ptr failure;
          try {
            for (;;) {
              sftp_attributes attributes =
                  sftp_readdir(sftp, directory);
              if (attributes == nullptr) {
                if (sftp_dir_eof(directory) == 0) {
                  throw libssh_sftp_failure(
                      session, sftp,
                      "Failed to read remote directory");
                }
                break;
              }
              const std::string name =
                  attributes->name == nullptr
                      ? std::string()
                      : std::string(attributes->name);
              try {
                if (name != "." && name != "..") {
                  result->entries.push_back(portable_attributes(
                      attributes,
                      sftp_child_path(result->canonical_path, name), name));
                }
              } catch (...) {
                sftp_attributes_free(attributes);
                throw;
              }
              sftp_attributes_free(attributes);
            }
          } catch (...) {
            failure = std::current_exception();
          }
          const int close_result = sftp_closedir(directory);
          if (failure) {
            std::rethrow_exception(failure);
          }
          if (close_result != SSH_OK) {
            throw libssh_sftp_failure(
                session, sftp, "Failed to close remote directory");
          }
        },
        std::move(cancellation));
    std::sort(
        result->entries.begin(), result->entries.end(),
        [](const RemoteFileAttributes &left,
           const RemoteFileAttributes &right) {
          return left.name < right.name;
        });
    co_return std::move(*result);
  }

  cardio::promise<std::optional<RemoteFileAttributes>>
  lstat_async(std::string path,
              cardio::cancellation cancellation) override {
    auto result =
        std::make_shared<std::optional<RemoteFileAttributes>>();
    co_await run_async(
        [path = std::move(path), result](ssh_session session,
                                         sftp_session sftp) {
          sftp_attributes attributes =
              sftp_lstat(sftp, path.c_str());
          if (attributes == nullptr) {
            const int code = sftp_get_error(sftp);
            if (code == SSH_FX_NO_SUCH_FILE ||
                code == SSH_FX_NO_SUCH_PATH) {
              result->reset();
              return;
            }
            throw libssh_sftp_failure(
                session, sftp, "Failed to inspect remote item");
          }
          result->emplace(portable_attributes(
              attributes, path, sftp_path_name(path)));
          sftp_attributes_free(attributes);
        },
        std::move(cancellation));
    co_return *result;
  }

  cardio::promise<std::string>
  read_link_async(std::string path,
                  cardio::cancellation cancellation) override {
    auto result = std::make_shared<std::string>();
    co_await run_async(
        [path = std::move(path), result](ssh_session session,
                                         sftp_session sftp) {
          char *target = sftp_readlink(sftp, path.c_str());
          if (target == nullptr) {
            throw libssh_sftp_failure(
                session, sftp, "Failed to read remote symbolic link");
          }
          *result = target;
          ssh_string_free_char(target);
        },
        std::move(cancellation));
    co_return *result;
  }

  cardio::promise<void>
  make_directory_async(
      std::string path, std::optional<std::uint32_t> permissions,
      cardio::cancellation cancellation) override {
    co_await run_async(
        [path = std::move(path), permissions](
            ssh_session session, sftp_session sftp) {
          if (sftp_mkdir(sftp, path.c_str(),
                         static_cast<mode_t>(
                             permissions.value_or(0755U) & 07777U)) !=
              SSH_OK) {
            throw libssh_sftp_failure(
                session, sftp, "Failed to create remote directory");
          }
        },
        std::move(cancellation));
  }

  cardio::promise<void>
  remove_file_async(
      std::string path,
      cardio::cancellation cancellation) override {
    co_await run_async(
        [path = std::move(path)](ssh_session session,
                                 sftp_session sftp) {
          if (sftp_unlink(sftp, path.c_str()) != SSH_OK) {
            throw libssh_sftp_failure(
                session, sftp, "Failed to remove remote file");
          }
        },
        std::move(cancellation));
  }

  cardio::promise<void>
  remove_directory_async(
      std::string path,
      cardio::cancellation cancellation) override {
    co_await run_async(
        [path = std::move(path)](ssh_session session,
                                 sftp_session sftp) {
          if (sftp_rmdir(sftp, path.c_str()) != SSH_OK) {
            throw libssh_sftp_failure(
                session, sftp, "Failed to remove remote directory");
          }
        },
        std::move(cancellation));
  }

  cardio::promise<void>
  rename_async(std::string source_path,
               std::string destination_path,
               cardio::cancellation cancellation) override {
    co_await run_async(
        [source_path = std::move(source_path),
         destination_path = std::move(destination_path)](
            ssh_session session, sftp_session sftp) {
          if (sftp_rename(sftp, source_path.c_str(),
                          destination_path.c_str()) != SSH_OK) {
            throw libssh_sftp_failure(
                session, sftp, "Failed to rename remote item");
          }
        },
        std::move(cancellation));
  }

  cardio::promise<void>
  make_symbolic_link_async(
      std::string target, std::string path,
      cardio::cancellation cancellation) override {
    co_await run_async(
        [target = std::move(target), path = std::move(path)](
            ssh_session session, sftp_session sftp) {
          if (sftp_symlink(sftp, target.c_str(), path.c_str()) !=
              SSH_OK) {
            throw libssh_sftp_failure(
                session, sftp,
                "Failed to create remote symbolic link");
          }
        },
        std::move(cancellation));
  }

  cardio::promise<void>
  set_attributes_async(
      std::string path, RemoteFileAttributes attributes,
      cardio::cancellation cancellation) override {
    co_await run_async(
        [path = std::move(path), attributes = std::move(attributes)](
            ssh_session session, sftp_session sftp) {
          if (attributes.permissions.has_value()) {
            if (sftp_chmod(
                    sftp, path.c_str(),
                    static_cast<mode_t>(*attributes.permissions & 07777U)) !=
                SSH_OK) {
              throw libssh_sftp_failure(
                  session, sftp,
                  "Failed to set remote item permissions");
            }
          }

          if (!attributes.access_time_unix_seconds.has_value() &&
              !attributes.modification_time_unix_seconds.has_value()) {
            return;
          }
          sftp_attributes current = sftp_lstat(sftp, path.c_str());
          if (current == nullptr) {
            throw libssh_sftp_failure(
                session, sftp, "Failed to inspect remote item times");
          }
          const std::int64_t access_time =
              attributes.access_time_unix_seconds.value_or(
                  static_cast<std::int64_t>(current->atime));
          const std::int64_t modification_time =
              attributes.modification_time_unix_seconds.value_or(
                  static_cast<std::int64_t>(current->mtime));
          sftp_attributes_free(current);
          const timeval times[2] = {
              timeval{
                  .tv_sec = static_cast<time_t>(access_time),
                  .tv_usec = 0,
              },
              timeval{
                  .tv_sec = static_cast<time_t>(modification_time),
                  .tv_usec = 0,
              },
          };
          if (sftp_utimes(sftp, path.c_str(), times) != SSH_OK) {
            throw libssh_sftp_failure(
                session, sftp, "Failed to set remote item times");
          }
        },
        std::move(cancellation));
  }

  cardio::promise<std::unique_ptr<RemoteFileReader>>
  open_read_async(std::string path,
                  cardio::cancellation cancellation) override;

  cardio::promise<std::unique_ptr<RemoteFileWriter>>
  open_write_async(std::string path,
                   std::optional<std::uint32_t> permissions,
                   cardio::cancellation cancellation) override;

  bool try_begin_transfer() override {
    return transport->try_begin_sftp_transfer();
  }

  void end_transfer() override {
    transport->end_sftp_transfer();
  }
};

class LibsshSftpClient::Reader final : public RemoteFileReader {
private:
  std::shared_ptr<LibsshSftpClient> owner;
  std::shared_ptr<LibsshSftpFileState> state;

public:
  Reader(std::shared_ptr<LibsshSftpClient> owner,
         std::shared_ptr<LibsshSftpFileState> state)
      : owner(std::move(owner)), state(std::move(state)) {
  }

  ~Reader() override {
    owner->close_file_later(state);
  }

  cardio::promise<std::size_t>
  read_async(std::span<std::byte> buffer,
             cardio::cancellation cancellation) override {
    auto storage =
        std::make_shared<std::vector<std::byte>>(buffer.size());
    auto size = std::make_shared<std::size_t>(0);
    const std::shared_ptr<LibsshSftpFileState> file_state = state;
    co_await owner->run_async(
        [file_state, storage, size](ssh_session session,
                                    sftp_session sftp) {
          if (file_state->file == nullptr) {
            throw std::runtime_error("Remote SFTP file is closed");
          }
          const ssize_t result =
              sftp_read(file_state->file, storage->data(),
                        storage->size());
          if (result < 0) {
            throw libssh_sftp_failure(
                session, sftp, "Failed to read remote file");
          }
          *size = static_cast<std::size_t>(result);
        },
        std::move(cancellation));
    std::copy_n(storage->data(), *size, buffer.data());
    co_return *size;
  }

  cardio::promise<void>
  close_async(cardio::cancellation cancellation) override {
    const std::shared_ptr<LibsshSftpFileState> file_state = state;
    co_await owner->run_async(
        [file_state](ssh_session session, sftp_session sftp) {
          if (file_state->file == nullptr) {
            return;
          }
          sftp_file file = file_state->file;
          file_state->file = nullptr;
          if (sftp_close(file) != SSH_OK) {
            throw libssh_sftp_failure(
                session, sftp, "Failed to close remote file");
          }
        },
        std::move(cancellation));
  }
};

class LibsshSftpClient::Writer final : public RemoteFileWriter {
private:
  std::shared_ptr<LibsshSftpClient> owner;
  std::shared_ptr<LibsshSftpFileState> state;

public:
  Writer(std::shared_ptr<LibsshSftpClient> owner,
         std::shared_ptr<LibsshSftpFileState> state)
      : owner(std::move(owner)), state(std::move(state)) {
  }

  ~Writer() override {
    owner->close_file_later(state);
  }

  cardio::promise<void>
  write_all_async(std::span<const std::byte> buffer,
                  cardio::cancellation cancellation) override {
    auto storage = std::make_shared<std::vector<std::byte>>(
        buffer.begin(), buffer.end());
    const std::shared_ptr<LibsshSftpFileState> file_state = state;
    co_await owner->run_async(
        [file_state, storage](ssh_session session, sftp_session sftp) {
          if (file_state->file == nullptr) {
            throw std::runtime_error("Remote SFTP file is closed");
          }
          std::size_t offset = 0;
          while (offset < storage->size()) {
            const ssize_t written = sftp_write(
                file_state->file, storage->data() + offset,
                storage->size() - offset);
            if (written <= 0) {
              throw libssh_sftp_failure(
                  session, sftp, "Failed to write remote file");
            }
            offset += static_cast<std::size_t>(written);
          }
        },
        std::move(cancellation));
  }

  cardio::promise<void>
  close_async(cardio::cancellation cancellation) override {
    const std::shared_ptr<LibsshSftpFileState> file_state = state;
    co_await owner->run_async(
        [file_state](ssh_session session, sftp_session sftp) {
          if (file_state->file == nullptr) {
            return;
          }
          sftp_file file = file_state->file;
          file_state->file = nullptr;
          if (sftp_close(file) != SSH_OK) {
            throw libssh_sftp_failure(
                session, sftp, "Failed to close remote file");
          }
        },
        std::move(cancellation));
  }
};

cardio::promise<std::unique_ptr<RemoteFileReader>>
LibsshSftpClient::open_read_async(
    std::string path, cardio::cancellation cancellation) {
  auto file_state = std::make_shared<LibsshSftpFileState>();
  co_await run_async(
      [path = std::move(path), file_state](
          ssh_session session, sftp_session sftp) {
        file_state->file =
            sftp_open(sftp, path.c_str(), O_RDONLY, 0);
        if (file_state->file == nullptr) {
          throw libssh_sftp_failure(
              session, sftp, "Failed to open remote file for reading");
        }
      },
      std::move(cancellation));
  co_return std::make_unique<Reader>(shared_from_this(),
                                     std::move(file_state));
}

cardio::promise<std::unique_ptr<RemoteFileWriter>>
LibsshSftpClient::open_write_async(
    std::string path, std::optional<std::uint32_t> permissions,
    cardio::cancellation cancellation) {
  auto file_state = std::make_shared<LibsshSftpFileState>();
  co_await run_async(
      [path = std::move(path), permissions, file_state](
          ssh_session session, sftp_session sftp) {
        file_state->file = sftp_open(
            sftp, path.c_str(), O_WRONLY | O_CREAT | O_TRUNC,
            static_cast<mode_t>(permissions.value_or(0600U) & 07777U));
        if (file_state->file == nullptr) {
          throw libssh_sftp_failure(
              session, sftp, "Failed to open remote file for writing");
        }
      },
      std::move(cancellation));
  co_return std::make_unique<Writer>(shared_from_this(),
                                     std::move(file_state));
}

cardio::promise<std::shared_ptr<RemoteFileClient>>
open_sftp_client_async(
    std::shared_ptr<AuthenticatedSshTransport> transport,
    cardio::cancellation cancellation) {
  co_return co_await LibsshSftpClient::open_async(
      std::move(transport), std::move(cancellation));
}

} // namespace elder_terms
