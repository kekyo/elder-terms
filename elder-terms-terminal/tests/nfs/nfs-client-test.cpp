// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "../../src/nfs/nfs-client.h"
#include "../../src/nfs/nfs-name.h"
#include "../../src/file-transfer/file-transfer-engine.h"
#include "../../src/file-transfer/activity-file-client.h"
#include <filesystem>
#include <array>
#include <fstream>
#include <iostream>
#include <iterator>
#include <unistd.h>

using namespace elder_terms;

static void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

static cardio::promise<void> test_client(const std::string &encoding, int version) {
  const std::string prefix = version == 4 ? "" : "/srv";
  if (encoding == "UTF-8") {
    NfsClientOpenOptions unresolved;
    unresolved.address = "elder-nfs-unresolvable.invalid";
    unresolved.known_exports = {prefix + "/public"};
    unresolved.discover_exports = false;
    bool failed = false;
    try { (void)co_await open_nfs_server_async(unresolved, {}); }
    catch (const std::exception &) { failed = true; }
    require(failed, "An unresolvable hostname must fail before publishing an available server");
  }
  NfsClientOpenOptions options;
  options.address = "127.0.0.1";
  options.version = version;
  options.filename_encoding = encoding;
  if (version == 4) {
    options.known_namespace_paths = {"/other/public", "/public/deep"};
    options.known_exports = {"/must-not-be-interpreted-as-v4"};
  }
  auto client = co_await open_nfs_server_async(options, {});
  auto observed = create_activity_file_client(client, [](ActivityIndicatorId) {}, {});
  const auto roots = co_await observed->load_directory_async("/", {});
  if (version == 4) {
    require(roots.notice.find("namespace") != std::string::npos,
        "The v4 browser must identify namespace discovery rather than claim a complete export list");
    require(std::none_of(roots.entries.begin(), roots.entries.end(), [](const auto &entry) {
      return entry.name == "/must-not-be-interpreted-as-v4";
    }), "v3 export settings must not leak into the v4 namespace");
  }
  std::string root, other, nested;
  for (const auto &entry : roots.entries) {
    require(entry.role == RemoteFileRole::nfs_export, "Exports must retain their UI role");
    require(!client->path_capabilities(entry.path).modify, "Export roots must be protected");
    if (entry.name == prefix + "/public") root = entry.path;
    if (entry.name == prefix + "/other/public") other = entry.path;
    if (entry.name == prefix + "/public/deep") nested = entry.path;
  }
  require(!root.empty() && !other.empty() && !nested.empty() && root != other && root != nested,
      "Same-basename and nested exports must have distinct operation identities");
  require(observed->parent_path(root) == "/" && observed->parent_path("/") == "/" &&
      observed->display_path(root) == prefix + "/public:/" && !observed->path_input_editable(),
      "Browser navigation must use identities while displaying the export path");
  require(nfs_export_identity(client, prefix + "/public") == root,
      "An explicitly configured export must resolve to this connection's identity");
  const auto listing = co_await observed->load_directory_async(root, {});
  require(listing.entries.size() >= 8, "A real export must be browsable");
  require(!client->path_capabilities("/").create_children, "The server root is not a writable directory");
  require(!client->path_capabilities(client->child_path(root, "deep")).modify,
      "A nested public root must remain protected when reached through its parent export");

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
  require(observed->parent_path(remote_folder) == root &&
      observed->display_path(remote_file) == prefix + "/public:/" + folder + "/深い/" + name,
      "Display paths must decode every component without becoming operation identities");
  require((co_await observed->lstat_async(remote_file, {}))->size == content.size(),
      "New encoded child paths must identify the uploaded bytes");
  const auto link_bytes = co_await observed->read_link_async(observed->child_path(remote_folder, "link"), {});
  const auto expected_link = encode_nfs_name("深い", encoding, 255) + "/" + encode_nfs_name(name, encoding, 255);
  if (link_bytes != expected_link)
    throw std::runtime_error("NFS link bytes differ: " + nfs_identifier_component(link_bytes) +
        "; expected " + nfs_identifier_component(expected_link));

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
  options.address = "localhost";
  options.known_exports = {prefix + "/public"};
  options.discover_exports = false;
  options.known_namespace_paths = {"/public"};
  options.discover_namespace = false;
  auto second = co_await open_nfs_server_async(options, {});
  rejected = false;
  try { (void)co_await second->load_directory_async(root, {}); }
  catch (const std::exception &) { rejected = true; }
  require(rejected, "Identifiers from an earlier connection must not address a new session");
  require(!(co_await second->load_directory_async(nfs_export_identity(second, prefix + "/public"), {})).entries.empty(),
      "Known exports must work through asynchronous hostname resolution without enumeration");
  co_await stop_nfs_client_async(second);
  co_await stop_nfs_client_async(client);
  std::cout << "PASS NFS client " << version << " " << encoding << '\n';
}

