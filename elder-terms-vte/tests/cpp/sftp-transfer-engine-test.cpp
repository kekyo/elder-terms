#include "../../src/file-transfer/file-transfer-engine.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <glib.h>

namespace elder_terms_sftp_transfer_engine_test {

struct TemporaryDirectoryCleanup {
  std::filesystem::path path;

  ~TemporaryDirectoryCleanup() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

struct FakeNode {
  elder_terms::RemoteFileAttributes attributes;
  std::vector<std::byte> content;
  std::string link_target;
};

static void expect_true(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

static std::string normalize_remote_path(const std::string &path) {
  std::string result =
      std::filesystem::path(path).lexically_normal().generic_string();
  if (result.empty()) {
    return ".";
  }
  return result;
}

static std::string remote_name(const std::string &path) {
  return std::filesystem::path(path).filename().generic_string();
}

static std::string remote_parent(const std::string &path) {
  return std::filesystem::path(path).parent_path().generic_string();
}

static std::vector<std::byte> bytes(const std::string &value) {
  const auto *begin =
      reinterpret_cast<const std::byte *>(value.data());
  return std::vector<std::byte>(begin, begin + value.size());
}

static std::string text(const std::vector<std::byte> &value) {
  return std::string(reinterpret_cast<const char *>(value.data()),
                     value.size());
}

class FakeSftpClient;

class FakeSftpReader final : public elder_terms::RemoteFileReader {
private:
  std::vector<std::byte> content;
  std::size_t offset = 0;
  bool closed = false;

public:
  explicit FakeSftpReader(std::vector<std::byte> content)
      : content(std::move(content)) {
  }

  cardio::promise<std::size_t>
  read_async(std::span<std::byte> buffer,
             cardio::cancellation cancellation) override {
    cancellation.throw_if_cancellation_requested();
    if (closed) {
      throw std::runtime_error("fake SFTP reader is closed");
    }
    const std::size_t size =
        std::min(buffer.size(), content.size() - offset);
    std::copy_n(content.data() + offset, size, buffer.data());
    offset += size;
    co_return size;
  }

  cardio::promise<void>
  close_async(cardio::cancellation cancellation) override {
    cancellation.throw_if_cancellation_requested();
    closed = true;
    co_return;
  }
};

class FakeSftpWriter final : public elder_terms::RemoteFileWriter {
private:
  std::shared_ptr<FakeSftpClient> client;
  std::string path;
  bool closed = false;

public:
  FakeSftpWriter(std::shared_ptr<FakeSftpClient> client,
                 std::string path)
      : client(std::move(client)), path(std::move(path)) {
  }

  cardio::promise<void>
  write_all_async(std::span<const std::byte> buffer,
                  cardio::cancellation cancellation) override;

  cardio::promise<void>
  close_async(cardio::cancellation cancellation) override {
    cancellation.throw_if_cancellation_requested();
    closed = true;
    co_return;
  }
};

class FakeSftpClient final
    : public elder_terms::RemoteFileClient,
      public std::enable_shared_from_this<FakeSftpClient> {
public:
  std::map<std::string, FakeNode> nodes;
  bool transfer_active = false;
  bool fail_next_write = false;

  void add_directory(const std::string &path,
                     std::uint32_t permissions = 0755,
                     std::int64_t mtime = 100) {
    const std::string normalized = normalize_remote_path(path);
    nodes[normalized] = FakeNode{
        .attributes =
            {
                .name = remote_name(normalized),
                .path = normalized,
                .type = elder_terms::RemoteFileType::directory,
                .size = 0,
                .permissions = permissions,
                .access_time_unix_seconds = mtime,
                .modification_time_unix_seconds = mtime,
            },
        .content = {},
        .link_target = {},
    };
  }

  void add_file(const std::string &path, const std::string &content,
                std::uint32_t permissions = 0644,
                std::int64_t mtime = 100) {
    const std::string normalized = normalize_remote_path(path);
    nodes[normalized] = FakeNode{
        .attributes =
            {
                .name = remote_name(normalized),
                .path = normalized,
                .type = elder_terms::RemoteFileType::regular,
                .size = content.size(),
                .permissions = permissions,
                .access_time_unix_seconds = mtime,
                .modification_time_unix_seconds = mtime,
            },
        .content = bytes(content),
        .link_target = {},
    };
  }

  void add_link(const std::string &path, const std::string &target,
                std::int64_t mtime = 100) {
    const std::string normalized = normalize_remote_path(path);
    nodes[normalized] = FakeNode{
        .attributes =
            {
                .name = remote_name(normalized),
                .path = normalized,
                .type = elder_terms::RemoteFileType::symbolic_link,
                .size = target.size(),
                .permissions = 0777,
                .access_time_unix_seconds = mtime,
                .modification_time_unix_seconds = mtime,
            },
        .content = {},
        .link_target = target,
    };
  }

  auto capabilities() const noexcept
      -> elder_terms::RemoteFileCapabilities override {
    return {
        .symbolic_links = true,
        .permissions = true,
        .access_time = true,
        .modification_time = true,
    };
  }

  cardio::promise<elder_terms::RemoteDirectorySnapshot>
  load_directory_async(std::string path,
                       cardio::cancellation cancellation) override {
    cancellation.throw_if_cancellation_requested();
    const std::string normalized = normalize_remote_path(path);
    const auto directory = nodes.find(normalized);
    if (directory == nodes.end() ||
        directory->second.attributes.type !=
            elder_terms::RemoteFileType::directory) {
      throw std::runtime_error("fake remote directory does not exist");
    }

    elder_terms::RemoteDirectorySnapshot result{
        .canonical_path = normalized,
        .entries = {},
    };
    for (const auto &[candidate_path, node] : nodes) {
      if (candidate_path != normalized &&
          remote_parent(candidate_path) == normalized) {
        result.entries.push_back(node.attributes);
      }
    }
    co_return result;
  }

  cardio::promise<std::optional<elder_terms::RemoteFileAttributes>>
  lstat_async(std::string path,
              cardio::cancellation cancellation) override {
    cancellation.throw_if_cancellation_requested();
    const auto iterator = nodes.find(normalize_remote_path(path));
    if (iterator == nodes.end()) {
      co_return std::nullopt;
    }
    co_return iterator->second.attributes;
  }

  cardio::promise<std::string>
  read_link_async(std::string path,
                  cardio::cancellation cancellation) override {
    cancellation.throw_if_cancellation_requested();
    const auto iterator = nodes.find(normalize_remote_path(path));
    if (iterator == nodes.end() ||
        iterator->second.attributes.type !=
            elder_terms::RemoteFileType::symbolic_link) {
      throw std::runtime_error("fake remote link does not exist");
    }
    co_return iterator->second.link_target;
  }

  cardio::promise<void>
  make_directory_async(std::string path,
                       std::optional<std::uint32_t> permissions,
                       cardio::cancellation cancellation) override {
    cancellation.throw_if_cancellation_requested();
    add_directory(path, permissions.value_or(0755U));
    co_return;
  }

  cardio::promise<void>
  remove_file_async(std::string path,
                    cardio::cancellation cancellation) override {
    cancellation.throw_if_cancellation_requested();
    const std::string normalized = normalize_remote_path(path);
    const auto iterator = nodes.find(normalized);
    if (iterator == nodes.end() ||
        iterator->second.attributes.type ==
            elder_terms::RemoteFileType::directory) {
      throw std::runtime_error("fake remote file does not exist");
    }
    nodes.erase(iterator);
    co_return;
  }

  cardio::promise<void>
  remove_directory_async(std::string path,
                         cardio::cancellation cancellation) override {
    cancellation.throw_if_cancellation_requested();
    const std::string normalized = normalize_remote_path(path);
    const auto child = std::find_if(
        nodes.begin(), nodes.end(),
        [&normalized](const auto &entry) {
          return entry.first != normalized &&
                 remote_parent(entry.first) == normalized;
        });
    if (child != nodes.end()) {
      throw std::runtime_error("fake remote directory is not empty");
    }
    const auto iterator = nodes.find(normalized);
    if (iterator == nodes.end() ||
        iterator->second.attributes.type !=
            elder_terms::RemoteFileType::directory) {
      throw std::runtime_error("fake remote directory does not exist");
    }
    nodes.erase(iterator);
    co_return;
  }

  cardio::promise<void>
  rename_async(std::string source_path, std::string destination_path,
               cardio::cancellation cancellation) override {
    cancellation.throw_if_cancellation_requested();
    const std::string source = normalize_remote_path(source_path);
    const std::string destination =
        normalize_remote_path(destination_path);
    const auto iterator = nodes.find(source);
    if (iterator == nodes.end() || nodes.contains(destination)) {
      throw std::runtime_error("fake remote rename failed");
    }
    FakeNode node = std::move(iterator->second);
    nodes.erase(iterator);
    node.attributes.name = remote_name(destination);
    node.attributes.path = destination;
    nodes.emplace(destination, std::move(node));
    co_return;
  }

  cardio::promise<void>
  make_symbolic_link_async(std::string target, std::string path,
                          cardio::cancellation cancellation) override {
    cancellation.throw_if_cancellation_requested();
    add_link(path, target);
    co_return;
  }

  cardio::promise<void>
  set_attributes_async(std::string path,
                       elder_terms::RemoteFileAttributes attributes,
                       cardio::cancellation cancellation) override {
    cancellation.throw_if_cancellation_requested();
    const auto iterator = nodes.find(normalize_remote_path(path));
    if (iterator == nodes.end()) {
      throw std::runtime_error("fake remote item does not exist");
    }
    if (attributes.permissions.has_value()) {
      iterator->second.attributes.permissions = attributes.permissions;
    }
    if (attributes.access_time_unix_seconds.has_value()) {
      iterator->second.attributes.access_time_unix_seconds =
          attributes.access_time_unix_seconds;
    }
    if (attributes.modification_time_unix_seconds.has_value()) {
      iterator->second.attributes.modification_time_unix_seconds =
          attributes.modification_time_unix_seconds;
    }
    co_return;
  }

  cardio::promise<std::unique_ptr<elder_terms::RemoteFileReader>>
  open_read_async(std::string path,
                  cardio::cancellation cancellation) override {
    cancellation.throw_if_cancellation_requested();
    const auto iterator = nodes.find(normalize_remote_path(path));
    if (iterator == nodes.end() ||
        iterator->second.attributes.type !=
            elder_terms::RemoteFileType::regular) {
      throw std::runtime_error("fake remote file does not exist");
    }
    co_return std::make_unique<FakeSftpReader>(iterator->second.content);
  }

  cardio::promise<std::unique_ptr<elder_terms::RemoteFileWriter>>
  open_write_async(std::string path,
                   std::optional<std::uint32_t> permissions,
                   cardio::cancellation cancellation) override {
    cancellation.throw_if_cancellation_requested();
    const std::string normalized = normalize_remote_path(path);
    add_file(normalized, "", permissions.value_or(0600U));
    co_return std::make_unique<FakeSftpWriter>(
        shared_from_this(), normalized);
  }

  bool try_begin_transfer() override {
    if (transfer_active) {
      return false;
    }
    transfer_active = true;
    return true;
  }

  void end_transfer() override {
    transfer_active = false;
  }
};

cardio::promise<void>
FakeSftpWriter::write_all_async(
    std::span<const std::byte> buffer,
    cardio::cancellation cancellation) {
  cancellation.throw_if_cancellation_requested();
  if (closed) {
    throw std::runtime_error("fake SFTP writer is closed");
  }
  if (client->fail_next_write) {
    client->fail_next_write = false;
    throw std::runtime_error("injected remote write failure");
  }
  FakeNode &node = client->nodes.at(path);
  node.content.insert(node.content.end(), buffer.begin(), buffer.end());
  node.attributes.size = node.content.size();
  co_return;
}

static std::filesystem::path test_root_directory() {
  return std::filesystem::temp_directory_path() /
         ("elder-terms-sftp-transfer-test-" +
          std::to_string(::getpid()));
}

static std::string read_file(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(file),
                     std::istreambuf_iterator<char>());
}

static void write_file(const std::filesystem::path &path,
                       const std::string &content) {
  std::ofstream file(path, std::ios::binary);
  file << content;
  if (!file) {
    throw std::runtime_error("failed to create local test file");
  }
}

static void set_file_times(const std::filesystem::path &path,
                           std::int64_t seconds,
                           int flags = 0) {
  const timespec times[2] = {
      timespec{.tv_sec = seconds, .tv_nsec = 0},
      timespec{.tv_sec = seconds, .tv_nsec = 0},
  };
  if (::utimensat(AT_FDCWD, path.c_str(), times, flags) < 0) {
    throw std::runtime_error("failed to set local test file times");
  }
}

static struct stat lstat_path(const std::filesystem::path &path) {
  struct stat result = {};
  if (::lstat(path.c_str(), &result) < 0) {
    throw std::runtime_error("failed to stat local test path");
  }
  return result;
}

static cardio::promise<void>
test_recursive_send_preserves_links_metadata_and_recovers() {
  const std::filesystem::path root = test_root_directory() / "send";
  std::filesystem::create_directories(root / "bundle");
  const TemporaryDirectoryCleanup cleanup{
      .path = test_root_directory(),
  };
  const std::filesystem::path source_file = root / "bundle" / "alpha.txt";
  write_file(source_file, "new remote content");
  expect_true(::chmod(source_file.c_str(), 0640) == 0,
              "failed to set source permissions");
  set_file_times(source_file, 1'700'000'123);
  std::filesystem::create_symlink("alpha.txt",
                                  root / "bundle" / "alpha-link");

  auto client = std::make_shared<FakeSftpClient>();
  client->add_directory("/");
  client->add_directory("/incoming");
  client->add_directory("/incoming/bundle");
  client->add_file("/incoming/bundle/alpha.txt", "old remote content");
  client->fail_next_write = true;
  int conflict_count = 0;
  int failure_count = 0;
  elder_terms::FileTransferProgress last_progress;

  elder_terms::FileTransferRequest request{
      .direction = elder_terms::FileTransferDirection::send,
      .source_paths = {(root / "bundle").string()},
      .destination_directory = "/incoming",
      .callbacks =
          {
              .conflict =
                  [&conflict_count](
                      const elder_terms::FileTransferConflict &,
                      cardio::cancellation cancellation)
                  -> cardio::promise<elder_terms::FileTransferConflictAction> {
                cancellation.throw_if_cancellation_requested();
                ++conflict_count;
                co_return elder_terms::FileTransferConflictAction::overwrite;
              },
              .failure =
                  [&failure_count](
                      const elder_terms::FileTransferFailure &failure,
                      cardio::cancellation cancellation)
                  -> cardio::promise<elder_terms::FileTransferFailureAction> {
                cancellation.throw_if_cancellation_requested();
                expect_true(
                    failure.message.find("injected remote write failure") !=
                        std::string::npos,
                    "send failure did not describe the remote write");
                ++failure_count;
                co_return elder_terms::FileTransferFailureAction::retry;
              },
              .progress =
                  [&last_progress](
                      const elder_terms::FileTransferProgress &progress) {
                    last_progress = progress;
                  },
          },
  };

  co_await elder_terms::run_file_transfer_async(client, std::move(request),
                                                 {});

  expect_true(conflict_count == 1,
              "send conflict decision should be requested once");
  expect_true(failure_count == 1,
              "send runtime failure should be retried once");
  expect_true(
      text(client->nodes.at("/incoming/bundle/alpha.txt").content) ==
          "new remote content",
      "send did not replace the conflicting remote file");
  expect_true(
      client->nodes.at("/incoming/bundle/alpha-link").attributes.type ==
          elder_terms::RemoteFileType::symbolic_link &&
          client->nodes.at("/incoming/bundle/alpha-link").link_target ==
              "alpha.txt",
      "send followed or lost the local symbolic link");
  const FakeNode &remote_file =
      client->nodes.at("/incoming/bundle/alpha.txt");
  expect_true(remote_file.attributes.permissions.has_value() &&
                  (*remote_file.attributes.permissions & 0777U) == 0640U,
              "send did not preserve file permissions");
  expect_true(remote_file.attributes.modification_time_unix_seconds ==
                  1'700'000'123,
              "send did not preserve file modification time");
  expect_true(last_progress.transferred_bytes ==
                  std::string("new remote content").size() &&
                  last_progress.completed_items ==
                      last_progress.total_items,
              "send did not report completed aggregate progress");
}

static cardio::promise<void>
test_recursive_receive_preserves_links_and_metadata() {
  const std::filesystem::path root = test_root_directory() / "receive";
  const TemporaryDirectoryCleanup cleanup{
      .path = test_root_directory(),
  };
  std::filesystem::create_directories(root / "bundle");
  write_file(root / "bundle" / "report.txt", "old local content");

  auto client = std::make_shared<FakeSftpClient>();
  client->add_directory("/");
  client->add_directory("/exports");
  client->add_directory("/exports/bundle", 0750, 1'700'001'000);
  client->add_file("/exports/bundle/report.txt", "downloaded content",
                   0600, 1'700'001'234);
  client->add_link("/exports/bundle/report-link", "report.txt",
                   1'700'001'345);
  int conflict_count = 0;

  elder_terms::FileTransferRequest request{
      .direction = elder_terms::FileTransferDirection::receive,
      .source_paths = {"/exports/bundle"},
      .destination_directory = root.string(),
      .callbacks =
          {
              .conflict =
                  [&conflict_count](
                      const elder_terms::FileTransferConflict &,
                      cardio::cancellation cancellation)
                  -> cardio::promise<elder_terms::FileTransferConflictAction> {
                cancellation.throw_if_cancellation_requested();
                ++conflict_count;
                co_return elder_terms::FileTransferConflictAction::overwrite;
              },
              .failure = {},
              .progress = {},
          },
  };

  co_await elder_terms::run_file_transfer_async(client, std::move(request),
                                                 {});

  expect_true(conflict_count == 1,
              "receive conflict decision should be requested once");
  expect_true(read_file(root / "bundle" / "report.txt") ==
                  "downloaded content",
              "receive did not replace the conflicting local file");
  expect_true(std::filesystem::is_symlink(
                  root / "bundle" / "report-link") &&
                  std::filesystem::read_symlink(
                      root / "bundle" / "report-link") == "report.txt",
              "receive followed or lost the remote symbolic link");
  const struct stat file_status =
      lstat_path(root / "bundle" / "report.txt");
  expect_true((file_status.st_mode & 0777) == 0600,
              "receive did not preserve file permissions");
  expect_true(file_status.st_mtime == 1'700'001'234,
              "receive did not preserve file modification time");
  const struct stat directory_status = lstat_path(root / "bundle");
  expect_true((directory_status.st_mode & 0777) == 0750 &&
                  directory_status.st_mtime == 1'700'001'000,
              "receive did not preserve directory metadata after children");
}

static cardio::promise<void>
test_cancellation_removes_remote_temporary_file() {
  const std::filesystem::path root = test_root_directory() / "cancel";
  const TemporaryDirectoryCleanup cleanup{
      .path = test_root_directory(),
  };
  std::filesystem::create_directories(root);
  const std::string content(192 * 1024, 'x');
  write_file(root / "large.bin", content);

  auto client = std::make_shared<FakeSftpClient>();
  client->add_directory("/");
  client->add_directory("/incoming");
  cardio::cancellation_source cancellation_source;
  bool canceled_from_progress = false;
  elder_terms::FileTransferRequest request{
      .direction = elder_terms::FileTransferDirection::send,
      .source_paths = {(root / "large.bin").string()},
      .destination_directory = "/incoming",
      .callbacks =
          {
              .conflict = {},
              .failure = {},
              .progress =
                  [&cancellation_source, &canceled_from_progress](
                      const elder_terms::FileTransferProgress &progress) {
                    if (!canceled_from_progress &&
                        progress.transferred_bytes > 0) {
                      canceled_from_progress = true;
                      (void)cancellation_source.cancel();
                    }
                  },
          },
  };

  bool canceled = false;
  try {
    co_await elder_terms::run_file_transfer_async(
        client, std::move(request),
        cancellation_source.get_cancellation());
  } catch (const cardio::canceled_exception &) {
    canceled = true;
  }

  expect_true(canceled && canceled_from_progress,
              "send cancellation was not propagated");
  const bool temporary_file_remains = std::any_of(
      client->nodes.begin(), client->nodes.end(),
      [](const auto &entry) {
        return entry.first.find(".elder-terms-part-") !=
               std::string::npos;
      });
  expect_true(!temporary_file_remains,
              "send cancellation left a remote temporary file");
  expect_true(!client->nodes.contains("/incoming/large.bin"),
              "send cancellation committed the incomplete remote file");
  expect_true(!client->transfer_active,
              "send cancellation did not release the transfer slot");
}

static cardio::promise<void> test_rejects_parallel_bulk_transfer() {
  auto client = std::make_shared<FakeSftpClient>();
  client->add_directory("/");
  expect_true(client->try_begin_transfer(),
              "failed to reserve fake transfer slot");
  bool rejected = false;
  try {
    co_await elder_terms::run_file_transfer_async(
        client,
        elder_terms::FileTransferRequest{
            .direction = elder_terms::FileTransferDirection::receive,
            .source_paths = {"/missing"},
            .destination_directory = "/tmp",
            .callbacks = {},
        },
        {});
  } catch (const std::runtime_error &error) {
    rejected =
        std::string(error.what()).find("already in progress") !=
        std::string::npos;
  }
  client->end_transfer();
  expect_true(rejected,
              "parallel SFTP bulk transfer should be rejected");
}

} // namespace elder_terms_sftp_transfer_engine_test

int main() {
  using namespace elder_terms_sftp_transfer_engine_test;

  cardio::dispatcher_group_glib dispatcher_group;
  cardio::dispatcher_host_glib dispatcher(dispatcher_group);
  std::exception_ptr error;
  auto task = [&]() -> cardio::promise<void> {
    try {
      co_await test_recursive_send_preserves_links_metadata_and_recovers();
      co_await test_recursive_receive_preserves_links_and_metadata();
      co_await test_cancellation_removes_remote_temporary_file();
      co_await test_rejects_parallel_bulk_transfer();
    } catch (...) {
      error = std::current_exception();
    }
    dispatcher_group.shutdown();
  }();

  dispatcher.park();
  task.unsafe_result();
  if (error) {
    try {
      std::rethrow_exception(error);
    } catch (const std::exception &exception) {
      g_printerr("sftp-transfer-engine-test failed: %s\n",
                 exception.what());
    }
    return 1;
  }
  return 0;
}
