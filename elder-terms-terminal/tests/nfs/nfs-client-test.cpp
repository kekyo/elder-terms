// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "../../src/nfs/nfs-client.h"
#include "../../src/nfs/nfs-name.h"
#include "../../src/nfs/nfs-rpc.h"
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

static cardio::promise<void> test_v2_receive(const std::string &encoding) {
  NfsClientOpenOptions options;
  options.address = "127.0.0.1";
  options.version = 2;
  options.filename_encoding = encoding;
  auto client = co_await open_nfs_server_async(options, {});
  require(client->capabilities().write_operations && client->capabilities().upload_commit,
      "v2 must advertise verified writes and conditional upload commits");
  const auto root = nfs_export_identity(client, "/srv/public");
  const auto entries = co_await client->load_directory_async(root, {});
  require(entries.entries.size() >= 8, "NFS v2 must list real export contents");
  std::array<std::byte, 3> bytes{};
  bool raw_found = false, japanese_found = false;
  for (const auto &entry : entries.entries) {
    if (entry.type != RemoteFileType::regular) continue;
    auto reader = std::move(co_await client->open_read_async(entry.path, {}));
    std::string received;
    for (;;) {
      const auto count = co_await reader->read_async(bytes, {});
      if (count == 0) break;
      received.append(reinterpret_cast<const char *>(bytes.data()), count);
    }
    co_await reader->close_async({});
    if (entry.name == "[undecodable: FF]") { raw_found = true; require(received == "raw\n", "Undecodable original names must remain readable"); }
    if (entry.name == "日本語表") {
      japanese_found = true;
      const auto expected = encoding == "UTF-8" ? "UTF-8\n" : encoding == "EUC-JP" ? "EUC-JP\n" : "CP932\n";
      require(received == expected, "v2 reads must address the selected original encoded bytes");
    }
    if (entry.name == "zero") require(received.empty(), "A zero-byte v2 file must reach EOF without invented bytes");
  }
  require(raw_found && japanese_found, "v2 listing must retain all encoded and undecodable entries");
  char pattern[] = "/tmp/elder-nfs2-receive-XXXXXX";
  const char *directory = mkdtemp(pattern);
  require(directory != nullptr, "A local directory must be available");
  const std::filesystem::path local(directory);
  struct Cleanup { std::filesystem::path path; ~Cleanup() { std::filesystem::remove_all(path); } } cleanup{local};
  FileTransferRequest request;
  request.direction = FileTransferDirection::receive;
  request.source_paths = {client->child_path(root, "deep")};
  request.destination_directory = local.string();
  const auto result = co_await run_file_transfer_async(client, request, {});
  require(result.outcome == FileTransferOutcome::completed && result.succeeded_items == 4,
      "v2 recursive receive must copy directories and a cyclic link without following it");
  std::ifstream file(local / "deep" / "inside" / "payload.txt");
  require(std::string{std::istreambuf_iterator<char>(file), {}} == "nested\n" &&
      std::filesystem::read_symlink(local / "deep" / "cycle") == "..", "v2 recursive data and link targets must remain exact");
  co_await stop_nfs_client_async(client);
  std::cout << "PASS NFS client 2 " << encoding << " recursive receive\n";
}