static cardio::promise<void> test_known_exports(int version) {
  const std::string prefix = version == 4 ? "" : "/srv";
  NfsClientOpenOptions options;
  options.version = version;
  options.address = "127.0.0.1";
  options.uid = options.gid = 0;
  options.known_exports = {prefix + "/public", prefix + "/other/public", prefix + "/restricted", prefix + "/readonly"};
  if (version == 4) {
    options.known_namespace_paths = options.known_exports;
    options.discover_namespace = true;
  }
  auto client = co_await open_nfs_server_async(options, {});
  const auto root = co_await client->load_directory_async("/", {});
  require(root.notice.find("failed") != std::string::npos && root.entries.size() == 4,
      "Failed export enumeration must show configured roots with a visible explanation");
  const auto restricted = nfs_export_identity(client, prefix + "/restricted");
  bool denied = false;
  try { (void)co_await client->load_directory_async(restricted, {}); }
  catch (const RemoteFileError &error) {
    denied = !error.connection_lost;
    if (!denied) std::cerr << "Restricted export error: " << error.what() << '\n';
  }
  require(denied, "AUTH_SYS root squashing must remain an export-specific permission failure");
  const auto public_path = nfs_export_identity(client, prefix + "/public");
  require(!(co_await client->load_directory_async(public_path, {})).entries.empty(),
      "An inaccessible export must not prevent browsing another known export");
  const auto readonly = client->child_path(nfs_export_identity(client, prefix + "/readonly"), "must-not-create");
  denied = false;
  try { auto writer = std::move(co_await client->open_write_async(readonly, 0, std::nullopt, {})); co_await writer->close_async({}); }
  catch (const RemoteFileError &error) { denied = !error.connection_lost; }
  require(denied && !(co_await client->lstat_async(readonly, {})), "Read-only export errors must not create a file or drop the server");
  auto reader = std::move(co_await client->open_read_async(client->child_path(public_path, "plain.txt"), {}));
  const auto temporary = client->child_path(public_path, "abandoned-upload");
  auto writer = std::move(co_await client->open_write_async(temporary, 100, std::nullopt, {}));
  const std::array<std::byte, 3> bytes{std::byte{1}, std::byte{0}, std::byte{255}};
  co_await writer->write_all_async(bytes, {});
  writer.reset();
  co_await reader->close_async({});
  co_await stop_nfs_client_async(client);
  client = co_await open_nfs_server_async(options, {});
  co_await client->remove_file_async(client->child_path(nfs_export_identity(client, prefix + "/public"), "abandoned-upload"), {});
  co_await stop_nfs_client_async(client);
  std::cout << "PASS NFS configured exports, access errors, and abandoned handles\n";
}

