// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "../../src/smb/smb1-client.h"
#include "../../src/file-transfer/file-transfer-engine.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <csignal>
#include <clocale>
#include <filesystem>

using namespace elder_terms;

static void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

static std::vector<int> children() {
  std::ifstream input("/proc/self/task/" + std::to_string(getpid()) + "/children");
  std::vector<int> result;
  for (int pid; input >> pid;) result.push_back(pid);
  return result;
}

static cardio::promise<void> input_byte() {
  (void)co_await cardio::from_fd(STDIN_FILENO, cardio::fd_event::read);
  char value;
  require(::read(STDIN_FILENO, &value, 1) == 1, "Expected fixture synchronization byte");
}

static cardio::promise<void> exercise_fault(const std::string &mode) {
  SmbClientOpenOptions options;
  options.connection.address = "127.0.0.1";
  options.connection.username = "elder";
  options.connection.require_signing = false;
  options.connection.version = "1";
  options.connection.idle_timeout_seconds = mode == "timeout" ? 1 : 60;
  options.password = std::getenv("ELDER_SMB_PASSWORD");
  auto client = co_await open_smb_server_async(options, {});
  auto reader = std::move(co_await client->open_read_async("/names/before.txt", {}));
  const auto owned = children();
  require(owned.size() == 1, "The client must own exactly one worker");
  std::cout << "READY fault" << std::endl;
  co_await input_byte();
  cardio::cancellation_source stopping;
  const auto trigger = [&]() -> cardio::promise<void> {
    co_await input_byte();
    if (mode == "worker-exit") require(kill(owned.front(), SIGKILL) == 0, "Could not terminate the owned worker");
    else stopping.cancel();
  };
  std::optional<cardio::promise<void>> interrupted;
  if (mode == "cancel" || mode == "worker-exit") interrupted.emplace(trigger());
  std::array<std::byte, 32> bytes{};
  bool failed = false;
  try { (void)co_await reader->read_async(bytes, stopping.get_cancellation()); }
  catch (const cardio::canceled_exception &) { failed = mode == "cancel"; }
  catch (const RemoteFileError &error) { failed = mode != "cancel" && error.connection_lost; }
  if (interrupted) co_await *interrupted;
  require(failed, "A withheld read must not succeed");
  co_await stop_smb_client_async(client);
  require(children().empty(), "The terminated worker must be reaped before stop returns");
  client = co_await open_smb_server_async(options, {});
  require((co_await client->load_directory_async("/empty", {})).entries.empty(), "Reconnect after failure must work");
  co_await stop_smb_client_async(client);
  require(children().empty(), "Reconnect must not leave a child process");
  std::cout << "PASS SMB1 fault " << mode << " reaped/reconnected" << std::endl;
}

