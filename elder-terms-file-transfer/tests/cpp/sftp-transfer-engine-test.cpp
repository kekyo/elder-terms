// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "../../src/file-transfer/file-transfer-engine.h"
#include "../../src/file-transfer/file-transfer-job.h"
#include "../../src/file-transfer/activity-file-client.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
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
  std::function<cardio::promise<void>()> before_close;

public:
  FakeSftpReader(std::vector<std::byte> content, std::function<cardio::promise<void>()> before_close)
      : content(std::move(content)), before_close(std::move(before_close)) {
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
    if (!closed && before_close) co_await before_close();
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
  bool encoded_child_names = false;
  std::set<std::string> undecodable_names;
  bool encoded_link_targets = false;
  std::function<cardio::promise<void>()> before_read_close;
  std::function<cardio::promise<void>(cardio::cancellation)> before_listing;
  std::function<void()> before_stat;
  std::function<void()> before_write;
  std::function<void()> before_remove;

  std::string child_path(const std::string &parent, const std::string &name) const override {
    return normalize_remote_path(parent + "/" + (encoded_child_names ? "id-" : "") + name);
  }

  std::string upload_temporary_path(const std::string &destination, const std::string &suffix) const override {
    if (!encoded_child_names) return destination + suffix;
    return destination.substr(0, destination.find_last_of('/')) + "/id-" + suffix;
  }

  std::optional<std::string> local_filename(const elder_terms::RemoteFileAttributes &item) const override {
    return undecodable_names.contains(item.path) ? std::nullopt : std::optional(item.name);
  }
  std::string import_link_target(const std::string &target) const override {
    return encoded_link_targets ? "wire:" + target : target;
  }
  std::string export_link_target(const std::string &target) const override {
    if (!encoded_link_targets) return target;
    if (!target.starts_with("wire:")) throw std::runtime_error("Undecodable link target");
    return target.substr(5);
  }

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
    if (before_listing) co_await before_listing(cancellation);
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
    if (before_stat) before_stat();
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
    if (before_remove) before_remove();
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
    co_return std::make_unique<FakeSftpReader>(iterator->second.content, before_read_close);
  }

  cardio::promise<std::unique_ptr<elder_terms::RemoteFileWriter>>
  open_write_async(std::string path, std::uint64_t,
                   std::optional<std::uint32_t> permissions,
                   cardio::cancellation cancellation) override {
    cancellation.throw_if_cancellation_requested();
    const std::string normalized = normalize_remote_path(path);
    if (encoded_child_names && !remote_name(normalized).starts_with("id-.elder-terms-part-"))
      throw std::runtime_error("Temporary upload identities must be generated by the remote client");
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
  if (client->before_write) client->before_write();
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
                           std::time_t seconds,
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


static cardio::promise<void> test_receive_preserves_unknown_aggregate_size() {
  const auto root = test_root_directory() / "unknown";
  const TemporaryDirectoryCleanup cleanup{.path = test_root_directory()};
  std::filesystem::create_directories(root);
  auto client = std::make_shared<FakeSftpClient>();
  client->add_directory("/");
  client->add_file("/unknown.txt", "unknown transfer");
  client->nodes.at("/unknown.txt").attributes.size.reset();
  elder_terms::FileTransferProgress final_progress;
  elder_terms::FileTransferRequest request;
  request.direction = elder_terms::FileTransferDirection::receive;
  request.source_paths = {"/unknown.txt"};
  request.destination_directory = root.string();
  request.callbacks.progress = [&final_progress](const elder_terms::FileTransferProgress &progress) {
    final_progress = progress;
  };
  auto pending = elder_terms::run_file_transfer_async(client, std::move(request), {});
  co_await pending;
  const std::optional<std::uint64_t> total = final_progress.total_bytes;
  expect_true(!total && final_progress.transferred_bytes == 16 && final_progress.completed_items == 1,
              "An unknown file size must remain unknown in aggregate progress");
  expect_true(read_file(root / "unknown.txt") == "unknown transfer",
              "Missing size metadata must not prevent receiving the file");
}


static cardio::promise<elder_terms::FileTransferConflictAction>
overwrite_changed_source_conflict(const elder_terms::FileTransferConflict &,
                                  cardio::cancellation cancellation) {
  cancellation.throw_if_cancellation_requested();
  co_return elder_terms::FileTransferConflictAction::overwrite;
}

static cardio::promise<void> test_changed_source_does_not_commit_partial_upload() {
  for (const auto &replacement : std::vector<std::string>{"short", "a replacement that is larger than the discovered source"}) {
    const auto root = test_root_directory() / "changed-source";
    std::filesystem::create_directories(root);
    const TemporaryDirectoryCleanup cleanup{test_root_directory()};
    const auto source = root / "source.txt";
    write_file(source, "discovered source content");
    auto client = std::make_shared<FakeSftpClient>();
    client->add_directory("/");
    client->add_directory("/incoming");
    client->add_file("/incoming/source.txt", "existing destination");
    bool changed = false;
    elder_terms::FileTransferRequest request;
    request.direction = elder_terms::FileTransferDirection::send;
    request.source_paths = {source.string()};
    request.destination_directory = "/incoming";
    request.callbacks.conflict = overwrite_changed_source_conflict;
    request.callbacks.progress = [&](const elder_terms::FileTransferProgress &progress) {
      if (!changed && progress.current_path.empty() && progress.total_items > 0) {
        write_file(source, replacement);
        changed = true;
      }
    };
    bool failed = false;
    try {
      co_await elder_terms::run_file_transfer_async(client, std::move(request), {});
    } catch (const std::exception &) { failed = true; }
    expect_true(changed && failed,
                "An upload must fail if its source grows or shrinks after discovery");
    expect_true(text(client->nodes.at("/incoming/source.txt").content) == "existing destination",
                "A changed source must not replace a completed destination");
    expect_true(client->nodes.size() == 3,
                "A rejected upload must remove its own temporary file");
  }
}

static cardio::promise<void> test_changed_remote_size_preserves_local_destination() {
  for (const std::uint64_t reported : {3U, 30U}) {
    const auto root = test_root_directory() / "changed-remote-size";
    const TemporaryDirectoryCleanup cleanup{test_root_directory()};
    std::filesystem::create_directories(root);
    write_file(root / "file.txt", "original");
    auto client = std::make_shared<FakeSftpClient>();
    client->add_directory("/");
    client->add_file("/file.txt", "payload");
    client->nodes.at("/file.txt").attributes.size = reported;
    elder_terms::FileTransferResult result;
    elder_terms::FileTransferRequest request;
    request.direction = elder_terms::FileTransferDirection::receive;
    request.source_paths = {"/file.txt"};
    request.destination_directory = root.string();
    request.callbacks.conflict = overwrite_changed_source_conflict;
    request.callbacks.finished = [&](const auto &value) { result = value; };
    bool failed = false;
    try { (void)co_await elder_terms::run_file_transfer_async(client, request, {}); }
    catch (const std::exception &) { failed = true; }
    expect_true(failed && result.outcome == elder_terms::FileTransferOutcome::failed && result.succeeded_items == 0,
        "A remote size mismatch must fail before committing the received file");
    expect_true(read_file(root / "file.txt") == "original" && result.temporary_paths.empty() &&
        std::distance(std::filesystem::directory_iterator(root), std::filesystem::directory_iterator{}) == 1,
        "Failed receive must preserve the destination and clean its adjacent temporary file");
  }
}

static cardio::promise<void> test_receive_reports_unrepresentable_total_as_unknown() {
  const auto root = test_root_directory() / "overflow";
  const TemporaryDirectoryCleanup cleanup{test_root_directory()};
  std::filesystem::create_directories(root);
  auto client = std::make_shared<FakeSftpClient>();
  client->add_directory("/");
  client->add_directory("/bundle");
  client->add_file("/bundle/first.txt", "a");
  client->add_file("/bundle/second.txt", "b");
  client->nodes.at("/bundle/first.txt").attributes.size = UINT64_MAX;
  elder_terms::FileTransferProgress final_progress;
  cardio::cancellation_source stopping;
  elder_terms::FileTransferRequest request;
  request.direction = elder_terms::FileTransferDirection::receive;
  request.source_paths = {"/bundle"};
  request.destination_directory = root.string();
  request.callbacks.progress = [&](const elder_terms::FileTransferProgress &progress) {
    final_progress = progress;
    if (progress.discovery_complete) (void)stopping.cancel();
  };
  // The huge metadata exercises aggregate accounting, without pretending the
  // small fixture contains UINT64_MAX bytes. Stop before opening any file.
  bool cancelled = false;
  try { (void)co_await elder_terms::run_file_transfer_async(client, std::move(request), stopping.get_cancellation()); }
  catch (const cardio::canceled_exception &) { cancelled = true; }
  const std::optional<std::uint64_t> total = final_progress.total_bytes;
  expect_true(cancelled && !total && final_progress.transferred_bytes == 0 && final_progress.completed_items == 0,
              "An unrepresentable recursive total must remain unknown instead of wrapping to zero");
}

static cardio::promise<elder_terms::FileTransferConflictAction>
overwrite_local_commit_conflict(const elder_terms::FileTransferConflict &,
                                cardio::cancellation cancellation) {
  cancellation.throw_if_cancellation_requested();
  co_return elder_terms::FileTransferConflictAction::overwrite;
}

static cardio::promise<void> test_failed_local_commit_preserves_completed_file() {
  const auto root = test_root_directory() / "failed-local-commit";
  std::filesystem::create_directories(root);
  const TemporaryDirectoryCleanup cleanup{test_root_directory()};
  const auto destination = root / "retained.txt";
  write_file(destination, "completed local content");
  auto client = std::make_shared<FakeSftpClient>();
  client->add_directory("/");
  client->add_file("/retained.txt", "replacement download");
  bool removed_temporary = false;
  client->before_read_close = [&]() -> cardio::promise<void> {
    // A concurrent cleanup removes the incomplete local download before commit.
    // The old completed destination must survive the resulting move failure.
    for (const auto &entry : std::filesystem::directory_iterator(root)) {
      if (entry.path() != destination) {
        expect_true(std::filesystem::remove(entry.path()), "Concurrent cleanup must remove the pending download");
        removed_temporary = true;
      }
    }
    co_return;
  };
  elder_terms::FileTransferRequest request;
  request.direction = elder_terms::FileTransferDirection::receive;
  request.source_paths = {"/retained.txt"};
  request.destination_directory = root.string();
  request.callbacks.conflict = overwrite_local_commit_conflict;
  bool failed = false;
  try { co_await elder_terms::run_file_transfer_async(client, std::move(request), {}); }
  catch (const std::exception &) { failed = true; }
  expect_true(removed_temporary && failed, "A missing completed download must fail its final commit");
  expect_true(read_file(destination) == "completed local content",
              "Failure of the final move must not delete the existing completed destination");
}

static cardio::promise<void> test_receive_does_not_replace_a_directory_with_a_file() {
  const auto root = test_root_directory() / "directory-conflict";
  const TemporaryDirectoryCleanup cleanup{test_root_directory()};
  std::filesystem::create_directories(root / "retained");
  write_file(root / "retained/child.txt", "completed child");
  auto client = std::make_shared<FakeSftpClient>();
  client->add_directory("/");
  client->add_file("/retained", "replacement file");
  elder_terms::FileTransferRequest request;
  request.direction = elder_terms::FileTransferDirection::receive;
  request.source_paths = {"/retained"};
  request.destination_directory = root.string();
  request.callbacks.conflict = overwrite_local_commit_conflict;
  bool failed = false;
  try {
    auto pending = elder_terms::run_file_transfer_async(client, std::move(request), {});
    co_await pending;
  } catch (const std::exception &) { failed = true; }
  expect_true(failed && std::filesystem::is_directory(root / "retained") &&
              read_file(root / "retained/child.txt") == "completed child",
              "A regular-file commit must preserve a conflicting directory and its completed children");
}

static cardio::promise<void> test_job_results_and_lifecycle() {
  using namespace elder_terms;
  const auto root = test_root_directory() / "jobs";
  const TemporaryDirectoryCleanup cleanup{test_root_directory()};
  std::filesystem::create_directories(root);
  write_file(root / "existing.txt", "local");
  auto client = std::make_shared<FakeSftpClient>();
  client->add_directory("/");
  client->add_file("/existing.txt", "remote");
  client->add_file("/zero.txt", "");
  FileTransferRequest request;
  request.direction = FileTransferDirection::receive;
  request.source_paths = {"/existing.txt"};
  request.destination_directory = root.string();
  std::vector<FileTransferJobState> states;
  auto job = create_file_transfer_job("test-connection", 1, "SFTP", request,
      [&](const FileTransferJobSnapshot &snapshot) { states.push_back(snapshot.state); });
  FileTransferCallbacks callbacks;
  callbacks.conflict = [](const FileTransferConflict &, cardio::cancellation)
      -> cardio::promise<FileTransferConflictAction> { co_return FileTransferConflictAction::skip; };
  co_await run_file_transfer_job_async(job, client, callbacks, {});
  const auto &done = file_transfer_job_snapshot(job);
  expect_true(done.state == FileTransferJobState::completed && done.result &&
      done.result->succeeded_items == 0 && done.result->skipped_items == 1 &&
      done.result->unprocessed_items == 0 && done.progress.transferred_bytes == 0,
      "All-skipped jobs must not report successful copies or fabricated bytes");
  expect_true(done.started_at && done.finished_at &&
      std::find(states.begin(), states.end(), FileTransferJobState::awaiting_decision) != states.end(),
      "A job must report its decision wait and retain timestamps");
  const auto notifications = states.size();
  cancel_file_transfer_job(job);
  co_await run_file_transfer_job_async(job, client, callbacks, {});
  expect_true(states.size() == notifications && done.state == FileTransferJobState::completed,
      "Completed jobs must not run or finish twice, including late cancellation");
  request.source_paths = {"/zero.txt"};
  auto zero = create_file_transfer_job("test-connection", 2, "SFTP", request, {});
  co_await run_file_transfer_job_async(zero, client, {}, {});
  const auto &zero_done = file_transfer_job_snapshot(zero);
  expect_true(zero_done.id != done.id && zero_done.connection_generation == 2 &&
      zero_done.result->succeeded_items == 1 && zero_done.progress.total_bytes == 0,
      "Zero-byte transfers must complete and reconnects must use distinct job identities");
  auto cancelled = create_file_transfer_job("test-connection", 2, "SFTP", request, {});
  cancel_file_transfer_job(cancelled);
  co_await run_file_transfer_job_async(cancelled, client, {}, {});
  expect_true(file_transfer_job_snapshot(cancelled).state == FileTransferJobState::cancelled &&
      !file_transfer_job_snapshot(cancelled).started_at, "Waiting cancellation must never start I/O");
  request.source_paths = {"/missing.txt", "/zero.txt"};
  auto failed = create_file_transfer_job("test-connection", 2, "SFTP", request, {});
  co_await run_file_transfer_job_async(failed, client, {}, {});
  const auto &failure = file_transfer_job_snapshot(failed);
  expect_true(failure.state == FileTransferJobState::failed && failure.result->failed_items == 1 &&
      !failure.result->failures.empty() && failure.result->failures[0].source_path == "/missing.txt" &&
      !failure.result->unprocessed_items,
      "Discovery failure must retain the failed path and leave undiscovered totals unknown");
}

static cardio::promise<void> test_job_queue_and_disconnect() {
  using namespace elder_terms;
  const auto root = test_root_directory() / "queue";
  const TemporaryDirectoryCleanup cleanup{test_root_directory()};
  std::filesystem::create_directories(root);
  write_file(root / "first.txt", "existing");
  auto client = std::make_shared<FakeSftpClient>();
  client->add_directory("/");
  client->add_file("/first.txt", "first");
  client->add_file("/second.txt", "second");
  auto queue = create_file_transfer_queue("queue", {});
  attach_file_transfer_queue(queue, client, "SFTP");
  FileTransferRequest request;
  request.direction = FileTransferDirection::receive;
  request.source_paths = {"/first.txt"};
  request.destination_directory = root.string();
  const auto first = enqueue_file_transfer_job(queue, request);
  request.source_paths = {"/second.txt"};
  const auto second = enqueue_file_transfer_job(queue, request);
  const auto cancelled = enqueue_file_transfer_job(queue, request);
  cancel_file_transfer_job(cancelled);
  cardio::primitives::manually_conditional deciding{false}, decided{false};
  FileTransferCallbacks callbacks;
  callbacks.conflict = [&](const FileTransferConflict &, cardio::cancellation stop)
      -> cardio::promise<FileTransferConflictAction> {
    deciding.raise();
    co_await decided.wait(stop);
    co_return FileTransferConflictAction::overwrite;
  };
  auto running = run_file_transfer_queue_async(queue, callbacks, {});
  co_await deciding.wait();
  expect_true(file_transfer_job_snapshot(first).state == FileTransferJobState::awaiting_decision &&
      file_transfer_job_snapshot(second).state == FileTransferJobState::queued,
      "A decision wait must retain the connection slot and leave later jobs queued");
  auto browsing = load_file_transfer_queue_directory_async(queue, "/", {});
  expect_true(!browsing.is_ready(), "Browser reads must be serialized with active transfers");
  decided.raise();
  co_await running;
  co_await browsing;
  expect_true(read_file(root / "first.txt") == "first" && read_file(root / "second.txt") == "second" &&
      file_transfer_job_snapshot(cancelled).state == FileTransferJobState::cancelled,
      "Queued requests must execute sequentially without reviving a cancelled job");
  const auto stale = enqueue_file_transfer_job(queue, request);
  disconnect_file_transfer_queue(queue, false);
  expect_true(file_transfer_job_snapshot(stale).state == FileTransferJobState::failed,
      "Disconnected waiting jobs must explicitly require retry");
  attach_file_transfer_queue(queue, client, "SFTP");
  co_await run_file_transfer_queue_async(queue, {}, {});
  expect_true(file_transfer_job_snapshot(stale).state == FileTransferJobState::failed,
      "Reconnection must not automatically execute old requests");
  const auto retry = enqueue_file_transfer_job(queue, request);
  expect_true(file_transfer_job_snapshot(retry).connection_generation != file_transfer_job_snapshot(stale).connection_generation &&
      file_transfer_job_snapshot(retry).id != file_transfer_job_snapshot(stale).id,
      "Explicit retry must capture a new identity and connection generation");
  disconnect_file_transfer_queue(queue, true);
  expect_true(file_transfer_job_snapshot(retry).state == FileTransferJobState::cancelled,
      "Owner close must cancel every waiting request");
  attach_file_transfer_queue(queue, client, "SFTP");
  request.direction = FileTransferDirection::send;
  request.source_paths = {(root / "first.txt").string()};
  request.destination_directory = "/gone";
  client->add_directory("/gone");
  const auto invalidated = enqueue_file_transfer_job(queue, request);
  client->nodes.erase("/gone");
  co_await run_file_transfer_queue_async(queue, {}, {});
  expect_true(file_transfer_job_snapshot(invalidated).state == FileTransferJobState::failed &&
      !client->nodes.contains("/gone/first.txt"),
      "Execution must revalidate a queued destination before modifying the remote service");
}

static cardio::promise<void> test_job_cancellation_waits_for_cleanup() {
  using namespace elder_terms;
  const auto root = test_root_directory() / "job-cleanup";
  const TemporaryDirectoryCleanup cleanup{test_root_directory()};
  for (const bool closing : {false, true}) {
    std::filesystem::create_directories(root);
    auto client = std::make_shared<FakeSftpClient>();
    client->add_file("/first.txt", "first");
    client->add_file("/second.txt", "second");
    cardio::primitives::manually_conditional cleaning{false}, release{false};
    client->before_read_close = [&]() -> cardio::promise<void> {
      cleaning.raise();
      co_await release.wait();
    };
    auto queue = create_file_transfer_queue("cleanup", {});
    attach_file_transfer_queue(queue, client, "SFTP");
    FileTransferRequest request;
    request.direction = FileTransferDirection::receive;
    request.source_paths = {"/first.txt"};
    request.destination_directory = root.string();
    const auto first = enqueue_file_transfer_job(queue, request);
    request.source_paths = {"/second.txt"};
    const auto second = enqueue_file_transfer_job(queue, request);
    FileTransferCallbacks callbacks;
    callbacks.progress = [&](const FileTransferProgress &progress) {
      if (progress.current_path == "/first.txt" && progress.transferred_bytes)
        cancel_file_transfer_job(first);
    };
    auto running = run_file_transfer_queue_async(queue, callbacks, {});
    co_await cleaning.wait();
    if (closing) disconnect_file_transfer_queue(queue, true);
    expect_true(!running.is_ready() && client->transfer_active &&
        file_transfer_job_snapshot(first).state == FileTransferJobState::cancelling &&
        !file_transfer_job_snapshot(first).finished_at && !file_transfer_job_snapshot(second).started_at,
        "Cancellation must retain handles and the connection slot until asynchronous cleanup finishes");
    release.raise();
    co_await running;
    expect_true(!client->transfer_active && file_transfer_job_snapshot(first).state == FileTransferJobState::cancelled &&
        file_transfer_job_snapshot(first).result->temporary_paths.empty() &&
        !std::filesystem::exists(root / "first.txt"), "Canceled downloads must retire their temporary files before finishing");
    expect_true(file_transfer_job_snapshot(second).state ==
        (closing ? FileTransferJobState::cancelled : FileTransferJobState::completed),
        "Only the canceled job stops; owner close also retires waiting jobs");
    for (const auto &entry : std::filesystem::directory_iterator(root))
      expect_true(entry.path().filename() == "second.txt", "Cleanup must leave no temporary download");
    std::filesystem::remove_all(root);
  }
}

static cardio::promise<void> test_job_discovery_cancel_and_transport_failure() {
  using namespace elder_terms;
  const auto root = test_root_directory() / "job-discovery";
  const TemporaryDirectoryCleanup cleanup{test_root_directory()};
  std::filesystem::create_directories(root);
  auto client = std::make_shared<FakeSftpClient>();
  client->add_directory("/directory");
  cardio::primitives::manually_conditional entered{false}, listing{false};
  client->before_listing = [&](cardio::cancellation stop) -> cardio::promise<void> {
    entered.raise();
    co_await listing.wait(stop);
  };
  auto queue = create_file_transfer_queue("discovery", {});
  attach_file_transfer_queue(queue, client, "SFTP");
  FileTransferRequest request;
  request.direction = FileTransferDirection::receive;
  request.source_paths = {"/directory"};
  request.destination_directory = root.string();
  const auto job = enqueue_file_transfer_job(queue, request);
  auto running = run_file_transfer_queue_async(queue, {}, {});
  co_await entered.wait();
  expect_true(file_transfer_job_snapshot(job).state == FileTransferJobState::discovering &&
      !file_transfer_job_snapshot(job).progress.discovery_complete, "Recursive discovery must remain indeterminate and cancellable");
  cancel_file_transfer_job(job);
  co_await running;
  expect_true(file_transfer_job_snapshot(job).state == FileTransferJobState::cancelled &&
      !file_transfer_job_snapshot(job).result->unprocessed_items, "Canceled discovery must not invent an undiscovered item count");
  client->before_stat = [&] {
    disconnect_file_transfer_queue(queue, false);
    throw RemoteFileError("transport closed during discovery", true);
  };
  const auto failed = enqueue_file_transfer_job(queue, request);
  co_await run_file_transfer_queue_async(queue, {}, {});
  const auto &result = file_transfer_job_snapshot(failed);
  expect_true(result.state == FileTransferJobState::failed && !result.result->failures.empty() &&
      result.result->failures.front().message == "transport closed during discovery",
      "Connection loss must preserve the transport failure even when its observer also cancels the owner");
  attach_file_transfer_queue(queue, client, "SFTP");
  client->before_stat = {};
  client->add_directory("/destination");
  write_file(root / "upload.txt", "upload");
  client->before_write = [&] {
    disconnect_file_transfer_queue(queue, false);
    throw RemoteFileError("transport closed during upload", true);
  };
  client->before_remove = [] { throw RemoteFileError("cleanup connection unavailable", true); };
  request.direction = FileTransferDirection::send;
  request.source_paths = {(root / "upload.txt").string()};
  request.destination_directory = "/destination";
  const auto cleanup_failed = enqueue_file_transfer_job(queue, request);
  co_await run_file_transfer_queue_async(queue, {}, {});
  const auto &cleanup_result = *file_transfer_job_snapshot(cleanup_failed).result;
  expect_true(cleanup_result.outcome == FileTransferOutcome::failed && cleanup_result.temporary_paths.size() == 1 &&
      cleanup_result.failures.front().message.find("transport closed during upload") != std::string::npos &&
      cleanup_result.failures.front().message.find("cleanup connection unavailable") != std::string::npos,
      "Cleanup failure must preserve the original transport error and the remaining path after disconnect cancellation");
}

static cardio::promise<void> test_job_partial_failure_and_commit_boundary() {
  using namespace elder_terms;
  const auto root = test_root_directory() / "partial-job";
  const TemporaryDirectoryCleanup cleanup{test_root_directory()};
  std::filesystem::create_directories(root);
  auto client = std::make_shared<FakeSftpClient>();
  client->add_file("/first.txt", "first");
  client->add_file("/second.txt", "second");
  client->add_file("/third.txt", "third");
  unsigned closed = 0;
  client->before_read_close = [&]() -> cardio::promise<void> {
    if (++closed >= 2) throw std::runtime_error("second source failed on close");
    co_return;
  };
  FileTransferRequest request;
  request.direction = FileTransferDirection::receive;
  request.source_paths = {"/first.txt", "/second.txt", "/third.txt"};
  request.destination_directory = root.string();
  auto job = create_file_transfer_job("partial", 1, "SFTP", request, {});
  co_await run_file_transfer_job_async(job, client, {}, {});
  const auto &result = *file_transfer_job_snapshot(job).result;
  expect_true(result.outcome == FileTransferOutcome::failed && result.succeeded_items == 1 &&
      result.failed_items == 1 && result.skipped_items == 0 && result.unprocessed_items == 1 &&
      result.failures.front().source_path == "/second.txt" && read_file(root / "first.txt") == "first" &&
      !std::filesystem::exists(root / "second.txt") && !std::filesystem::exists(root / "third.txt"),
      "Partial failure must retain committed results and distinguish the failed item from unprocessed items");
  client->before_read_close = {};
  request.source_paths = {"/third.txt"};
  auto late = create_file_transfer_job("partial", 1, "SFTP", request, {});
  FileTransferCallbacks callbacks;
  callbacks.progress = [&](const FileTransferProgress &progress) {
    if (progress.completed_items == 1) cancel_file_transfer_job(late);
  };
  co_await run_file_transfer_job_async(late, client, callbacks, {});
  expect_true(file_transfer_job_snapshot(late).state == FileTransferJobState::completed &&
      file_transfer_job_snapshot(late).result->succeeded_items == 1 && read_file(root / "third.txt") == "third",
      "Cancellation after the last commit must not relabel a completed copy as canceled");
}

static cardio::promise<void> test_send_uses_client_name_boundary() {
  using namespace elder_terms;
  const auto root = test_root_directory() / "encoded-send";
  const TemporaryDirectoryCleanup cleanup{test_root_directory()};
  std::filesystem::create_directories(root / "日本語/deep");
  write_file(root / "日本語/deep/payload.txt", "unchanged content");
  auto client = std::make_shared<FakeSftpClient>();
  client->add_directory("/destination");
  client->encoded_child_names = true;
  FileTransferRequest request;
  request.direction = FileTransferDirection::send;
  request.source_paths = {(root / "日本語").string()};
  request.destination_directory = "/destination";
  auto observed = create_activity_file_client(client, [](ActivityIndicatorId) {}, {});
  co_await run_file_transfer_async(observed, request, {});
  expect_true(client->nodes.contains("/destination/id-日本語/id-deep/id-payload.txt"),
      "Every new child must be encoded by the receiving client from its parent identity and UTF-8 name");
}

static cardio::promise<void> test_receive_preserves_remote_identity() {
  using namespace elder_terms;
  const auto root = test_root_directory() / "identities";
  const TemporaryDirectoryCleanup cleanup{test_root_directory()};
  std::filesystem::create_directories(root);
  auto client = std::make_shared<FakeSftpClient>();
  client->add_directory("/opaque-root");
  client->nodes.at("/opaque-root").attributes.name = "日本語";
  client->add_directory("/opaque-root/opaque-directory");
  client->nodes.at("/opaque-root/opaque-directory").attributes.name = "資料";
  client->add_file("/opaque-root/opaque-directory/opaque-file", "original bytes\n");
  client->nodes.at("/opaque-root/opaque-directory/opaque-file").attributes.name = "名前%?#.txt";
  FileTransferRequest request;
  request.direction = FileTransferDirection::receive;
  request.source_paths = {"/opaque-root"};
  request.destination_directory = root.string();
  co_await run_file_transfer_async(client, request, {});
  expect_true(read_file(root / "日本語/資料/名前%?#.txt") == "original bytes\n",
      "Receive must retain the listed remote identity and use the display name only for local naming");
  expect_true(!std::filesystem::exists(root / "opaque-root"),
      "Remote identifiers must never become local filenames");
}

static cardio::promise<void> test_delete_preserves_remote_identity() {
  using namespace elder_terms;
  auto client = std::make_shared<FakeSftpClient>();
  client->add_directory("/opaque-root");
  client->add_file("/opaque-root/opaque-one", "one");
  client->add_file("/opaque-root/opaque-two", "two");
  // Two different byte strings may decode to the same display name.
  client->nodes.at("/opaque-root/opaque-one").attributes.name = "同じ名前";
  client->nodes.at("/opaque-root/opaque-two").attributes.name = "同じ名前";
  std::vector<std::string> paths{"/opaque-root"};
  co_await delete_file_transfer_items_async(client, FileTransferEndpoint::remote, paths, {});
  expect_true(client->nodes.empty(), "Recursive removal must operate on both distinct listed identities");
}

static cardio::promise<void> test_receive_ambiguous_and_undecodable_names() {
  using namespace elder_terms;
  const auto root = test_root_directory() / "ambiguous-receive";
  const TemporaryDirectoryCleanup cleanup{test_root_directory()};
  std::filesystem::create_directories(root / "source");
  write_file(root / "source/same.txt", "keep existing content");
  auto client = std::make_shared<FakeSftpClient>();
  client->add_directory("/source");
  client->add_file("/source/first-identity", "first");
  client->add_file("/source/second-identity", "second");
  client->add_file("/source/invalid-identity", "invalid bytes");
  client->add_file("/source/good.txt", "good");
  client->nodes.at("/source/first-identity").attributes.name = "same.txt";
  client->nodes.at("/source/second-identity").attributes.name = "same.txt";
  client->nodes.at("/source/invalid-identity").attributes.name = "[undecodable: FF]";
  client->undecodable_names.insert("/source/invalid-identity");
  FileTransferRequest request;
  request.direction = FileTransferDirection::receive;
  request.source_paths = {"/source"};
  request.destination_directory = root.string();
  request.callbacks.conflict = [](const FileTransferConflict &, cardio::cancellation) -> cardio::promise<FileTransferConflictAction> {
    co_return FileTransferConflictAction::overwrite;
  };
  request.callbacks.failure = [](const FileTransferFailure &, cardio::cancellation) -> cardio::promise<FileTransferFailureAction> {
    co_return FileTransferFailureAction::skip;
  };
  auto observed = create_activity_file_client(client, [](ActivityIndicatorId) {}, {});
  auto result = co_await run_file_transfer_async(observed, request, {});
  expect_true(read_file(root / "source/same.txt") == "keep existing content" &&
      !std::filesystem::exists(root / "source/[undecodable: FF]") && read_file(root / "source/good.txt") == "good",
      "Ambiguous and undecodable local names must never create or replace files, even with overwrite selected");
  expect_true(result.skipped_items == 3 && result.succeeded_items == 2 && result.failures.size() == 3,
      "Skipping unsafe names must retain each original item's failure and copy unaffected siblings");
  std::set<std::string> failed;
  for (const auto &item : result.failures) failed.insert(item.source_path);
  expect_true(failed == std::set<std::string>{"/source/first-identity", "/source/second-identity", "/source/invalid-identity"},
      "Name failures must identify each original remote item");
  request.source_paths = {"/source/first-identity", "/source/second-identity", "/source/invalid-identity"};
  request.destination_directory = (root / "source").string();
  result = co_await run_file_transfer_async(observed, request, {});
  expect_true(result.skipped_items == 3 && result.succeeded_items == 0 && read_file(root / "source/same.txt") == "keep existing content",
      "Selected roots must apply the same collision and undecodable-name rules as recursive children");
}

static cardio::promise<void> test_link_target_conversion_boundary() {
  using namespace elder_terms;
  const auto root = test_root_directory() / "link-conversion";
  const TemporaryDirectoryCleanup cleanup{test_root_directory()};
  std::filesystem::create_directories(root / "source");
  write_file(root / "source/日本語", "linked content");
  std::filesystem::create_symlink("日本語", root / "source/link");
  std::filesystem::create_symlink("missing", root / "source/dangling");
  auto client = std::make_shared<FakeSftpClient>();
  client->encoded_link_targets = true;
  client->add_directory("/destination");
  FileTransferRequest request;
  request.direction = FileTransferDirection::send;
  request.source_paths = {(root / "source").string()};
  request.destination_directory = "/destination";
  auto observed = create_activity_file_client(client, [](ActivityIndicatorId) {}, {});
  co_await run_file_transfer_async(observed, request, {});
  expect_true(client->nodes.at("/destination/source/link").link_target == "wire:日本語",
      "New remote links must convert local targets at the remote client's boundary");
  client->nodes.at("/destination/source/link").attributes.modification_time_unix_seconds = 123;
  client->nodes.at("/destination/source/dangling").attributes.modification_time_unix_seconds = 456;
  std::filesystem::create_directory(root / "received");
  request.direction = FileTransferDirection::receive;
  request.source_paths = {"/destination/source"};
  request.destination_directory = (root / "received").string();
  co_await run_file_transfer_async(observed, request, {});
  expect_true(std::filesystem::read_symlink(root / "received/source/link") == "日本語" &&
      read_file(root / "received/source/link") == "linked content", "Received links must still resolve to the copied file");
  struct stat link_attributes{}, dangling_attributes{}, target_attributes{};
  expect_true(lstat((root / "received/source/link").c_str(), &link_attributes) == 0 &&
      lstat((root / "received/source/dangling").c_str(), &dangling_attributes) == 0 &&
      stat((root / "received/source/日本語").c_str(), &target_attributes) == 0 &&
      link_attributes.st_mtime == 123 && dangling_attributes.st_mtime == 456 &&
      target_attributes.st_mtime == *client->nodes.at("/destination/source/日本語").attributes.modification_time_unix_seconds,
      "Receiving link timestamps must work for dangling links and preserve the target's timestamps");
}

static cardio::promise<void> test_receive_link_timestamp_range() {
  using namespace elder_terms;
  const auto root = test_root_directory() / "link-times";
  const TemporaryDirectoryCleanup cleanup{test_root_directory()};
  std::vector<std::int64_t> timestamps{-1, 2'147'483'647, 2'147'483'648};
  if (sizeof(std::time_t) == 4) timestamps.push_back(-2'147'483'649);
  for (const auto seconds : timestamps) {
    const auto destination = root / std::to_string(seconds);
    std::filesystem::create_directories(destination);
    write_file(destination / "target", "unchanged");
    set_file_times(destination / "target", 1'700'000'000);
    auto client = std::make_shared<FakeSftpClient>();
    client->add_link("/link", "target", seconds);
    FileTransferRequest request;
    request.direction = FileTransferDirection::receive;
    request.source_paths = {"/link"};
    request.destination_directory = destination.string();
    bool out_of_range = false;
    try {
      const auto result = co_await run_file_transfer_async(client, request, {});
      expect_true(result.succeeded_items == 1,
          "Representable symbolic link times must allow receiving the link");
    } catch (const std::out_of_range &) {
      out_of_range = true;
    }
    const bool outside_local_range = sizeof(std::time_t) == 4 &&
        (seconds < -2'147'483'648 || seconds > 2'147'483'647);
    expect_true(out_of_range == outside_local_range,
        "Unrepresentable symbolic link times must report an error instead of wrapping");
    if (!out_of_range) {
      const auto attributes = lstat_path(destination / "link");
      expect_true(attributes.st_atime == seconds && attributes.st_mtime == seconds,
          "Received symbolic links must preserve representable access and modification times");
    }
    expect_true(lstat_path(destination / "target").st_mtime == 1'700'000'000,
        "Receiving link timestamps must leave the target's timestamp unchanged");
  }
}

static cardio::promise<void> test_rejects_parallel_bulk_transfer() {
  auto client = std::make_shared<FakeSftpClient>();
  client->add_directory("/");
  expect_true(client->try_begin_transfer(),
              "failed to reserve fake transfer slot");
  // Name the request before co_await for GCC 12 coroutine compatibility.
  auto request = elder_terms::FileTransferRequest{
      .direction = elder_terms::FileTransferDirection::receive,
      .source_paths = {"/missing"},
      .destination_directory = "/tmp",
      .callbacks = {},
  };
  bool rejected = false;
  try {
    co_await elder_terms::run_file_transfer_async(
        client, std::move(request), {});
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
  auto task_body = [&]() -> cardio::promise<void> {
    try {
      std::string failures;
      for (const auto &[name, run] : std::vector<std::pair<const char *, cardio::promise<void>(*)()>>{
          {"test_receive_preserves_remote_identity", test_receive_preserves_remote_identity},
          {"test_send_uses_client_name_boundary", test_send_uses_client_name_boundary},
          {"test_receive_ambiguous_and_undecodable_names", test_receive_ambiguous_and_undecodable_names},
          {"test_link_target_conversion_boundary", test_link_target_conversion_boundary},
          {"test_receive_link_timestamp_range", test_receive_link_timestamp_range},
          {"test_delete_preserves_remote_identity", test_delete_preserves_remote_identity},
          {"test_job_results_and_lifecycle", test_job_results_and_lifecycle},
          {"test_job_partial_failure_and_commit_boundary", test_job_partial_failure_and_commit_boundary},
          {"test_job_queue_and_disconnect", test_job_queue_and_disconnect},
          {"test_job_cancellation_waits_for_cleanup", test_job_cancellation_waits_for_cleanup},
          {"test_job_discovery_cancel_and_transport_failure", test_job_discovery_cancel_and_transport_failure},
          {"test_receive_preserves_unknown_aggregate_size", test_receive_preserves_unknown_aggregate_size},
          {"test_receive_reports_unrepresentable_total_as_unknown", test_receive_reports_unrepresentable_total_as_unknown},
          {"test_receive_does_not_replace_a_directory_with_a_file", test_receive_does_not_replace_a_directory_with_a_file},
          {"test_changed_source_does_not_commit_partial_upload", test_changed_source_does_not_commit_partial_upload},
          {"test_changed_remote_size_preserves_local_destination", test_changed_remote_size_preserves_local_destination},
          {"test_failed_local_commit_preserves_completed_file", test_failed_local_commit_preserves_completed_file},
          {"test_recursive_send_preserves_links_metadata_and_recovers", test_recursive_send_preserves_links_metadata_and_recovers},
          {"test_recursive_receive_preserves_links_and_metadata", test_recursive_receive_preserves_links_and_metadata},
          {"test_cancellation_removes_remote_temporary_file", test_cancellation_removes_remote_temporary_file},
          {"test_rejects_parallel_bulk_transfer", test_rejects_parallel_bulk_transfer},
      }) {
        try { co_await run(); }
        catch (const std::exception &failure) { failures += std::string(name) + ": " + failure.what() + "\n"; }
      }
      if (!failures.empty()) throw std::runtime_error(failures);
    } catch (...) {
      error = std::current_exception();
    }
    dispatcher_group.shutdown();
  };
  auto task = task_body();

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