static cardio::promise<void> test_v2_upload_guards() {
  NfsClientOpenOptions options;
  options.address = "127.0.0.1";
  options.version = 2;
  options.known_exports = {"/srv/public"};
  options.discover_exports = false;
  auto client = co_await open_nfs_server_async(options, {});
  const auto root = nfs_export_identity(client, "/srv/public");
  const auto existing = client->child_path(root, "plain.txt");
  const auto before = co_await client->lstat_async(existing, {});
  auto foreign = std::move(co_await client->open_write_async(existing, 0, std::nullopt, {}));
  bool rejected = false;
  try { co_await foreign->close_async({}); }
  catch (const RemoteFileError &) { rejected = true; }
  require(rejected && foreign->creation_state() == RemoteFileCreationState::not_created &&
      (co_await client->lstat_async(existing, {}))->size == before->size,
      "A colliding v2 upload must not claim or truncate the existing file");
  foreign.reset();
  foreign = std::move(co_await client->open_write_async(existing, 0, std::nullopt, {}));
  rejected = false;
  try { co_await foreign->close_async({}); }
  catch (const RemoteFileError &) { rejected = true; }
  require(rejected && foreign->creation_state() == RemoteFileCreationState::not_created,
      "Retrying a rejected creation must reach the server without retaining a false ownership claim");
  foreign.reset();
  const auto temporary = client->child_path(root, ".elder-terms-part-guard");
  for (const std::uint64_t size : {UINT64_C(4294967295), UINT64_C(4294967296)}) {
    rejected = false;
    try { (void)co_await client->open_write_async(temporary, size, std::nullopt, {}); }
    catch (const RemoteFileError &) { rejected = true; }
    require(rejected && !(co_await client->lstat_async(temporary, {})),
        "An unrepresentable v2 upload size must fail before any creation");
  }
  std::vector<std::byte> bytes(25000);
  for (std::size_t index = 0; index < bytes.size(); ++index) bytes[index] = std::byte(index % 251);
  auto writer = std::move(co_await client->open_write_async(temporary, bytes.size(), 0440, {}));
  require(writer->creation_state() == RemoteFileCreationState::not_created,
      "Preparing a writer must not claim a remote path before creating it");
  co_await writer->write_all_async(bytes, {});
  co_await writer->close_async({});
  require(writer->creation_state() == RemoteFileCreationState::created, "Successful v2 writes must prove temporary ownership");
  rejected = false;
  try { co_await client->commit_upload_async(temporary, existing, false, {}); }
  catch (const RemoteFileError &) { rejected = true; }
  require(rejected && (co_await client->lstat_async(existing, {}))->size == before->size,
      "A conditional v2 commit must never replace an existing destination");
  const auto destination = client->child_path(root, "v2-commit-guard");
  co_await client->commit_upload_async(temporary, destination, false, {});
  require(!(co_await client->lstat_async(temporary, {})), "A successful commit must remove its temporary upload resources");
  require(((co_await client->lstat_async(destination, {}))->permissions.value() & 0777) == 0440,
      "An upload must preserve a read-only source mode after finishing writes");
  auto reader = std::move(co_await client->open_read_async(destination, {}));
  std::vector<std::byte> received;
  std::array<std::byte, 4096> buffer{};
  for (;;) {
    const auto count = co_await reader->read_async(buffer, {});
    if (!count) break;
    received.insert(received.end(), buffer.begin(), buffer.begin() + count);
  }
  co_await reader->close_async({});
  require(received == bytes, "Multiple v2 WRITE requests must preserve the exact binary content");
  writer = std::move(co_await client->open_write_async(temporary, 0, std::nullopt, {}));
  co_await writer->close_async({});
  co_await client->commit_upload_async(temporary, destination, true, {});
  require((co_await client->lstat_async(destination, {}))->size == 0 && !(co_await client->lstat_async(temporary, {})),
      "An authorized replacement must atomically install the completed empty file and clean up");
  co_await client->remove_file_async(destination, {});
  co_await stop_nfs_client_async(client);
  std::cout << "PASS v2 upload ownership and commit guards\n";
}