static cardio::promise<void> test_lost_create(int version) {
  NfsClientOpenOptions options;
  options.address = "127.0.0.1";
  options.version = version;
  options.known_exports = {"/srv/public"};
  options.known_namespace_paths = {"/public"};
  options.discover_exports = options.discover_namespace = false;
  const auto public_path = version == 4 ? "/public" : "/srv/public";
  auto client = co_await open_nfs_server_async(options, {});
  const auto root = nfs_export_identity(client, public_path);
  char pattern[] = "/tmp/elder-nfs-create-XXXXXX";
  const auto *directory = mkdtemp(pattern);
  require(directory != nullptr, "A local test directory must be available");
  const std::filesystem::path local(directory);
  struct Cleanup { std::filesystem::path path; ~Cleanup() { std::filesystem::remove_all(path); } } cleanup{local};
  { std::ofstream file(local / "uncertain-create"); file << "must not report success"; }
  FileTransferResult result;
  FileTransferRequest send;
  send.source_paths = {(local / "uncertain-create").string()};
  send.destination_directory = root;
  send.callbacks.finished = [&](const FileTransferResult &finished) { result = finished; };
  bool failed = false;
  try { (void)co_await run_file_transfer_async(client, send, {}); }
  catch (const std::exception &) { failed = true; }
  require(failed && result.outcome == FileTransferOutcome::failed && result.temporary_paths.size() == 1,
      "A lost CREATE reply must report the uncertain temporary file for inspection");
  const auto temporary = nfs_identifier_bytes(result.temporary_paths.front().substr(
      result.temporary_paths.front().find_last_of('/') + 1));
  co_await stop_nfs_client_async(client);
  client = co_await open_nfs_server_async(options, {});
  const auto new_root = nfs_export_identity(client, public_path);
  require(!(co_await client->lstat_async(client->child_path(new_root, "uncertain-create"), {})),
      "An uncertain upload must never be committed");
  const auto left = client->child_path(new_root, temporary);
  require((co_await client->lstat_async(left, {})).has_value(),
      "Unknown ownership must not trigger deletion or CREATE replay");
  co_await client->remove_file_async(left, {});
  co_await stop_nfs_client_async(client);
  std::cout << "PASS lost CREATE reply retained for inspection\n";
}

static cardio::promise<void> test_v4_name_rejection() {
  NfsClientOpenOptions options;
  options.address = "127.0.0.1";
  options.version = 4;
  options.filename_encoding = "CP932";
  options.known_namespace_paths = {"/public"};
  options.discover_namespace = false;
  auto client = co_await open_nfs_server_async(options, {});
  const auto root = nfs_export_identity(client, "/public");
  bool rejected = false;
  try { co_await client->make_directory_async(client->child_path(root, "拒否表"), 0700, {}); }
  catch (const RemoteFileError &error) { rejected = !error.connection_lost; }
  require(rejected, "A v4 server's filename rejection must be returned as an operation failure");
  require(!(co_await client->lstat_async(client->child_path(root, "拒否表"), {})),
      "Rejected names must not be silently retried in another encoding");
  co_await stop_nfs_client_async(client);
  std::cout << "PASS v4 filename rejection\n";
}

int main(int argc, char **argv) {
  if (argc < 2 || argc > 3) return 2;
  const int version = argc == 3 ? std::stoi(argv[2]) : 3;
  cardio::dispatcher_group_glib group;
  cardio::dispatcher_host_glib dispatcher(group);
  std::exception_ptr failure;
  auto body = [&]() -> cardio::promise<void> {
    try {
      if (std::string_view(argv[1]) == "--known-exports") co_await test_known_exports(version);
      else if (std::string_view(argv[1]) == "--lost-create") co_await test_lost_create(version);
      else if (std::string_view(argv[1]) == "--reject-name") co_await test_v4_name_rejection();
      else co_await test_client(argv[1], version);
    }
    catch (...) { failure = std::current_exception(); }
    group.shutdown();
  };
  auto operation = body();
  dispatcher.park();
  try { if (failure) std::rethrow_exception(failure); }
  catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
