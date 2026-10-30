// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "../../src/smb/smb-client.h"
#include "../../src/file-transfer/file-transfer-engine.h"
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>

static void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

static cardio::promise<std::string> read_async(
    const std::shared_ptr<elder_terms::RemoteFileClient> &client, std::string path) {
  auto reader = std::move(co_await client->open_read_async(std::move(path), {}));
  std::string result;
  std::array<std::byte, 65536> bytes{};
  for (;;) {
    const auto count = co_await reader->read_async(bytes, {});
    if (!count) break;
    result.append(reinterpret_cast<const char *>(bytes.data()), count);
  }
  co_await reader->close_async({});
  co_return result;
}

static cardio::promise<void> write_async(
    const std::shared_ptr<elder_terms::RemoteFileClient> &client,
    std::string path, std::string text) {
  auto writer = std::move(co_await client->open_write_async(std::move(path), text.size(), {}, {}));
  co_await writer->write_all_async(std::as_bytes(std::span(text)), {});
  co_await writer->close_async({});
  require(writer->creation_state() == elder_terms::RemoteFileCreationState::created,
          "Successful exclusive creation must establish ownership");
}

static cardio::promise<void> transfer_async(
    const std::shared_ptr<elder_terms::RemoteFileClient> &client,
    elder_terms::FileTransferDirection direction, std::string source,
    std::string destination, elder_terms::FileTransferConflictAction conflict,
    bool cancel_after_chunk) {
  elder_terms::FileTransferRequest request;
  request.direction = direction;
  request.source_paths.push_back(std::move(source));
  request.destination_directory = std::move(destination);
  request.callbacks.conflict = [conflict](const auto &, cardio::cancellation)
      -> cardio::promise<elder_terms::FileTransferConflictAction> { co_return conflict; };
  cardio::cancellation_source stopping;
  bool progressed = false;
  request.callbacks.progress = [&](const auto &progress) {
    progressed |= progress.transferred_bytes > 0;
    if (cancel_after_chunk && progressed) (void)stopping.cancel();
  };
  bool cancelled = false;
  try { co_await elder_terms::run_file_transfer_async(client, std::move(request), stopping.get_cancellation()); }
  catch (const cardio::canceled_exception &) { cancelled = true; }
  require(cancelled == cancel_after_chunk, "Transfer cancellation result differs");
  require(progressed, "Transfer did not publish completed bytes");
}