static cardio::promise<void> test_client(const std::string &encoding, int version) {
  if (version == 2) co_await test_v2_receive(encoding);
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
  options.uid = options.gid = 1000;
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
  std::filesystem::permissions(local / folder / "深い" / name, std::filesystem::perms::owner_read);
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
  require(((co_await observed->lstat_async(remote_file, {}))->permissions.value() & 0777) == 0400,
      "Uploading a read-only source must preserve its mode after writing its complete contents");
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

static cardio::promise<void> test_v2_write_failure(const std::string &mode) {
  NfsClientOpenOptions options;
  options.address = "127.0.0.1";
  options.version = 2;
  options.known_exports = {"/srv/public"};
  options.discover_exports = false;
  auto client = co_await open_nfs_server_async(options, {});
  const auto root = nfs_export_identity(client, "/srv/public");
  char pattern[] = "/tmp/elder-nfs2-failure-XXXXXX";
  const char *directory = mkdtemp(pattern);
  require(directory != nullptr, "A local directory must be available");
  const std::filesystem::path local(directory);
  struct Cleanup { std::filesystem::path path; ~Cleanup() { std::filesystem::remove_all(path); } } cleanup{local};
  const std::string name = "failed-v2-upload";
  { std::ofstream file(local / name); file << std::string(300000, 'x'); }
  FileTransferResult result;
  FileTransferRequest request;
  request.source_paths = {(local / name).string()};
  request.destination_directory = root;
  cardio::cancellation_source stopping;
  request.callbacks.progress = [&](const FileTransferProgress &progress) {
    if (mode == "cancel" && progress.transferred_bytes > 0) (void)stopping.cancel();
  };
  request.callbacks.finished = [&](const FileTransferResult &value) { result = value; };
  bool failed = false;
  try { (void)co_await run_file_transfer_async(client, request, stopping.get_cancellation()); }
  catch (const std::exception &) { failed = true; }
  require(failed && result.succeeded_items == 0 && result.outcome ==
      (mode == "cancel" ? FileTransferOutcome::cancelled : FileTransferOutcome::failed),
      "Interrupted v2 writes must publish failure or cancellation after cleanup, never completion");
  require(result.temporary_paths.size() == (mode == "disconnect" ? 1U : 0U),
      "Only failed cleanup after a lost connection should retain the owned temporary path");
  if (mode == "nospace") {
    require(!result.failures.empty() && result.failures.front().message.find("NOSPC") != std::string::npos,
        "Capacity failures must retain the server's NFS error");
  }
  co_await stop_nfs_client_async(client);
  client = co_await open_nfs_server_async(options, {});
  const auto new_root = nfs_export_identity(client, "/srv/public");
  require(!(co_await client->lstat_async(client->child_path(new_root, name), {})),
      "An incomplete upload must not appear at the final destination");
  const auto listing = co_await client->load_directory_async(new_root, {});
  std::vector<std::string> leftovers;
  for (const auto &entry : listing.entries)
    if (entry.name.starts_with(".elder-terms-part-")) leftovers.push_back(entry.path);
  require(leftovers.size() == result.temporary_paths.size(), "Reported temporary resources must agree with the real server");
  if (!leftovers.empty()) co_await delete_file_transfer_items_async(client, FileTransferEndpoint::remote, std::move(leftovers), {});
  require((co_await client->lstat_async(client->child_path(new_root, "plain.txt"), {}))->size.value_or(0) > 0,
      "Failed upload cleanup must leave unrelated files intact");
  co_await stop_nfs_client_async(client);
  std::cout << "PASS v2 interrupted write " << mode << '\n';
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
  std::vector<std::string> abandoned{client->child_path(nfs_export_identity(client, prefix + "/public"), "abandoned-upload")};
  co_await delete_file_transfer_items_async(client, FileTransferEndpoint::remote, std::move(abandoned), {});
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
  if (version == 2) co_await client->remove_directory_async(left, {});
  else co_await client->remove_file_async(left, {});
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

static cardio::promise<void> test_v2_boundaries(bool repeated_cookie) {
  NfsClientOpenOptions options;
  options.address = "127.0.0.1";
  options.version = 2;
  options.known_exports = {"/srv/public"};
  options.discover_exports = false;
  auto client = co_await open_nfs_server_async(options, {});
  const auto root = nfs_export_identity(client, "/srv/public");
  if (repeated_cookie) {
    bool rejected = false;
    try { (void)co_await client->load_directory_async(root, {}); }
    catch (const RemoteFileError &error) { rejected = !error.connection_lost && std::string(error.what()).find("cookie") != std::string::npos; }
    require(rejected, "A repeated v2 directory cookie must stop instead of looping or claiming completion");
    require(!(co_await client->load_directory_async(root, {})).entries.empty(), "A fresh listing must remain possible after a cookie error");
  } else {
    const auto pages = co_await client->load_directory_async(client->child_path(root, "pages"), {});
    require(pages.entries.size() == 600, "v2 directory pagination must retain every entry");
    for (const auto &name : {"at-limit", "too-large"}) {
      const auto path = client->child_path(client->child_path(root, "limits"), name);
      bool rejected = false;
      try {
        const auto attributes = co_await client->lstat_async(path, {});
        if (attributes) std::cout << "v2 boundary " << name << " size=" << attributes->size.value_or(0) << '\n';
        auto reader = std::move(co_await client->open_read_async(path, {}));
        std::array<std::byte, 3> bytes{};
        (void)co_await reader->read_async(bytes, {});
        co_await reader->close_async({});
      } catch (const RemoteFileError &error) { rejected = !error.connection_lost; }
      require(rejected, "v2 files at or above its ambiguous 32-bit size boundary must be refused");
    }
  }
  co_await stop_nfs_client_async(client);
  std::cout << "PASS v2 directory and size boundary\n";
}

int main(int argc, char **argv) {
  if (argc < 2 || argc > 3) return 2;
  const int version = argc == 3 ? std::stoi(argv[2]) : 3;
  cardio::dispatcher_group_glib group;
  cardio::dispatcher_host_glib dispatcher(group);
  std::exception_ptr failure;
  auto body = [&]() -> cardio::promise<void> {
    try {
      bool valid_error = false;
      try { check_nfs_result({-EACCES, std::string("Access denied: /") + char(0xff)}, "lookup"); }
      catch (const RemoteFileError &error) {
        valid_error = !error.connection_lost && g_utf8_validate(error.what(), -1, nullptr) &&
            std::string(error.what()).find("Access denied") != std::string::npos;
      }
      require(valid_error, "Library errors containing raw filenames must remain readable UTF-8 for GTK");
      if (std::string_view(argv[1]) == "--known-exports") co_await test_known_exports(version);
      else if (std::string_view(argv[1]) == "--lost-create") co_await test_lost_create(version);
      else if (std::string_view(argv[1]) == "--reject-name") co_await test_v4_name_rejection();
      else if (std::string_view(argv[1]) == "--v2-cookie") co_await test_v2_boundaries(true);
      else if (std::string_view(argv[1]) == "--v2-boundaries") co_await test_v2_boundaries(false);
      else if (std::string_view(argv[1]) == "--v2-upload-guards") co_await test_v2_upload_guards();
      else if (std::string_view(argv[1]).starts_with("--v2-write-")) co_await test_v2_write_failure(std::string(argv[1]).substr(11));
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