static cardio::promise<void> exercise_upload_fault(const std::string &mode) {
  SmbClientOpenOptions options;
  options.connection.version = "1";
  options.connection.address = "127.0.0.1";
  options.connection.username = "elder";
  options.connection.require_signing = false;
  options.password = std::getenv("ELDER_SMB_PASSWORD");
  auto client = co_await open_smb_server_async(options, {});
  const std::string root = "/probe/" + mode;
  co_await client->make_directory_async(root, {}, {});
  auto original = std::move(co_await client->open_write_async(root + "/seed", 8, {}, {}));
  const std::string text = "original";
  co_await original->write_all_async(std::as_bytes(std::span(text)), {});
  co_await original->close_async({});
  co_await client->commit_upload_async(root + "/seed", root + "/destination.txt", false, {});
  char pattern[] = "/tmp/elder-smb1-upload-XXXXXX";
  require(mkdtemp(pattern) != nullptr, "Could not create the upload fixture");
  const auto local = std::filesystem::path(pattern);
  const auto source = local / "destination.txt";
  std::ofstream(source, std::ios::binary) << std::string(8 * 1024 * 1024 + 17, 'r');
  std::cout << "READY fault" << std::endl;
  co_await input_byte();
  cardio::cancellation_source cancellation;
  const auto cancel = [&]() -> cardio::promise<void> { co_await input_byte(); cancellation.cancel(); };
  std::optional<cardio::promise<void>> pending;
  const bool cancelling = mode.starts_with("cancel");
  if (cancelling) pending.emplace(cancel());
  FileTransferRequest request;
  request.direction = FileTransferDirection::send;
  request.source_paths.push_back(source.string());
  request.destination_directory = root;
  request.callbacks.conflict = [](const auto &, cardio::cancellation) -> cardio::promise<FileTransferConflictAction> {
    co_return FileTransferConflictAction::overwrite;
  };
  std::string temporary;
  try { (void)co_await run_file_transfer_async(client, std::move(request), cancellation.get_cancellation()); }
  catch (const FileTransferCleanupFailure &error) {
    temporary = error.temporary_path;
    require(error.cancelled == cancelling, "The cleanup result must retain cancellation");
    if (mode.ends_with("create"))
      require(std::string(error.what()).find("ownership is uncertain") != std::string::npos,
              "A lost creation response must retain uncertain ownership");
  }
  if (pending) co_await *pending;
  require(temporary.starts_with(root + "/destination.txt.elder-terms-part-"), "A failed upload must report its exact cleanup path");
  co_await stop_smb_client_async(client);
  require(children().empty(), "An interrupted upload must reap the worker");
  client = co_await open_smb_server_async(options, {});
  const bool committed = mode.ends_with("commit");
  const auto retained = committed ? temporary + "/previous" : root + "/destination.txt";
  auto reader = std::move(co_await client->open_read_async(retained, {}));
  std::array<std::byte, 32> bytes{};
  const auto count = co_await reader->read_async(bytes, {});
  require(count == 8 && std::string(reinterpret_cast<char *>(bytes.data()), count) == "original",
          "Interrupted replacement must retain original content");
  co_await reader->close_async({});
  require((co_await client->lstat_async(temporary, {})).has_value(), "Uncertain uploads must not be deleted automatically");
  if (committed) {
    require(!(co_await client->lstat_async(root + "/destination.txt", {})), "The retained backup must describe the interrupted rename");
    co_await client->rename_async(retained, root + "/destination.txt", {});
  }
  if (co_await client->lstat_async(temporary + "/content", {})) co_await client->remove_file_async(temporary + "/content", {});
  co_await client->remove_directory_async(temporary, {});
  co_await client->remove_file_async(root + "/destination.txt", {});
  co_await client->remove_directory_async(root, {});
  co_await stop_smb_client_async(client);
  std::filesystem::remove_all(local);
  std::cout << "PASS SMB1 fault " << mode << " retained/reaped/reconnected" << std::endl;
}

static cardio::promise<void> exercise_response(const std::string &mode) {
  SmbClientOpenOptions options;
  options.connection.version = "1";
  options.connection.address = "127.0.0.1";
  options.connection.username = "elder";
  options.connection.require_signing = false;
  options.password = std::getenv("ELDER_SMB_PASSWORD");
  if (mode == "connect-timeout") {
    options.connection.connect_timeout_seconds = 1;
    std::cout << "READY fault" << std::endl;
    co_await input_byte();
    bool timed_out = false;
    try { (void)co_await open_smb_server_async(options, {}); }
    catch (const RemoteFileError &error) { timed_out = error.connection_lost && std::string(error.what()).find("timed out") != std::string::npos; }
    require(timed_out, "An expired connection deadline must be reported as a timeout, not user cancellation");
    require(children().empty(), "Connection timeout must reap the worker");
    std::cout << "PASS SMB1 fault connect-timeout" << std::endl;
    co_return;
  }
  auto client = co_await open_smb_server_async(options, {});
  std::cout << "READY fault" << std::endl;
  co_await input_byte();
  if (mode == "partial-shares") {
    bool failed = false;
    try { (void)co_await client->load_directory_async("/", {}); }
    catch (const RemoteFileError &error) { failed = !error.connection_lost; }
    require(failed, "ERROR_MORE_DATA must not become a complete successful share listing");
    require((co_await client->load_directory_async("/empty", {})).entries.empty(),
            "Known shares must remain usable after an incomplete share listing");
  } else if (mode == "invalid-name") {
    bool failed = false;
    try { (void)co_await client->load_directory_async("/names", {}); }
    catch (const RemoteFileError &) { failed = true; }
    require(failed, "A malformed UTF-16 name must not become a partial successful listing");
    require((co_await client->load_directory_async("/names", {})).entries.size() == 3,
            "A rejected name must not alter the remote directory");
  } else {
    std::vector<std::byte> bytes(100000);
    for (std::size_t index = 0; index < bytes.size(); ++index) bytes[index] = static_cast<std::byte>(index % 251);
    auto writer = std::move(co_await client->open_write_async("/probe/short.tmp", bytes.size(), {}, {}));
    co_await writer->write_all_async(bytes, {});
    co_await writer->close_async({});
    co_await client->commit_upload_async("/probe/short.tmp", "/probe/short.txt", false, {});
    auto reader = std::move(co_await client->open_read_async("/probe/short.txt", {}));
    std::vector<std::byte> actual(bytes.size());
    for (std::size_t offset = 0; offset < actual.size();) {
      const auto size = co_await reader->read_async(std::span(actual).subspan(offset), {});
      require(size > 0, "A short write must not truncate the upload");
      offset += size;
    }
    require(bytes == actual, "A short write must preserve every byte in order");
    co_await reader->close_async({});
    co_await client->remove_file_async("/probe/short.txt", {});
  }
  co_await stop_smb_client_async(client);
  require(children().empty(), "Response tests must reap their worker");
  std::cout << "PASS SMB1 fault " << mode << std::endl;
}