struct LocalFixture {
  std::filesystem::path path;
  ~LocalFixture() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

static cardio::promise<void> exercise_async(int argc, char **argv) {
  using namespace elder_terms;
  require(argc == 5 || argc == 6, "Usage: smb-transfer-test host share user port [readonly]");
  SmbClientOpenOptions options;
  options.connection.address = argv[1];
  options.connection.share = argv[2];
  options.connection.username = argv[3];
  options.connection.port = std::stoi(argv[4]);
  options.password = g_getenv("ELDER_SMB_PASSWORD");
  auto client = co_await open_smb_client_async(std::move(options), {});
  const auto capabilities = client->capabilities();
  require(capabilities.write_operations && capabilities.upload_commit && capabilities.modification_time,
          "SMB upload, conditional commit and modification time must be supported");
  require(!capabilities.symbolic_links && !capabilities.permissions,
          "Unsupported SMB link and POSIX operations must not be advertised");
  auto *uuid = g_uuid_string_random();
  const std::string root = "/elder-transfer-" + std::string(uuid);
  g_free(uuid);
  if (std::string(argv[2]) == "readonly" || (argc == 6 && std::string(argv[5]) == "readonly")) {
    bool denied = false;
    try { co_await client->make_directory_async(root, {}, {}); }
    catch (const RemoteFileError &error) { denied = !error.connection_lost; }
    require(denied, "Read-only share accepted a directory creation");
    require(!co_await client->lstat_async(root, {}), "Denied creation left an item behind");
    co_await stop_smb_client_async(client);
    std::cout << "PASS SMB client read-only access refusal\n";
    co_return;
  }
  co_await client->make_directory_async(root, {}, {});
  co_await write_async(client, root + "/destination.txt", "original");
  bool collision = false;
  try { co_await write_async(client, root + "/destination.txt", "foreign"); }
  catch (const RemoteFileError &) { collision = true; }
  require(collision, "Exclusive creation replaced another file");
  require(co_await read_async(client, root + "/destination.txt") == "original", "Foreign content changed");
  co_await write_async(client, root + "/temporary.txt", "replacement");
  collision = false;
  try { co_await client->commit_upload_async(root + "/temporary.txt", root + "/destination.txt", false, {}); }
  catch (const RemoteFileError &) { collision = true; }
  require(collision, "No-replace commit accepted an existing destination");
  require(co_await read_async(client, root + "/destination.txt") == "original", "Failed commit removed destination");
  co_await client->commit_upload_async(root + "/temporary.txt", root + "/destination.txt", true, {});
  require(co_await read_async(client, root + "/destination.txt") == "replacement", "Replacement content differs");
  require(co_await read_async(client, root + "/DESTINATION.TXT") == "replacement", "Case-insensitive lookup differs");
  require(!co_await client->lstat_async(root + "/temporary.txt", {}), "Commit left the temporary path");
  RemoteFileAttributes attributes;
  attributes.modification_time_unix_seconds = 1700000000;
  co_await client->set_attributes_async(root + "/destination.txt", attributes, {});
  require((co_await client->lstat_async(root + "/destination.txt", {}))->modification_time_unix_seconds == 1700000000,
          "SMB modification time differs");
  co_await write_async(client, root + "/zero.bin", "");
  require((co_await client->lstat_async(root + "/zero.bin", {}))->size == 0, "Empty upload size differs");
  collision = false;
  try { co_await client->rename_async(root + "/zero.bin", root + "/destination.txt", {}); }
  catch (const RemoteFileError &) { collision = true; }
  require(collision, "Ordinary rename replaced an existing destination");
  co_await client->rename_async(root + "/zero.bin", root + "/日本語 space.bin", {});

  auto short_writer = std::move(co_await client->open_write_async(root + "/short.bin", 3, {}, {}));
  bool short_rejected = false;
  try { co_await short_writer->close_async({}); }
  catch (const RemoteFileError &) { short_rejected = true; }
  require(short_rejected, "Incomplete upload was reported successful");
  co_await client->remove_file_async(root + "/short.bin", {});
  short_writer.reset();
  auto overlong = std::move(co_await client->open_write_async(root + "/overlong.bin", 1, {}, {}));
  const std::string excess = "too long";
  bool excess_rejected = false;
  try { co_await overlong->write_all_async(std::as_bytes(std::span(excess)), {}); }
  catch (const RemoteFileError &) { excess_rejected = true; }
  require(excess_rejected, "Overlong upload was accepted");
  require(overlong->creation_state() == RemoteFileCreationState::not_created,
          "Rejected input must not claim an uncreated path");
  overlong.reset();
  require(!co_await client->lstat_async(root + "/overlong.bin", {}), "Rejected input created a file");

  char local_pattern[] = "/tmp/elder-smb-transfer-XXXXXX";
  const auto *created = ::mkdtemp(local_pattern);
  require(created != nullptr, "Could not create local fixtures");
  const LocalFixture local{created};
  const auto source = local.path / "source";
  std::filesystem::create_directories(source / "nested/empty");
  std::filesystem::create_directory(local.path / "download");
  std::string content(40 * 1024 * 1024 + 13, '\0');
  for (std::size_t index = 0; index < content.size(); ++index)
    content[index] = static_cast<char>((index * 31 + 7) % 256);
  std::ofstream(source / "large.bin", std::ios::binary).write(content.data(), content.size());
  std::ofstream(source / "nested/資料 #+%.txt", std::ios::binary) << "nested contents\n";
  std::ofstream(source / "zero.txt", std::ios::binary);
  co_await transfer_async(client, FileTransferDirection::send, source.string(), root,
      FileTransferConflictAction::overwrite, false);
  co_await transfer_async(client, FileTransferDirection::receive, root + "/source", (local.path / "download").string(),
      FileTransferConflictAction::overwrite, false);
  for (const auto &relative : {"large.bin", "nested/資料 #+%.txt", "zero.txt"}) {
    std::ifstream expected(source / relative, std::ios::binary);
    std::ifstream actual(local.path / "download/source" / relative, std::ios::binary);
    require(expected.good() && actual.good(), "Round-trip file is missing");
    require(std::string(std::istreambuf_iterator<char>(expected), {}) ==
            std::string(std::istreambuf_iterator<char>(actual), {}), "Round-trip bytes differ");
  }
  require(std::filesystem::is_empty(local.path / "download/source/nested/empty"), "Empty directory was lost");
  std::ofstream(source / "large.bin", std::ios::binary) << "changed";
  co_await transfer_async(client, FileTransferDirection::send, (source / "large.bin").string(), root + "/source",
      FileTransferConflictAction::skip, false);
  require((co_await client->lstat_async(root + "/source/large.bin", {}))->size == content.size(), "Skip replaced remote content");
  co_await transfer_async(client, FileTransferDirection::send, (source / "large.bin").string(), root + "/source",
      FileTransferConflictAction::overwrite, false);
  require(co_await read_async(client, root + "/source/large.bin") == "changed", "Overwrite did not commit new content");
  std::ofstream(source / "cancel.bin", std::ios::binary).write(content.data(), content.size());
  co_await transfer_async(client, FileTransferDirection::send, (source / "cancel.bin").string(), root,
      FileTransferConflictAction::overwrite, true);
  const auto after_cancel = co_await client->load_directory_async(root, {});
  for (const auto &entry : after_cancel.entries)
    require(entry.name.find("cancel.bin") == std::string::npos, "Cancelled upload left its owned temporary file");
  co_await client->rename_async(root + "/source", root + "/renamed", {});
  require((co_await client->load_directory_async(root + "/renamed", {})).entries.size() == 3,
          "Renamed tree content differs");
  std::vector<std::string> deletion{root};
  auto deleting = delete_file_transfer_items_async(client, FileTransferEndpoint::remote, std::move(deletion), {});
  co_await deleting;
  require(!co_await client->lstat_async(root, {}), "Recursive deletion left the test directory");
  co_await stop_smb_client_async(client);
  std::cout << "PASS SMB client recursive upload/download/commit/metadata/cancellation/delete\n";
}

static cardio::promise<void> run_async(cardio::dispatcher_group_glib &group,
    int argc, char **argv, std::exception_ptr &failure) {
  try { co_await exercise_async(argc, argv); }
  catch (...) { failure = std::current_exception(); }
  group.shutdown();
}

int main(int argc, char **argv) {
  cardio::dispatcher_group_glib group;
  cardio::dispatcher_host_glib_auto dispatcher(group);
  std::exception_ptr failure;
  auto pending = run_async(group, argc, argv, failure);
  dispatcher.park();
  if (failure) {
    try { std::rethrow_exception(failure); }
    catch (const std::exception &error) { std::cerr << error.what() << '\n'; }
    return 1;
  }
  return 0;
}
