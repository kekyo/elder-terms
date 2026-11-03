// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "../../src/smb/smb1-client.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <csignal>
#include <clocale>

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
  auto client = co_await open_smb_server_async(options, {});
  require(actual_version == "SMB 1", "SMB1 must report its actual protocol");
  if (std::getenv("ELDER_SMB_DENY_ENUMERATION")) {
    bool denied = false;
    try { (void)co_await client->load_directory_async("/", {}); }
    catch (const RemoteFileError &error) { denied = !error.connection_lost; }
    require(denied, "Share enumeration refusal must be an explicit recoverable error");
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
  std::vector<std::byte> bytes(150000);
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
      if (argc == 2) co_await exercise_fault(argv[1]);
      else co_await exercise(argc > 2 ? argv[2] : "TCP");
    }
    catch (const std::exception &error) { std::cerr << error.what() << std::endl; status = 1; }
    group.shutdown();
  };
  auto pending = run();
  dispatcher.park();
  return status;
}
