// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "../../src/nfs/nfs-client.h"
#include "../../src/nfs/nfs-name.h"
#include "../../src/file-transfer/file-transfer-engine.h"
#include "../../src/file-transfer/activity-file-client.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <unistd.h>

using namespace elder_terms;

static void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

static cardio::promise<void> test_client(const std::string &encoding) {
  NfsClientOpenOptions options;
  options.address = "127.0.0.1";
  options.filename_encoding = encoding;
  auto client = co_await open_nfs_server_async(options, {});
  auto observed = create_activity_file_client(client, [](ActivityIndicatorId) {}, {});
  const auto roots = co_await observed->load_directory_async("/", {});
  std::string root, other, nested;
  for (const auto &entry : roots.entries) {
    require(entry.role == RemoteFileRole::nfs_export, "Exports must retain their UI role");
    require(!client->path_capabilities(entry.path).modify, "Export roots must be protected");
    if (entry.name == "/srv/public") root = entry.path;
    if (entry.name == "/srv/other/public") other = entry.path;
    if (entry.name == "/srv/public/deep") nested = entry.path;
  }
  require(!root.empty() && !other.empty() && !nested.empty() && root != other && root != nested,
      "Same-basename and nested exports must have distinct operation identities");
  const auto listing = co_await observed->load_directory_async(root, {});
  require(listing.entries.size() >= 8, "A real export must be browsable");
  require(!client->path_capabilities("/").create_children, "The server root is not a writable directory");

  char pattern[] = "/tmp/elder-nfs-client-XXXXXX";
  const char *temporary = mkdtemp(pattern);
  require(temporary != nullptr, "A local test directory must be available");
  const std::filesystem::path local(temporary);
  struct Cleanup { std::filesystem::path path; ~Cleanup() { std::filesystem::remove_all(path); } } cleanup{local};
  const std::string folder = "新規 %?#";
  const std::string name = "内容表.txt";
  const std::string content("binary\0\xff\n", 9);
  std::filesystem::create_directories(local / folder / "深い");
  { std::ofstream file(local / folder / "深い" / name, std::ios::binary); file.write(content.data(), content.size()); }
  { std::ofstream file(local / folder / "empty"); }
  std::filesystem::create_symlink("深い/" + name, local / folder / "link");
  FileTransferRequest send;
  send.direction = FileTransferDirection::send;
  send.source_paths = {(local / folder).string()};
  send.destination_directory = root;
  const auto sent = co_await run_file_transfer_async(observed, send, {});
  require(sent.outcome == FileTransferOutcome::completed && sent.succeeded_items == 5,
      "Recursive upload must complete including an empty file");
  const auto remote_folder = observed->child_path(root, folder);
  const auto remote_file = observed->child_path(observed->child_path(remote_folder, "深い"), name);
  require((co_await observed->lstat_async(remote_file, {}))->size == content.size(),
      "New encoded child paths must identify the uploaded bytes");
  const auto link_bytes = co_await observed->read_link_async(observed->child_path(remote_folder, "link"), {});
  require(link_bytes == encode_nfs_name("深い", encoding, 255) + "/" + encode_nfs_name(name, encoding, 255),
      "The remote link must address the uploaded encoded names, not the original UTF-8 bytes");

  std::filesystem::create_directory(local / "received");
  FileTransferRequest receive;
  receive.direction = FileTransferDirection::receive;
  receive.source_paths = {remote_folder};
  receive.destination_directory = (local / "received").string();
  const auto received = co_await run_file_transfer_async(observed, receive, {});
  require(received.outcome == FileTransferOutcome::completed && received.succeeded_items == 5,
      "Recursive receive must preserve Unicode names");
  std::ifstream downloaded(local / "received" / folder / "深い" / name, std::ios::binary);
  const std::string downloaded_content{std::istreambuf_iterator<char>(downloaded), {}};
  require(downloaded_content == content, "Filename conversion must not change file contents");
  require(std::filesystem::read_symlink(local / "received" / folder / "link") == "深い/" + name &&
      std::filesystem::equivalent(local / "received" / folder / "link", local / "received" / folder / "深い" / name),
      "Transferred symbolic links must point to the same copied file after filename conversion");

  FileTransferRequest undecodable;
  undecodable.direction = FileTransferDirection::receive;
  undecodable.destination_directory = (local / "received").string();
  for (const auto &entry : listing.entries)
    if (entry.name.starts_with("[undecodable:")) undecodable.source_paths.push_back(entry.path);
  undecodable.callbacks.failure = [](const FileTransferFailure &, cardio::cancellation) -> cardio::promise<FileTransferFailureAction> {
    co_return FileTransferFailureAction::skip;
  };
  require(!undecodable.source_paths.empty(), "The fixture must contain undecodable names");
  const auto skipped = co_await run_file_transfer_async(observed, undecodable, {});
  require(skipped.succeeded_items == 0 && skipped.skipped_items == undecodable.source_paths.size(),
      "Undecodable names must remain individually skippable without creating substitute local filenames");

  const auto renamed = observed->child_path(remote_folder, "変更表.txt");
  co_await rename_file_transfer_item_async(observed, FileTransferEndpoint::remote, remote_file, renamed, {});
  require(!(co_await observed->lstat_async(remote_file, {})) && (co_await observed->lstat_async(renamed, {})),
      "Rename must move the selected original item");
  bool rejected = false;
  try { co_await observed->rename_async(renamed, observed->child_path(other, name), {}); }
  catch (const std::exception &) { rejected = true; }
  require(rejected && (co_await observed->lstat_async(renamed, {})), "Cross-export rename must leave the source unchanged");
  rejected = false;
  try { co_await observed->remove_directory_async(root, {}); }
  catch (const std::exception &) { rejected = true; }
  require(rejected, "Export roots cannot be removed");
  if (encoding != "UTF-8") {
    rejected = false;
    try { (void)observed->child_path(root, "emoji-😀"); }
    catch (const std::exception &) { rejected = true; }
    require(rejected, "Unrepresentable new names must fail before remote creation");
  }
  std::vector<std::string> removals{remote_folder};
  co_await delete_file_transfer_items_async(observed, FileTransferEndpoint::remote, std::move(removals), {});
  require(!(co_await observed->lstat_async(remote_folder, {})), "Recursive deletion must use the original child identities");
  auto second = co_await open_nfs_server_async(options, {});
  rejected = false;
  try { (void)co_await second->load_directory_async(root, {}); }
  catch (const std::exception &) { rejected = true; }
  require(rejected, "Identifiers from an earlier connection must not address a new session");
  co_await stop_nfs_client_async(second);
  co_await stop_nfs_client_async(client);
  std::cout << "PASS NFS client 3 " << encoding << '\n';
}

int main(int argc, char **argv) {
  if (argc != 2) return 2;
  cardio::dispatcher_group_glib group;
  cardio::dispatcher_host_glib dispatcher(group);
  std::exception_ptr failure;
  auto body = [&]() -> cardio::promise<void> {
    try { co_await test_client(argv[1]); }
    catch (...) { failure = std::current_exception(); }
    group.shutdown();
  };
  auto operation = body();
  dispatcher.park();
  try { if (failure) std::rethrow_exception(failure); }
  catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
