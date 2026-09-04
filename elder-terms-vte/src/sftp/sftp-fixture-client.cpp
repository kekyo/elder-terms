#include "sftp-fixture-client.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace elder_terms {

struct SftpFixtureNode {
  RemoteFileAttributes attributes;
  std::vector<std::byte> content;
  std::string link_target;
};

static std::string normalize_fixture_path(
    const std::string &path) {
  std::string result =
      std::filesystem::path(path).lexically_normal().generic_string();
  return result.empty() ? "." : result;
}

static std::string fixture_path_name(const std::string &path) {
  return std::filesystem::path(path).filename().generic_string();
}

static std::string fixture_parent_path(const std::string &path) {
  return std::filesystem::path(path).parent_path().generic_string();
}

static std::vector<std::byte> fixture_bytes(
    const std::string &value) {
  const auto *begin =
      reinterpret_cast<const std::byte *>(value.data());
  return std::vector<std::byte>(begin, begin + value.size());
}

class SftpFixtureClient;

class SftpFixtureReader final : public RemoteFileReader {
private:
  std::vector<std::byte> content;
  std::size_t offset = 0;
  bool closed = false;

public:
  explicit SftpFixtureReader(std::vector<std::byte> content)
      : content(std::move(content)) {
  }

  cardio::promise<std::size_t>
  read_async(std::span<std::byte> buffer,
             cardio::cancellation cancellation) override {
    cancellation.throw_if_cancellation_requested();
    if (closed) {
      throw std::runtime_error("Fixture SFTP reader is closed");
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

class SftpFixtureWriter final : public RemoteFileWriter {
private:
  std::shared_ptr<SftpFixtureClient> client;
  std::string path;
  bool closed = false;

public:
  SftpFixtureWriter(std::shared_ptr<SftpFixtureClient> client,
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

class SftpFixtureClient final
    : public RemoteFileClient,
      public std::enable_shared_from_this<SftpFixtureClient> {
public:
  std::map<std::string, SftpFixtureNode> nodes;
  cardio::primitives::manually_conditional write_gate{false};
  bool pause_writes = false;
  bool transfer_active = false;

  explicit SftpFixtureClient(bool pause_writes)
      : pause_writes(pause_writes) {
  }

  void add_directory(const std::string &path,
                     std::uint32_t permissions = 0755) {
    const std::string normalized = normalize_fixture_path(path);
    nodes[normalized] = {
        .attributes =
            {
                .name = fixture_path_name(normalized),
                .path = normalized,
                .type = RemoteFileType::directory,
                .size = 0,
                .permissions = permissions,
                .access_time_unix_seconds = 1'700'000'000,
                .modification_time_unix_seconds = 1'700'000'000,
            },
        .content = {},
        .link_target = {},
    };
  }

  void add_file(const std::string &path, const std::string &content,
                std::uint32_t permissions = 0644) {
    const std::string normalized = normalize_fixture_path(path);
    nodes[normalized] = {
        .attributes =
            {
                .name = fixture_path_name(normalized),
                .path = normalized,
                .type = RemoteFileType::regular,
                .size = content.size(),
                .permissions = permissions,
                .access_time_unix_seconds = 1'700'000'000,
                .modification_time_unix_seconds = 1'700'000'000,
            },
        .content = fixture_bytes(content),
        .link_target = {},
    };
  }

  void add_link(const std::string &path, const std::string &target) {
    const std::string normalized = normalize_fixture_path(path);
    nodes[normalized] = {
        .attributes =
            {
                .name = fixture_path_name(normalized),
                .path = normalized,
                .type = RemoteFileType::symbolic_link,
                .size = target.size(),
                .permissions = 0777,
                .access_time_unix_seconds = 1'700'000'000,
                .modification_time_unix_seconds = 1'700'000'000,
            },
        .content = {},
        .link_target = target,
    };
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
  load_directory_async(std::string path,
                       cardio::cancellation cancellation) override {
    cancellation.throw_if_cancellation_requested();
    const std::string normalized = normalize_fixture_path(path);
    const auto iterator = nodes.find(normalized);
    if (iterator == nodes.end() ||
        iterator->second.attributes.type != RemoteFileType::directory) {
      throw std::runtime_error(
          "Fixture remote directory does not exist");
    }
    RemoteDirectorySnapshot result{
        .canonical_path = normalized,
        .entries = {},
    };
    for (const auto &[candidate, node] : nodes) {
      if (candidate != normalized &&
          fixture_parent_path(candidate) == normalized) {
        result.entries.push_back(node.attributes);
      }
    }
    co_return result;
  }

  cardio::promise<std::optional<RemoteFileAttributes>>
  lstat_async(std::string path,
              cardio::cancellation cancellation) override {
    cancellation.throw_if_cancellation_requested();
    const auto iterator =
        nodes.find(normalize_fixture_path(path));
    if (iterator == nodes.end()) {
      co_return std::nullopt;
    }
    co_return iterator->second.attributes;
  }

  cardio::promise<std::string>
  read_link_async(std::string path,
                  cardio::cancellation cancellation) override {
    cancellation.throw_if_cancellation_requested();
    const auto iterator =
        nodes.find(normalize_fixture_path(path));
    if (iterator == nodes.end() ||
        iterator->second.attributes.type !=
            RemoteFileType::symbolic_link) {
      throw std::runtime_error("Fixture remote link does not exist");
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
    const std::string normalized = normalize_fixture_path(path);
    const auto iterator = nodes.find(normalized);
    if (iterator == nodes.end() ||
        iterator->second.attributes.type == RemoteFileType::directory) {
      throw std::runtime_error("Fixture remote file does not exist");
    }
    nodes.erase(iterator);
    co_return;
  }

  cardio::promise<void>
  remove_directory_async(std::string path,
                         cardio::cancellation cancellation) override {
    cancellation.throw_if_cancellation_requested();
    const std::string normalized = normalize_fixture_path(path);
    const auto child = std::find_if(
        nodes.begin(), nodes.end(),
        [&normalized](const auto &entry) {
          return entry.first != normalized &&
                 fixture_parent_path(entry.first) == normalized;
        });
    if (child != nodes.end()) {
      throw std::runtime_error(
          "Fixture remote directory is not empty");
    }
    const auto iterator = nodes.find(normalized);
    if (iterator == nodes.end() ||
        iterator->second.attributes.type != RemoteFileType::directory) {
      throw std::runtime_error(
          "Fixture remote directory does not exist");
    }
    nodes.erase(iterator);
    co_return;
  }

  cardio::promise<void>
  rename_async(std::string source_path, std::string destination_path,
               cardio::cancellation cancellation) override {
    cancellation.throw_if_cancellation_requested();
    const std::string source =
        normalize_fixture_path(source_path);
    const std::string destination =
        normalize_fixture_path(destination_path);
    if (!nodes.contains(source) || nodes.contains(destination)) {
      throw std::runtime_error("Fixture remote rename failed");
    }
    const std::string descendant_prefix = source + "/";
    std::vector<std::pair<std::string, SftpFixtureNode>> renamed;
    for (const auto &[candidate, node] : nodes) {
      if (candidate != source &&
          !candidate.starts_with(descendant_prefix)) {
        continue;
      }
      const std::string renamed_path =
          destination + candidate.substr(source.size());
      if (nodes.contains(renamed_path) &&
          renamed_path != candidate &&
          !renamed_path.starts_with(descendant_prefix)) {
        throw std::runtime_error("Fixture remote rename failed");
      }
      SftpFixtureNode renamed_node = node;
      renamed_node.attributes.name = fixture_path_name(renamed_path);
      renamed_node.attributes.path = renamed_path;
      renamed.emplace_back(renamed_path, std::move(renamed_node));
    }
    for (auto iterator = nodes.begin(); iterator != nodes.end();) {
      if (iterator->first == source ||
          iterator->first.starts_with(descendant_prefix)) {
        iterator = nodes.erase(iterator);
      } else {
        ++iterator;
      }
    }
    for (auto &[renamed_path, node] : renamed) {
      nodes.emplace(std::move(renamed_path), std::move(node));
    }
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
  set_attributes_async(
      std::string path, RemoteFileAttributes attributes,
      cardio::cancellation cancellation) override {
    cancellation.throw_if_cancellation_requested();
    const auto iterator =
        nodes.find(normalize_fixture_path(path));
    if (iterator == nodes.end()) {
      throw std::runtime_error("Fixture remote item does not exist");
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

  cardio::promise<std::unique_ptr<RemoteFileReader>>
  open_read_async(std::string path,
                  cardio::cancellation cancellation) override {
    cancellation.throw_if_cancellation_requested();
    const auto iterator =
        nodes.find(normalize_fixture_path(path));
    if (iterator == nodes.end() ||
        iterator->second.attributes.type != RemoteFileType::regular) {
      throw std::runtime_error("Fixture remote file does not exist");
    }
    co_return std::make_unique<SftpFixtureReader>(
        iterator->second.content);
  }

  cardio::promise<std::unique_ptr<RemoteFileWriter>>
  open_write_async(std::string path,
                   std::optional<std::uint32_t> permissions,
                   cardio::cancellation cancellation) override {
    cancellation.throw_if_cancellation_requested();
    const std::string normalized = normalize_fixture_path(path);
    add_file(normalized, "", permissions.value_or(0600U));
    co_return std::make_unique<SftpFixtureWriter>(
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
SftpFixtureWriter::write_all_async(
    std::span<const std::byte> buffer,
    cardio::cancellation cancellation) {
  cancellation.throw_if_cancellation_requested();
  if (closed) {
    throw std::runtime_error("Fixture SFTP writer is closed");
  }
  if (client->pause_writes) {
    co_await client->write_gate.wait(cancellation);
  }
  SftpFixtureNode &node = client->nodes.at(path);
  node.content.insert(node.content.end(), buffer.begin(), buffer.end());
  node.attributes.size = node.content.size();
}

std::shared_ptr<RemoteFileClient>
create_sftp_fixture_client(bool pause_writes) {
  auto client =
      std::make_shared<SftpFixtureClient>(pause_writes);
  client->add_directory("/");
  client->add_directory("/remote");
  client->add_directory("/remote/archive");
  client->add_file(
      "/remote/archive/long-remote-filename-that-keeps-extending-until-the-"
      "name-column-needs-more-space-than-the-file-transfer-pane-allows.log",
      "long remote file\n");
  client->add_file("/remote/archive/old.log", "old remote log\n");
  client->add_file("/remote/readme.txt", "hello from remote\n");
  client->add_link("/remote/latest", "readme.txt");
  return client;
}

} // namespace elder_terms