static cardio::promise<void> exercise(const std::string &transport) {
  SmbClientOpenOptions options;
  options.connection.address = "127.0.0.1";
  options.connection.domain = "WORKGROUP";
  options.connection.port = transport == "NBT" ? 139 : 445;
  options.connection.netbios_name = "ELDERTEST";
  options.connection.username = "elder";
  options.connection.require_signing = false;
  options.password = std::getenv("ELDER_SMB_PASSWORD");
  options.connection.version = "1";
  std::string actual_version;
  options.negotiated = [&](std::string value) { actual_version = std::move(value); };
  const std::string parent_locale = std::setlocale(LC_ALL, nullptr);
  auto rejected_options = options;
  rejected_options.password += "invalid";
  bool rejected = false;
  try { (void)co_await open_smb_server_async(rejected_options, {}); }
  catch (const RemoteFileError &) { rejected = true; }
  require(rejected, "Incorrect credentials must fail authentication");
  require(children().empty(), "Failed authentication must reap its worker before returning");
  rejected_options.connection.username = "unknown-plan18-user";
  rejected = false;
  try { (void)co_await open_smb_server_async(rejected_options, {}); }
  catch (const RemoteFileError &error) { rejected = std::string(error.what()).find("guest") != std::string::npos; }
  require(rejected && children().empty(), "Guest mapping must not count as requested-user authentication");
  auto client = co_await open_smb_server_async(options, {});
  require(actual_version == "SMB 1", "SMB1 must report its actual protocol");
  if (std::getenv("ELDER_SMB_DENY_ENUMERATION")) {
    bool denied = false;
    try { (void)co_await client->load_directory_async("/", {}); }
    catch (const RemoteFileError &error) { denied = !error.connection_lost; }
    require(denied, "Share enumeration refusal must be an explicit recoverable error");
  } else if (const auto *bulk = std::getenv("ELDER_SMB_SHARE_COUNT")) {
    std::optional<RemoteDirectorySnapshot> listing;
    try { listing = co_await client->load_directory_async("/", {}); }
    catch (const RemoteFileError &error) { require(!error.connection_lost, "Incomplete enumeration should leave known shares usable"); }
    if (listing && listing->entries.size() != std::stoul(bulk) + 5)
      throw std::runtime_error("A fragmented share listing must be complete or explicitly rejected: got " +
                               std::to_string(listing->entries.size()) + " of " + std::to_string(std::stoul(bulk) + 5));
  } else {
    auto shares = co_await client->load_directory_async("/", {});
    require(std::any_of(shares.entries.begin(), shares.entries.end(), [](const auto &item) {
      return item.name == "日本語 space" && item.role == RemoteFileRole::smb_share;
    }), "The browser must expose Unicode shares");
  }
  const auto names = co_await client->load_directory_async("/names", {});
  require(names.entries.size() == 3, "Listing must retain every Unicode name");
  require((co_await client->load_directory_async("/empty", {})).entries.empty(), "Empty share must remain browsable");
  bool denied = false;
  try { (void)co_await client->load_directory_async("/denied", {}); }
  catch (const RemoteFileError &error) { denied = !error.connection_lost; }
  require(denied, "A denied share must report an operation error");
  (void)co_await client->load_directory_async("/probe", {});
  co_await client->make_directory_async("/probe/folder", std::nullopt, {});
  std::vector<std::byte> bytes(8 * 1024 * 1024 + 123);
  for (std::size_t index = 0; index < bytes.size(); ++index)
    bytes[index] = static_cast<std::byte>(index % 251);
  const std::string temporary = "/probe/folder/upload.tmp";
  const std::string destination = "/probe/folder/日本語😀.txt";
  auto writer = std::move(co_await client->open_write_async(temporary, bytes.size(), std::nullopt, {}));
  co_await writer->write_all_async(bytes, {});
  co_await writer->close_async({});
  require(writer->creation_state() == RemoteFileCreationState::created, "Upload must establish exclusive ownership");
  co_await client->commit_upload_async(temporary, destination, false, {});
  const auto info = co_await client->lstat_async(destination, {});
  require(info && info->size == bytes.size(), "Committed upload must have the complete size");
  auto reader = std::move(co_await client->open_read_async(destination, {}));
  std::vector<std::byte> actual(bytes.size());
  for (std::size_t offset = 0; offset < actual.size();) {
    const auto size = co_await reader->read_async(std::span(actual).subspan(offset), {});
    require(size > 0, "Read must not end before all bytes are received");
    offset += size;
  }
  co_await reader->close_async({});
  require(actual == bytes, "Upload and download content must match");
  auto replacement = std::move(co_await client->open_write_async(temporary, 0, std::nullopt, {}));
  co_await replacement->close_async({});
  bool collision = false;
  try { co_await client->commit_upload_async(temporary, destination, false, {}); }
  catch (const RemoteFileError &error) { collision = !error.connection_lost; }
  require(collision, "A no-replace commit must preserve an existing destination");
  require((co_await client->lstat_async(destination, {}))->size == bytes.size(), "No-replace must retain destination bytes");
  co_await client->commit_upload_async(temporary, destination, true, {});
  require((co_await client->lstat_async(destination, {}))->size == 0, "Replacement must commit an empty upload");
  bool protected_root = false;
  try { co_await client->remove_directory_async("/probe", {}); }
  catch (const RemoteFileError &error) { protected_root = !error.connection_lost; }
  require(protected_root, "A share root must not be removed");
  co_await client->rename_async(destination, "/probe/folder/renamed.txt", {});
  co_await client->remove_file_async("/probe/folder/renamed.txt", {});
  co_await client->remove_directory_async("/probe/folder", {});
  require(!(co_await client->lstat_async("/probe/folder", {})), "Deleted directory must be absent");
  co_await stop_smb_client_async(client);
  require(children().empty(), "Normal stop must reap its worker");
  require(parent_locale == std::setlocale(LC_ALL, nullptr), "The parent locale must remain unchanged");
  client = co_await open_smb_server_async(options, {});
  require((co_await client->load_directory_async("/empty", {})).entries.empty(), "Reconnect must create a usable worker");
  co_await stop_smb_client_async(client);
  std::cout << "PASS SMB1 client" << std::endl;
}

int main(int argc, char **argv) {
  std::setlocale(LC_ALL, "");
  cardio::dispatcher_group_glib group;
  cardio::dispatcher_host_glib_auto dispatcher(group);
  int status = 0;
  const auto run = [&]() -> cardio::promise<void> {
    try {
      if (argc == 2 && (std::string_view(argv[1]) == "short-write" || std::string_view(argv[1]) == "invalid-name" || std::string_view(argv[1]) == "connect-timeout" || std::string_view(argv[1]) == "partial-shares"))
        co_await exercise_response(argv[1]);
      else if (argc == 2 && std::string_view(argv[1]).find('-') != std::string_view::npos && std::string_view(argv[1]) != "worker-exit")
        co_await exercise_upload_fault(argv[1]);
      else if (argc == 2) co_await exercise_fault(argv[1]);
      else co_await exercise(argc > 2 ? argv[2] : "TCP");
    }
    catch (const std::exception &error) { std::cerr << error.what() << std::endl; status = 1; }
    group.shutdown();
  };
  auto pending = run();
  dispatcher.park();
  return status;
}
